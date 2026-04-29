# Session 2026-05-07 — gate 9 follow-up: semFlush swap doesn't close gate, but pinpoints the blocker

Plan: 4-step diagnostic on the gate-9 hypotheses left from 2026-05-06
(`semGive shim ran but tFecEndRx never woke`). Step 0 swapped the
shim's tail-call from `semGive @ 0x002ff5c4` to `semFlush @ 0x002ff884`
on the theory that semFlush's wake-all impl bypasses whatever state
check semGive applied. Steps 1-3 added read-only diagnostics.

Result: gate 9 still open. But three of the four hypotheses are
falsified and one is the surviving cause.

## TL;DR

- ❌ **Step 0 (semFlush swap) did NOT close gate 9.** semFlush is
  called via the shim and runs end-to-end (verified via SEM-HIST),
  but `tFecEndRx` still stays PEND on `0x07bee080` throughout the
  vt=8..14s synth-doorbell window.
- ❌ **Hypothesis 3 (sem-layout mismatch) FALSIFIED.** SEM-LAYOUT
  shows identical structure across the failing sem (`0x07bee080`,
  tFecEndRx) and two known-OK sems (`0x00980a48` tNetTask,
  `0x07ba6828` tWdbTask). Same +0x0C magic ptr (`0x008d8fd4`),
  same class byte at +4 (`0x07` for all three). Reading the layout
  the same way semGive's slow path does it.
- ✅ **Hypothesis 1 (semGive bailed early) ALSO FALSIFIED.** The
  swap is to semFlush, which has a different post-dispatch impl
  but identical entry/dispatch wrapper. SEM-HIST shows
  `0x002ff884:1` (prologue), `0x002ff9a0:1`, `0x002ff9e0:1` — the
  function ran fully through entry, body, and epilogue once when
  the synth doorbell fired. Either give-impl reaches the wake step.
