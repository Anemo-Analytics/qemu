# Session 2026-05-05 — gate 9 dispatch hunt: SLT1 drop confirms MSR.EE=0 is the real wall

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

## Phase 1.X — additional dispatch experiment, post-agent review

After agents reviewed the plan and pointed out an EXT-OR
hypothesis (`s->ic_pending` SLT1 holds the line high), tried
a third dispatch attempt: drop SLT1 from `mpc5200_update_ext`
OR. sysClkInt is on the DEC vector (per
BSP_static_a1_vxworks.md and runtime station 0x117fd0), so the
clock keeps working even with SLT1 removed from EXT.

Built and ran with enhanced SDMA eval diagnostic that dumps
`pi.EXT` (CPU pending_interrupts EXT bit) and `MSR.EE` at the
moment of evaluation.

**Result of SLT1-drop experiment:**

```
SDMA eval: IntPending=0x0000000c IntMask=0xeffffff7 unmasked=0x00000008
RAISE  [pi.EXT=0 MSR.EE=0 slt=1 fec=0 sdma=1 nip=0x00145430]
EXT line 0->1 (slt=1 fec=0 sdma=1) #47
```

Two facts:

1. ✅ **The fix worked at the OR level.** `pi.EXT=0` BEFORE
   the RAISE — confirms SLT1 was indeed holding `pending_
   interrupts` EXT bit set previously, and removing SLT1 from
   the OR cleanly produced a 0→1 transition in
   `pending_interrupts`.

2. ❌ **MSR.EE=0 throughout.** Every SDMA eval, EE=0. The CPU
   is in a state where interrupts are disabled. When
   `ppc_set_irq(EXT,1)` calls `ppc_maybe_interrupt`, it sees
   `async_deliver = msr.EE || resume_as_sreset` = 0,
   and falls through to `cpu_reset_interrupt(HARD)` —
   clears the HARD bit. The line is up in
   `pending_interrupts`, but the CPU never re-evaluates
   because its idle loop never enables EE.

3. ❌ **Same NIP=0x145430 across all SDMA eval calls.** CPU is
   stuck in some kernel idle/lock loop with EE=0. FEC IRQs
   that DID dispatch happened during BSP init when EE was 1
   most of the time. By the time SDMA needs to deliver, the
   BSP has entered steady-state where EE=0 dominates.

4. ❌ **Still 0 STATION HIT 0x132854.**

So **dispatch is genuinely walled off by an EE=0 hold**, not
just an edge issue. Three dispatch experiments now ruled out
(1.A mask, 1.B edge re-trigger, 1.X SLT1 drop). Reverted: PEND-
WATCH (didn't fire — diag_sample stops at 60s of guest time).
Kept: SLT1 drop (it's correct — sysClkInt is DEC-vector) and
enhanced SDMA eval diagnostic.

## What's permanently in code after this session

- `g_boot_stations[]`: SDMA Main ISR @ 0x132854 entry (Phase
  0.1).
- IC MMIO write logging at MBAR+0x500..0x52c with per-offset
  decode (Phase 0.3).
- `mpc5200_update_ext` no longer ORs in `s->ic_pending` (Phase
  1.X — sysClkInt is on DEC, removing SLT1 from EXT is correct
  regardless of dispatch outcome).
- `mpc5200_sdma_eval_irq` enhanced log: `pi.EXT`, `MSR.EE`,
  per-source flags, NIP (Phase 1.X diagnostic — invaluable for
  future debug).

## Next session plan — QEMU-internal paths only

Three dispatch experiments ruled out (mask, edge re-trigger,
SLT1 drop). MSR.EE=0 hold confirmed as the wall. Several
QEMU-internal options exist, ranked by promise:

### A — Phase 2.B (BSP-side semGive shim) — RECOMMENDED

Install a one-shot patch on a periodic BSP code site (SLT1 tick
callback, sysClkInt tail, or similar that runs in kernel
context with the right scheduler invariants) that, when
triggered by a flag we set in the QEMU IO thread, calls the
BSP's own `semGive(0x07bee080)`. Doorbell scaffolding already
exists at `FSHOOK_OFFSET`.

Per agent 2's review, **don't try direct sem-poke from QEMU
side** — VxWorks `SEM_OBJ` layout assumed in the original plan
was wrong (magic at +0 is a class pointer, not ASCII tag; Q_HEAD
is 16 bytes; ready-queue invariants are extensive — `taskIdReady`,
`readyQBmap`, etc.). BSP's own kernel must do the give.

First step (per agent 2): zero-code-change run that dumps 64
bytes at `0x07bee080` + 2 other sems (e.g. `tNetTask`'s
`0x00980a48`) for layout comparison. Identify class pointer,
Q_HEAD offset, state byte. **Only then** decide patch site.

Estimated: 1-2 sessions to first verified `tFecEndRx` wake.

### B — Patch tFecEndRecover so tFecEndRx survives

Side issue from this session: `tFecEndRecover` (PEND+TO with
~14s timeout) wakes at t≈15s and runs `m5200FecRestart`,
which **kills `tFecEndRx`**. Even if we get a wake mechanism
working, by t=20s there's no task to wake. NOP out the
tFecEndRecover timer or the kill path. Cheap, complementary
to A.

### C — Force MSR.EE=1 transition from QEMU side

Invasive: in the RX hook, after the SDMA RAISE, directly poke
`s->cpu->env.msr |= MSR_EE` and call `ppc_maybe_interrupt(env)`.
Forces the dispatch to re-evaluate with EE=1. Risk: violates
VxWorks's intLock invariant (BSP set EE=0 for a reason).
Could corrupt scheduler state. Try only if A fails.

### D — Find the EE=0 holder and patch it

CPU sits at NIP=0x145430 with EE=0 across the SDMA-pending
window. That's an `intLock`'d region. Disasm 0x145430,
identify what's holding it. Could be `windExit`, `taskLock`,
or a recovery path. Patch the EE-clear so EE stays 1.
Higher-stakes but architecturally cleanest.

### Recommended order

1. **A first.** semGive shim sidesteps the EE=0 problem
   entirely without touching delicate kernel internals.
   Highest probability of unblocking gate 9.
2. **B alongside A** — cheap insurance that the wake target
   doesn't get murdered before we wake it.
3. C and D are escalation paths if A doesn't work, in that
   order of risk.

Strategic note: even after gate 9 closes, gates 10-12 (toolkit
acceptance + software-load) are the actual long pole. Each
session should aim to make a downstream blocker visible
(socket layer wakeup, TX BD walk on reply, daemon-specific
protocol handler) — not just to wake the current PEND'd task.
