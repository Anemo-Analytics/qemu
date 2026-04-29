# Session 2026-05-10 — readyQ found, BSP dispatch unblocked

Plan continuation from `SESSION_LOG_2026-05-09.md`. Picked up at the
"Find the real readyQ" step in the next-steps section.

## TL;DR

- ✅ **readyQ located at `0x0099AF58`** in runtime BSS. Identified by
  signature-matching bootrom symbols against the runtime ELF: bootrom
  `qPriBMapPut` (0x010b533c) → runtime `0x002cd8e0` (byte-identical
  body modulo BSS addresses). Cross-checked by tracing the wake-tail
  in runtime windPendQGet at 0x00178550 → `lis 3, 154; addi 3, 3,
  -20648; bctr [vtable+0x10]` ≡ `qPriBMapPut(0x0099AF58, TCB, prio)`.
- ✅ **windPendQGet identified at runtime `0x00178550`.** Equivalent
  to bootrom symbol `windPendQGet` (0x010cd914), with the in-line
  `windReadyQPut` tail (the runtime folds both into one function).
- ✅ **Replaced sysClkInt shim**: was `b semFlush @ 0x002ff884`
  (errors out before workQ enqueue), now sets up qPriBMapPut args
  inline and tail-calls `qPriBMapPut(readyQ, TCB, prio)` directly.
- ✅ **Broke through the dispatch wall.** Combined the BSP-side shim
  with a one-shot QEMU-side `*(0x0099AF58) = TCB` write at vt=10s
  ("READYQ-FORCE"). With both, **tFecEndRx PC moves** from
  `0x002fe918` (sem-block) to `0x00175628` (post-windExit epilogue
  in some Q operation). TCB.status flips PEND → READY persistently.
  Verified via TCB-WATCH every 0.5s vt=8..30s.
- 🟡 **Wake cascade observed.** Task list at vt=20s now shows tNetTask,
  tFecEndRecover, confLogMsg, tLed, getFactoryVerison all READY where
  they were PEND/PEND+TO before. tFecEndRx wake propagated to other
  blockers (semGive on related sems somewhere in its run).
- ❌ **Byte-level traffic still empty.** `nc 127.0.0.1 {2121, 9482}`
  during the live run returned nothing. pcap shows only host ARPs
  going unanswered (same as before). Wake mechanism cosmetically
  succeeds but the actual FEC RX → network-stack → daemon-response
  pipeline doesn't deliver.
- 🛑 **BSP-side qPriBMapPut alone insufficient.** With ONLY the BSP
  shim (no QEMU READYQ-FORCE), the task stays PEND-PC even though the
  shim ran (doorbell cleared, TCB.status=READY). qPriBMapPut writes
  TCB+0x08=key correctly, but readyQ.first/bmap[0] don't reflect our
  insertion at vt=10s. Either (a) the function is being called but
  its writes get rolled back by a concurrent context switch, or (b)
  the call signature isn't quite what we think. Forcing readyQ.first
  from QEMU at vt=10s is what actually unblocks dispatch.

## What landed in `hw/ppc/mac_newworld.c`

### 1. Shim rewrite (lines ~590..720)

- `semgive_addr` constant changed from `0x002ff884` (semFlush) to
  `0x002cd8e0` (qPriBMapPut).
- Stub expanded from 12 → 21 instructions (48 → 84 bytes). Sets up
  three explicit args before the tail-call:
  - `r3 = 0x0099AF58` (readyQ struct base) via lis/addi
  - `r4 = 0x07BEDE38` (tFecEndRx TCB) via lis/addi
  - `r5 = 29` (priority) via li
- Also explicitly clears `TCB.status` (TCB+0x3C = 0) and `TCB.pSemId`
  (TCB+0x5C = 0) before the call.
- beq target updated from +0x10 to +0x34 to skip past the new args
  setup when no doorbell is pending.

### 2. Track A removed (was lines ~1297..1410)

The 2026-05-09 manual-TCB-clear approach is gone. With the shim
rewrite, the BSP does the proper TCB writes itself — Track A's
clearing of sem.qHead/qTail was actually counter-productive.

### 3. READYQ-PROBE diagnostic (one-shot at vt=10s)

Reads sem state, TCB state, readyQ struct fields, bmap word/byte —
gives a complete snapshot of the wake mechanism's effect.

### 4. READYQ-FORCE one-shot (one-shot at vt=10s)

Writes `*(0x0099AF58) = TCB` and sets `bmap[0] |= (1 << 28)` plus
`bmap_byte[28] |= (1 << 2)` to forcibly add tFecEndRx to readyQ
priority bucket 29 from QEMU C. **This is the write that actually
makes dispatch happen.** Marked as a stop-gap until the BSP-side
shim's qPriBMapPut reaches its full effect.

### 5. TCB-WATCH extended (vt=8..30s, every 0.5s)

Lets us see post-dispatch state evolution through vt=30s.

## Ground truth captured (`/tmp/qemu_p3l.log`)

