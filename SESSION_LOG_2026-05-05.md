# Session 2026-05-05 — gate 9 dispatch hunt: phase 0 done, phase 1.B doesn't crack it

Plan: in-message plan from PLAN_2026-05-05 ("close the FEC RX IRQ
loop, wake tFecEndRx"). Phase 0 diagnostics + Phase 1 dispatch fix
attempt. Phase 0 found facts; Phase 1.B (edge re-trigger) did NOT
fix dispatch — reverted.

## TL;DR

- ✅ **Phase 0 diagnostics landed.** Boot station for SDMA Main ISR
  (0x132854) plus per-offset IC MMIO write logging at MBAR+0x500..
  0x52c. Both reusable for future debug sessions.
- ✅ **Phase 0 ruled out the mask hypothesis.** BSP writes MainMask
  (0x510) and PerMask (0x500), but final values are 0 (= all
  enabled). Mask isn't blocking SDMA dispatch.
- ✅ **Phase 0 confirmed the dispatch break.** `SDMA eval RAISE`
  fires (1× per run after force-arm), `ic_sdma_pending` stays true
  for the rest of the run, but **0 hits at SDMA Main ISR (0x132854)
  and 0 reads of `0x524 => 0x20000000 (SDMA)`**. Same as 2026-05-04
  symptom. EXT line is at 1 (level), CPU just doesn't take the
  exception.
- ❌ **Phase 1.B (edge re-trigger) did not fix dispatch.** Tried
  `ppc_set_irq(EXT, 0); eval_irq;` to force a fresh 0→1 edge inside
  the RX hook. Built, ran, no STATION HIT 0x132854, no SDMA reads,
  no FTP banner. Reverted.
- ❌ **Gate 9 unchanged.** TCP handshake works, byte-level comms
  doesn't.

## Diagnostic findings

### Phase 0.1 — SDMA Main ISR boot station

Added `{0x00132854, 0x00132857, "VX: SDMA Main ISR entry"}` to
`g_boot_stations[]`. After 90s of guest run the station reports
`UNREACHED`. The BSP never executes the SDMA Main ISR.

### Phase 0.3 — IC MMIO write logging at 0x500..0x52c

Added per-offset logging (first 64 writes per offset). Output
shows what the BSP actually touches:

```
   64  W +0x500 (PerMask)        ← repeatedly written, ends at 0
   25  W +0x504 (PerPri+Main)
   24  W +0x510 (MainMask)       ← ends at 0 = all enabled
   21  W +0x508 (MainPri1)
   21  W +0x50c (MainPri2)
    8  W +0x514 (MainEnStat)
    1  W +0x51c
    1  W +0x518
    0  W +0x520 (PerEnable)
    0  W +0x524 (PerStatEnc)
```

Final mask state — both PerMask (0x500) and MainMask (0x510) are
zero (= all unmasked) before any RX traffic. So the BSP isn't
gating SDMA via a mask register. Phase 1.A (model the mask) is
unnecessary.

### Phase 0 → decision tree outcome

Plan said:
- STATION HIT 0x132854 fires → Phase 2 (sem-post).
- No hit + relevant write to 0x510/0x520 → Phase 1.A.
- No hit + no relevant write → Phase 1.B (edge re-trigger).

We landed in the third bucket — **dispatch is broken at a deeper
level than masks**.

### Phase 1.B attempt — edge re-trigger

`ppc_set_irq` is edge-tracking inside QEMU
(`hw/ppc/ppc.c:60` — `if (old_pending != env->pending_interrupts)`):

```c
old_pending = env->pending_interrupts;
if (level) env->pending_interrupts |= irq;
else       env->pending_interrupts &= ~irq;
if (old_pending != env->pending_interrupts) {
    ppc_maybe_interrupt(env);
}
```

So calling `ppc_set_irq(EXT, 1)` when `EXT` bit is already set in
`pending_interrupts` is a **no-op** — `ppc_maybe_interrupt` is
NOT invoked, and `cpu_interrupt(CPU_INTERRUPT_HARD)` is NOT
re-asserted. Hypothesis: ic_sdma_pending stays true (level held
high), so each subsequent re-call to `update_ext` does nothing
new — the CPU never re-checks the interrupt request.

Fix attempt: in the RX hook, BEFORE calling `eval_irq`, drop EXT
to 0 explicitly. Then eval_irq's `update_ext` raises EXT to 1.
That's a real 1→0→1 edge.

```c
ppc_set_irq(s->cpu, PPC_INTERRUPT_EXT, 0);
mpc5200_sdma_eval_irq(s);
```

**Result: didn't help.** Two test runs, both showed:
- 11 force-arming events (frames delivered to DRAM)
- 1 SDMA RAISE event (initial transition to true)
- 0 STATION HIT 0x132854
- 0 0x524 SDMA reads
- BSP never enters EXT for SDMA

Reverted.

### Why Phase 1.B didn't work — open question

Two plausible reasons we couldn't separate this session:

1. **MSR[EE]=0 at the time of the pulse.** The RX hook runs in
   QEMU's IO thread under BQL. If the CPU thread is currently in
   a code section with EE=0 (e.g. inside a brief critical
   section), `ppc_maybe_interrupt` finds no unmasked-pending
   interrupt and clears `CPU_INTERRUPT_HARD`. The next time MSR[EE]
   becomes 1 (mtmsr or rfi), `helper_regs.c` re-runs
   `ppc_maybe_interrupt` — which should re-arm HARD — but maybe
   the BSP never executes that path while the line is held.
2. **Per-source enable register we still don't model.** Despite
   the plan ruling out masks, the MPC5200 IC has more registers
   we treat as plain RAM (e.g. PerEnable 0x520 wasn't written, but
   maybe other state we're ignoring).

Either way, more invasive surgery is required than this session's
budget allowed. Phase 2 (direct sem poke) becomes the natural next
step — it bypasses the dispatch problem entirely by writing
directly to the kernel sem layout.

### Side note — `tFecEndRx` gets destroyed mid-run

In the longer test run (90s), `tFecEndRecover` (PEND+TO with
~14s timeout) wakes up at task list `t=15s` and runs
`m5200FecRestart`. That restart path zeros TCR[2..3], masks
SDMA bits, and **destroys `tFecEndRx`**. After that the task
list shrinks by 1 and frames have nobody to wake.

Not caused by Phase 1.B (same behavior with the fix reverted).
A long-run consequence of the BSP's recovery logic when it
decides FEC is dead. Mitigation for future: either we deliver
frames before t=15s, or we patch out `tFecEndRecover`'s timeout.

## What changed in code

- `hw/ppc/mac_newworld.c`:
  - **Phase 0.1:** boot station `{0x00132854, 0x00132857, "VX: SDMA
    Main ISR entry (0x132854)"}` added to `g_boot_stations[]`.
  - **Phase 0.3:** per-offset IC MMIO write logging at MBAR+0x500..
    0x52c — logs first 64 writes per offset with name (PerMask,
    MainMask, etc.), value, NIP, LR.
  - **Phase 1.B reverted** — no behavioral change to the RX hook.

## Verification

```bash
LOG=/tmp/qemu_p1c.log TIMEOUT=120 ./run.sh &
# wait for tFecEndRx in log + 8s settling, then probe FTP/firedrake

grep -c "STATION HIT.*0x00132854" /tmp/qemu_p1c.log     # 0 (ISR never runs)
grep -c "0x524 read => 0x20000000" /tmp/qemu_p1c.log    # 0 (no SDMA dispatch)
grep -c "SDMA eval.*RAISE" /tmp/qemu_p1c.log            # 1 (initial transition)
grep -c "force-arming" /tmp/qemu_p1c.log                # 11 (frames in DRAM)
grep "IC W +0x510" /tmp/qemu_p1c.log | tail -1          # MainMask = 0x00000000
```

## Honest progress

- Last session: ~40-45% to gate 12.
- This session: still ~40-45%. Phase 0 diagnostics added value to
  future debug sessions but no functional progress on gate 9.
- Gate 9 status unchanged: TCP handshake works, byte-level comms
  doesn't.

## Next session plan

Phase 2.A — direct sem poke. From the RX hook, after BD update +
SDMA RAISE, write into the VxWorks sem at `0x07bee080` to mark
it given. tFecEndRx wakes up regardless of whether EXT dispatch
ever runs. Steps:

1. Dump 64 bytes at `0x07bee080` (pre-PEND and post-PEND) to
   verify VxWorks `SEM_OBJ` layout (magic, type, state count,
   semQueue head).
2. Find a working sem-give path in the BSP (e.g. SLT1 timer's
   sysClkInt is known to do `semGive` somewhere — disasm it for
   the byte sequence to mimic).
3. From the RX hook, mimic semGive: if semQueue non-empty,
   dequeue first task and mark it READY (TCB+0x3C: 2→0); if
   empty, increment count.

Risk: VxWorks ready queue invariants we miss. Mitigate by
matching observed working sem state exactly.

Fallback: Phase 2.B — install one-shot patch at SLT1 tick that
calls BSP's `semGive(0x07bee080)`. Doorbell scaffolding already
exists at FSHOOK_OFFSET; add new opcode for sem-give. Safer
because BSP's own kernel does the work.

Out of scope: deeper QEMU interrupt-routing investigation (would
be a multi-day rabbit hole).
