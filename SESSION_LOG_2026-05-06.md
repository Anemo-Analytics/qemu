# Session 2026-05-06 — gate 9 BSP-side semGive shim: shim works, downstream wake doesn't

Plan: BSP-side semGive shim — wake tFecEndRx via host→guest doorbell
(in-message plan from this session). Result: shim mechanically works
end-to-end (read+clear+tail-call semGive verified), but tFecEndRx
stays PEND. New visible blocker is downstream of semGive.

## TL;DR

- ✅ **`semGive` address found.** Public dispatch wrapper at
  **`0x002ff5c4`** (vxworks.out runtime). Verified via:
  - Byte-identical prologue to bootrom semGive @ 0x010c6584 (instruction-
    for-instruction match modulo immediate offsets).
  - Runtime symbol-table record at `0x008fe080` mapping name string
    "semGive" @ `0x003e67b0` → fn `0x002ff5c4`.
  - 686 direct `bl` callers in vxworks.out text.
- ✅ **`FSHOOK_REG_SEM_QUEUE` register.** New offset `0x1C` in the
  FSHOOK doorbell range. QEMU writes a sem_id; BSP polls via the
  sysClkInt tail-patch.
- ✅ **sysClkInt tail-patch installed and verified.** Replaced
  `bl 0x00138a7c` (intUnlock) at `0x001180b8` with `bl 0x002acef0`
  (our 12-instruction stub installed in the freed body of the
  no-op'd printf).
- ✅ **Shim mechanically works.** With a synthetic doorbell write
  at virtual t=8s:
  - At t=8s, `s->fshook.sem_queue = 0x07bee080` (set by QEMU).
  - At t=9s, `s->fshook.sem_queue = 0x00000000` (cleared by BSP shim).
  - `FS_HOOK: SEM_QUEUE write 0x00000000 (NIP=0x002acf14
    LR=0x001180bc)` — proves the shim's `stw r9, 0(r10)` ran with
    caller LR pointing at sysClkInt's continuation.
- ❌ **`tFecEndRx` stays PEND.** Across t=8s..t=13s the task list
  shows `tFecEndRx prio=29 PEND sem=0x07bee080` unchanged. SEM_OBJ
  at 0x07bee080 also unchanged (wait queue still has tFecEndRx
  enqueued at offsets +0x00/+0x04). Either semGive's wake-the-waiter
  logic didn't run, OR it did and the wake got queued via workQAdd
  but never dispatched (likely because of the same MSR.EE=0 hold the
  previous session identified — windExit can't dispatch if the next
  RFI doesn't restore EE).
- ✅ **Side-effect: tFecEndRx is no longer killed by tFecEndRecover at
  t=15s.** Previous session reported the recovery path destroying
  tFecEndRx by t=15s. With the synth doorbell at t=8s, tFecEndRx
  survives at least to t=18s. Some state changed; not enough to
  unblock RX comms.

## Code changes

`hw/ppc/mac_newworld.c`:

1. **`FSHOOK_REG_SEM_QUEUE = 0x1C`** added. Read returns
   `s->fshook.sem_queue`; write stores into it (with rate-limited log
   "FS_HOOK: SEM_QUEUE write 0x… (NIP=… LR=…)"). New struct field
   `s->fshook.sem_queue` (uint32_t).
2. **`mpc5200_apply_keyswitch_patches`** extended:
   - Builds a 48-byte (12-instruction) stub at `0x002acef0` with PC-
     relative `bl 0x00138a7c` (intUnlock) and `b 0x002ff5c4` (semGive
     tail-call). Branch displacements computed in C from
     `(target - pc) & 0x03FFFFFC`.
   - Patches sysClkInt at `0x001180b8`: replaces `bl 0x00138a7c`
     (intUnlock = `0x480209c5`) with `bl 0x002acef0` (= `0x48194e39`).
   - Read-back verifies the patched bytes immediately after
     installation: `MPC5200: post-patch verify: 0x001180b8 = 48194e39
     ...`. (The misleading "expect 48194c39" string in the original
     log is a typo in the format string — actual encoding 48194e39 IS
     correct.)
3. **`mpc5200_bestcomm_rx_hook`** — after the existing SDMA force-arm,
   sets `s->fshook.sem_queue = 0x07bee080`. Doorbell write happens
   from the IO thread; BSP picks it up on the next sysClkInt tick.
4. **`g_boot_stations[]`** — two new entries: `0x002acef0` (stub
   entry) and `0x002ff5c4` (semGive entry). Boot stations only fire
   on first NIP hit, so they're useful as "did this code path exist
   at all" sanity checks.
5. **Synthetic doorbell test** (in `mpc5200_tick`): at virtual t=8s
   sets `s->fshook.sem_queue = 0x07bee080`, then logs the slot value
   once per second t=8s..14s. Lets us verify the shim end-to-end
   without needing inbound RX traffic.

## What the shim looks like

Stub at `0x002acef0` (48 bytes, 12 instructions):

```
+0x00  7d8802a6  mflr  r12              ; save sysClkInt continuation
+0x04  4be8bb89  bl    0x00138a7c       ; intUnlock (sets MSR.EE=1)
+0x08  3d40f000  lis   r10, 0xF000
+0x0C  614a401c  ori   r10, r10, 0x401C ; r10 = SEM_QUEUE doorbell
+0x10  806a0000  lwz   r3,  0(r10)      ; r3 = pending sem_id
+0x14  7d8803a6  mtlr  r12              ; pre-set LR for tail-call
+0x18  2c030000  cmpwi r3, 0
+0x1C  41820010  beq   +0x10            ; (-> 0x002acf2c) if no sem
+0x20  39200000  li    r9, 0
+0x24  912a0000  stw   r9,  0(r10)      ; clear flag BEFORE call
+0x28  480526ac  b     0x002ff5c4       ; tail-call semGive(r3)
+0x2C  4e800020  blr                    ; reached only when no sem
```

The tail-call trick: we load LR with the sysClkInt continuation
(0x001180bc) before `b semGive`. semGive saves LR on stack, does its
work, restores LR, and `blr` returns directly to sysClkInt — bypassing
the stub on return.

## Why it didn't actually wake tFecEndRx

Open question. The shim's `stw r9, 0(r10)` ran (we see the FS_HOOK
SEM_QUEUE write 0x00000000 in the log). The `b 0x002ff5c4` MUST also
execute (no exception, no fault). So semGive WAS entered.

Three plausible reasons it didn't translate to a tFecEndRx wake:

1. **semGive's slow path bailed early.** semGive @ 0x002ff5c4 first
   does `andi. r0, r31, 1` to check the low bit of sem_id. For
   0x07bee080 (low bit 0) it branches to 0x002ff610 (slow path).
   The slow path then reads `*(0x008d5c60)` and bails to 0x002ff6dc
   if zero — that's a "sem subsystem not initialized" check. If it
   bailed, no wake.
2. **semGive succeeded but deferred via workQAdd, never dispatched.**
   sysClkInt runs in interrupt context (intCnt > 0). semGive in this
   context queues the wake via workQAdd; the actual scheduler
   reschedule happens in `windExit` on ISR exit. If MSR.EE=0 hold
   prevents windExit from running properly (matches 2026-05-05
   finding), the workQ entry sits indefinitely.
3. **0x07bee080 isn't a standard sem.** Layout dump shows:
   `+0x00=07bede38 +0x04=07bede38 +0x08=00000000 +0x0C=008d8fd4
    +0x10=07fefe00 +0x14=00000000 +0x18=00000000 +0x1C=00fefe00`.
   `+0x00`/`+0x04` = tFecEndRx's TCB (the lone waiter), suggesting
   the wait queue head IS at +0x00. But `+0x0C=008d8fd4` looks like
   a class magic ptr — different layout than agent 2 assumed (agent
   2 expected magic at +0). semGive's class-dispatch may not match
   this layout, falling through silently.

We don't have evidence enough to pick between these three this
session.

## Verification log

`/tmp/qemu_p2f.log` (TIMEOUT=60, with synth doorbell at t=8s):

```
SYNTH-DOORBELL: writing s->fshook.sem_queue = 0x07bee080 at t=8s ...
SYNTH-DOORBELL: t=8s, s->fshook.sem_queue = 0x07bee080
SYNTH-DOORBELL: t=9s, s->fshook.sem_queue = 0x00000000   <-- BSP cleared!
SYNTH-DOORBELL: t=10s, s->fshook.sem_queue = 0x00000000
...
FS_HOOK: SEM_QUEUE write 0x00000000 (NIP=0x002acf14 LR=0x001180bc)

(task list shows tFecEndRx PEND sem=0x07bee080 unchanged
 throughout t=8s..t=13s)
```

The clearing of sem_queue and the FS_HOOK log together prove the
shim's prologue (`mflr r12`, `bl intUnlock`), MMIO read (`lwz r3,
0(r10)`), the conditional check, and the clearing store
(`stw r9, 0(r10)`) all execute correctly. The next instruction
(`b 0x002ff5c4`) must also execute to reach the BSP semGive entry.

## Honest progress

- Last session (2026-05-05): ~40-45% to gate 12.
- This session: still ~40-45%. We surgically demonstrated the shim
  mechanism works at the instruction level, but the wake doesn't
  propagate to tFecEndRx — so gate 9 remains effectively where it
  was (TCP handshake works, byte-level comms doesn't).
- New visible blocker: **somewhere in semGive's wake path or workQ
  dispatch**, not in EXT IRQ dispatch as we previously suspected.
  This is more useful than the previous "MSR.EE=0 wall" framing —
  we now know the wake mechanism *can* run with EE=1, and the
  problem is downstream of where we get control.

## What's permanently in code after this session

- `FSHOOK_REG_SEM_QUEUE` register at 0x1C of the FSHOOK doorbell.
- `s->fshook.sem_queue` field in MPC5200State.
- 48-byte semGive shim at `0x002acef0` (in freed body of no-op'd
  printf at 0x002acee8).
- Patch at sysClkInt `0x001180b8` calling the shim instead of
  intUnlock directly.
- Boot stations: `0x002acef0` (stub entry), `0x002ff5c4` (semGive
  entry).
- RX hook writes `s->fshook.sem_queue = 0x07bee080` after force-arm.
- Synthetic doorbell test in `mpc5200_tick` at virtual t=8s
  (preserved as a verification utility — fires once per boot).
- Read-back verification of patched bytes in
  `mpc5200_apply_keyswitch_patches`.

## Next session — debug semGive's wake path

Three concrete experiments to identify which of the 3 reasons above
is correct:

### A. Add a downstream boot station to detect semGive's slow-path body

Place stations at `0x002ff5c4 + N` (e.g. `0x002ff610` slow-path
target, `0x002ff6dc` early-bail target, `0x002ff710`-ish wake-the-
waiter target). Currently boot stations only fire on first hit, so
all of these are likely already triggered during BSP init. Need to
extend the diag_sample to log every Nth hit (or all hits with a
counter) for a small set of "semGive internal" stations. This will
directly answer whether semGive went down the wake path or bailed.

### B. Inspect kernel intCnt and workQ state

After our shim fires (t=8s), dump kernel state at
`*(0x008d5c60)` and any visible workQ data structures. If intCnt is
high, the wake is queued and not dispatched.

### C. Confirm sem layout via tNetTask (also PEND'd)

tNetTask is PEND'd on `0x00980a48`. Dump 32 bytes there and compare
to 0x07bee080's layout. If the structure is identical, our sem-layout
read is correct and the issue is in the give path; if different, the
sem class/structure assumption is wrong.

### Recommended order

1. **A first** — quickest signal about whether semGive bailed early
   or made it to the wake step. ~30 minutes.
2. **C second** if A says "semGive bailed" — checks the sem-layout
   theory. ~15 min.
3. **B last** — only relevant if A says "semGive wake step ran but
   nothing happened" (i.e., workQ dispatch issue).

If all three rule out a code-path issue, the fallback is to install
a more invasive workaround: instead of relying on sysClkInt's tick,
put the shim in a non-ISR context (e.g. the BSP's idle loop, where
intCnt = 0). That's a deeper patch site to find.