- readyQ struct at vt=10s pre-FORCE:
  - `readyQ@0x0099af58 first=0x07fd0608 [CpuloadLow] bmap=0x009a1208 vtbl=0x008d9014`
  - `readyQ.first.key = 0x000000ff` (CpuloadLow.key = 255 / idle)
  - `bmap[0]=0x00000001` (only CpuloadLow's bit set)
  - `bmap[1]=0x01000000` (= byte_array[0] = 0x01, also CpuloadLow's group-0 bit)
- Our TCB has key=0x1D = 29 written by shim (TCB+0x08).
- Post-FORCE: readyQ.first = TCB, bmap[0] |= bit 28.
- TCB-WATCH evolution:
  - vt=8.00s: PEND, PC=0x002fe918
  - vt=8.50s: READY, PC=0x002fe918 (status flipped, not yet dispatched)
  - vt=9.00..10.00s: READY, PC=0x002fe918 (still not dispatched)
  - vt=10.00s: READYQ-FORCE write
  - vt=10.50s: READY, PC=0x00175628 (DISPATCHED — PC moved)
  - vt=10.50..30s: READY, PC=0x00175628 (post-windExit epilogue)

## Symbol-matching method

Built a Python helper that takes a bootrom function start address,
extracts its byte signature with masking on instructions that load
BSS (lis/addi/lwz/stw with absolute addresses) and absolute branches
(b/bl), and searches for a UNIQUE match in the runtime .text. Found:

| Symbol            | Bootrom    | Runtime    |
|-------------------|------------|------------|
| qPriBMapPut       | 0x010b533c | 0x002cd8e0 |
| qPriBMapGet       | 0x010b53c0 | 0x002cd964 |
| qPriBMapRemove    | 0x010b53fc | 0x002cd9a0 |
| qPriBMapInit      | 0x010b5240 | 0x002cd7e4 |
| windExit          | 0x010d3624 | 0x00207efc |
| semBGiveDefer     | 0x010c5bb8 | 0x002fe95c |
| semFlush          | 0x010c6844 | 0x002ff884 |
| windPendQGet      | 0x010cd914 | 0x00178550 (less unique, traced) |
| readyQBMap        | 0x011fb910 | 0x0099AF58 (BSS, traced via lis/addi) |

## Next steps

1. **Why doesn't BSP qPriBMapPut update readyQ.first durably?** The
   shim's mechanical writes succeed (TCB.status=READY, TCB+0x08=key)
   but the readyQ-side updates (readyQ.first=TCB, bmap bit set)
   either don't happen or get rolled back. Possibilities:
   - bMapAtomicSet at 0x002cdc78 may invert the priority via
     `subfic 4, 4, 255` — needs careful re-check of bit positions
     vs what we observe.
   - A concurrent context switch (e.g. CpuloadLow being re-PUT into
     readyQ on its own dispatch loop) overwrites our bmap bit.
   - The function we sig-matched as qPriBMapPut might not be that.

2. **Why no byte-level traffic after dispatch?** tFecEndRx wakes,
   ends at PC=0x175628 (function epilogue). The Q operation it ran
   may not have processed an actual FEC RX frame. Check whether
   FEC RX walker is delivering frames at the moment of wake — pcap
   shows host ARPs going unanswered. Possible re-investigation:
   - tFecEndRx might wake, find no work in its sem queue (we cleared
     sem.qHead via Track A in old session, sem.qHead currently
     untouched but may have stale data), and yield.
   - Or the TX path is broken (FEC TX → BestComm → host pcap).

3. **The wake CASCADE.** tNetTask, tFecEndRecover, confLogMsg, tLed
   becoming READY in the task list is interesting. Either tFecEndRx
   actually did some work that woke them, OR the readyQ structure
   we manipulated has knock-on effects on other tasks' status. Worth
   a focused look — if the cascade is genuine, we might be closer to
   functional networking than the pcap suggests.

4. **Toolkit-recognition test (Track C).** Still blocked until byte-
   level comms work. Defer.

## Time

- ~2.5h. Symbol matching + sig-finding took ~30 min, most of the
  rest was iterating on the shim's calling convention (passed sem,
  then sem+8, then explicit args; only the explicit-args version
  showed effect, and even then needed QEMU-side READYQ-FORCE to
  actually dispatch).

## Risks captured

- The shim's args are hard-coded for tFecEndRx (TCB=0x07BEDE38,
  prio=29, sem=0x07bee080). Any other PEND'd task we want to wake
  needs a different shim — currently this is single-shot per build.
- The QEMU-side READYQ-FORCE is a hack. It works because the
  scheduler reads readyQ.first directly, but the bmap state is now
  inconsistent with readyQ.first (bmap claims a key=29 task is in
  group 28, but the bucket FIFO list inside the bmap struct doesn't
  have us linked). This may break later kernel ops that walk the
  bucket lists.
- Wake CASCADE may indicate we corrupted other tasks' status fields
  by accident. Verify by reading individual TCB.status for tNetTask,
  tFecEndRecover at vt=20s — if READY but PC=PEND-PC, the task is
  in an inconsistent state and might never recover.