- ✅ **Hypothesis 2 (workQAdd defers wake; windExit doesn't dispatch)
  is the surviving cause.** TCB-WATCH at 60 Hz across vt=8..14s
  shows zero transitions on tFecEndRx — status stays `0x2 PEND`,
  pSemId stays `0x07bee080`, errno never moves, saved PC stays at
  `0x002fe918` (the kernel sem-block routine). The wake never
  reaches the scheduler.

## What ran end-to-end

`STATION HIT [72] @ NIP=0x002ff5c4 ... semGive entry (BSP-direct)` —
ordinary BSP `semGive(...)` callers (5 hits before vt=15s; not
shim-related, expected).

`STATION HIT [73] @ NIP=0x002ff884 MSR=0x00009030 ... semFlush entry (via shim)` —
the synthetic-doorbell tail-call hits semFlush. MSR=0x9030 → EE=1
when the wake runs, so the 2026-05-05 EE=0 hold isn't the issue at
the wake site itself.

`FS_HOOK: SEM_QUEUE write 0x00000000 (NIP=0x002acf14 LR=0x001180bc)` —
shim cleared the doorbell after reading sem_id (one and only one
clear, matching the single synth doorbell).

`SYNTH-DOORBELL: t=9s ... s->fshook.sem_queue = 0x00000000` — first
post-doorbell tick shows the BSP-side already cleared the slot.

## SEM-HIST (Step 1)

```
SEM-HIST: semGive+semFlush body samples (0x002ff5c0..0x002ffa00) at vt=15s:
SEM-HIST:   0x002ff5c4 : 5    semGive prologue  (BSP-direct callers)
SEM-HIST:   0x002ff610 : 1    semGive slow-path entry
SEM-HIST:   0x002ff71c : 2    semGive class-table dispatch area
SEM-HIST:   0x002ff730 : 4    semGive post-dispatch
SEM-HIST:   0x002ff884 : 1    semFlush prologue  (THE SHIM CALL)
SEM-HIST:   0x002ff9a0 : 1    semFlush body
SEM-HIST:   0x002ff9e0 : 1    semFlush body / epilogue
SEM-HIST: 7 distinct PCs sampled
```

The semFlush body sampled at three distinct PCs (prologue, mid, end)
in a function ~0x180 bytes long, sampled at 100 us intervals. Single
end-to-end execution, return path reached.

## SEM-LAYOUT (Step 2) — falsifies hypothesis 3

```
SEM-LAYOUT: 0x07bee080 tFecEndRx (failing)
  [+0x00]=0x07bede38  [+0x04]=0x07bede38  [+0x08]=0x00000000  [+0x0C]=0x008d8fd4
  [+0x10]=0x07fefe00  [+0x14]=0x00000000  [+0x18]=0x00000000  [+0x1C]=0x00fefe00
  class-byte (top of word @ +4) = 0x07

SEM-LAYOUT: 0x00980a48 tNetTask (BSP-internal)
  [+0x00]=0x07fc1f30  [+0x04]=0x07fc1f30  [+0x08]=0x00000000  [+0x0C]=0x008d8fd4
  [+0x10]=0x07fc1f30  [+0x14]=0x00000000  [+0x18]=0x00000000  [+0x1C]=0x00000000
  class-byte (top of word @ +4) = 0x07

SEM-LAYOUT: 0x07ba6828 tWdbTask (debug agent)
  [+0x00]=0x07ba65e0  [+0x04]=0x07ba65e0  [+0x08]=0x00000000  [+0x0C]=0x008d8fd4
  [+0x10]=0x07fefe00  [+0x14]=0x00000000  [+0x18]=0x00000000  [+0x1C]=0x00000000
  class-byte (top of word @ +4) = 0x07
```

All three sems:

- Same +0x0C magic ptr `0x008d8fd4` — sem class table / vtable.
- Same class byte at +4 = `0x07` (top byte of qTail pointer; NOT an
  out-of-range class index — the dispatch is keyed on something
  else, OR the +0x0C magic IS the class).
- qHead = qTail (both = waiter's TCB+0 qNode) — so the wait queue
  has exactly one waiter for each sem. tFecEndRx IS queued on its
  sem; not a queue-pointer issue.

## TCB-WATCH (Step 3) — confirms hypothesis 2

```
TCB-WATCH: t=8.00s  status=0x2 PEND  sem=0x07bee080  errno=0  PC=0x002fe918
TCB-WATCH: t=8.02s  status=0x2 PEND  sem=0x07bee080  errno=0  PC=0x002fe918
... (360 ticks, all identical) ...
TCB-WATCH: t=14.00s status=0x2 PEND  sem=0x07bee080  errno=0  PC=0x002fe918
```

Zero transitions across the entire window. The wake produced by
semFlush never reaches the scheduler. The kernel's sem-block routine
at `0x002fe918` is where tFecEndRx is suspended; the routine never
returns even though its sem was flushed.

## What this leaves us with (the next blocker, precisely)

**Hypothesis 2 (workQAdd defer / windExit dispatch) is the surviving
cause.** The chain:

1. Shim runs in sysClkInt ISR context (intCnt > 0).
2. Shim calls intUnlock to set EE=1 (verified — semFlush executes
   with MSR.EE=1).
3. Shim tail-calls semFlush. semFlush's wake step does NOT directly
   schedule the waiter — it queues a work item via `workQAdd`.
4. The queued work item is dispatched in `windExit` when the ISR
   returns and intCnt drops to 0.
5. **Either windExit isn't running, or its workQ drain isn't
   processing the queued wake.**

Confirming evidence: every other PEND'd task in the system is also
stuck at PC=`0x002fe918` and never moves. So this isn't tFecEndRx-
specific — the kernel scheduler isn't dispatching ANY task across
the post-doorbell window. Either windExit is permanently blocked or
the workQ drain path is broken.

(Cross-reference: 2026-05-05's "MSR.EE=0 hold" finding — sysClkInt
runs, but the dispatch wall holds EE=0 across SDMA-pending windows.
At vt=8s we deliberately fired outside that window, but maybe
windExit is gated on something else we haven't characterized.)

## Recommended next-session moves

Two avenues, ranked by signal-per-effort:

1. **Probe windExit / workQ state directly.** Add a watcher in
   `mpc5200_tick` that dumps:
   - `intCnt` location (need to find — likely BSS near taskIdCurrent
     at 0x008d... range)
   - workQ head/tail pointers (look up `workQHead` / `workQTail`
     symbols in vxworks.out symtab, similar to how we found activeQHead)
   - kernel state byte (`kernelState`?)
   Sample at 60 Hz across vt=7..15s. If intCnt stays > 0 the entire
   window, the ISR is reentrant or stuck. If workQ has unprocessed
   items, drain isn't running.

2. **Pivot to non-ISR patch site** (the option the 2026-05-06 plan
   noted as fallback). The idle loop `0x145430` runs at intCnt=0,
   so a semFlush call from there sidesteps the workQAdd-defer path
   entirely. Risk: that's also where the EE=0 hold lives, so the
   patch site might be unreachable for the same reason the workQ
   isn't draining.

Order: probe (1) first. ~30 min. Tells us which fork to take.

## What's permanently in code after this session

- `semgive_addr = 0x002ff884` in `mpc5200_apply_keyswitch_patches`
  (the swap from `0x002ff5c4`). All inline comments, the verify
  printout, and the final fprintf updated to mention semFlush.
- New boot station `0x002ff884` "VX: semFlush entry (via shim)";
  pre-existing `0x002ff5c4` station relabeled "BSP-direct, not shim".
- `g_semgive_hist[]` (parallel histogram, 272 entries covering
  `0x002ff5c0..0x002ffa00`). Increment in `mpc5200_diag_sample`;
  end-of-run dump in `mpc5200_diag_sample`'s diag-end branch; one-shot
  dump in `mpc5200_tick` at vt=15s (because the diag-end branch only
  fires if QEMU runs >60 s of virtual time).
- One-shot SEM-LAYOUT dump in `mpc5200_tick` at vt=10s (three sems).
- Per-tick TCB-WATCH dump in `mpc5200_tick` across vt=8..14s.

Logging is rate-limited (one-shot or fixed window). Adds ~370 lines
to a 60 s run — small.
