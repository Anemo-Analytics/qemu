# Session 2026-05-09 — pivot to toolkit-side test, Track A cosmetic only

Plan: stop the gate-9 dispatch hunt; manually unblock `tFecEndRx` from
QEMU C code (Track A), confirm byte-level Firedrake/FTP comms (Track B),
optionally probe with a Python client (Track C.1). Premise per
COMPLETE_CONNECTION_FLOW_FOUND.md: toolkit recognition only needs
Firedrake daemons, not full `tApMain` boot.

Result: Track A's TCB writes are mechanically clean and don't crash
the BSP, but **status flip alone does not dispatch the task** — the
scheduler uses a separate readyQ that we have not located. Track B
confirms no byte-level reply on any daemon port.

## TL;DR

- ✅ **Track A TCB unblock writes succeed** at vt=8s: sem qHead/qTail
  cleared to sentinel `0x0099af68`, TCB.status flipped `0x2 PEND` →
  `0x0 READY`, TCB.pSemId zeroed. TCB-WATCH confirms status stays
  READY through vt=14s. BSP did not crash.
- 🛑 **tFecEndRx still not dispatched.** PC stays at `0x002fe918`
  (sem-block routine) the entire window — the scheduler never picks
  the task. CpuloadLow (prio=255) keeps running while a prio=29 READY
  task waits. Confirms TCB.status=READY is necessary but not
  sufficient; the scheduler reads a separate readyQ.
- 🛑 **Track B byte-level comms still silent.** `nc 127.0.0.1
  {2121,9482,2049,3111}` all return zero bytes; pcap shows only the
  guest's gratuitous ARP at boot, no replies to host probes. Host-side
  TCP sessions sit in CLOSE-WAIT (slirp accepted but guest never
  read). Track C.1 skipped — would have hit the same wall.
- ❌ **Plan's "activeQ splice" was wrong** — the queue at `0x008d94e4`
  is the tasksList (all tasks), not a ready queue. Verified by the
  pre-patch task list at t=8s containing 60 tasks of every status
  (READY, PEND, DELAY, PEND+TO). First splice attempt corrupted the
  DLL by leaving tFecEndRx in its old position while inserting at
  head; the walker then looped (tFecEndRx appeared at indexes [0],
  [11], [22], [33], …). Reverted; only TCB writes + sem clear remain.

## What landed in code (`hw/ppc/mac_newworld.c`)

1. **`TRACK-A` block** at `mpc5200_tick`, fired once at `tick_count
   == 60 * 8`:
   - Reads sem `0x07bee080` qHead/qTail, TCB `0x07bede38`
     status/pSemId — pre-state dump.
   - `stl_be_phys`: sem+0=sentinel, sem+4=sentinel,
     TCB+0x3C=0 (READY), TCB+0x5C=0.
   - Post-state dump confirms all four writes.
2. **`READYQ-SCAN` block** at `tick_count == 60 * 11`:
   - Sanity check on `*(0x008d94e4)` (activeQHead first link).
   - Scans BSS `0x008d0000..0x00920000` for any RAM-range pointer
     (`0x07b00000..0x08000000`). 131 hits at vt=11s.

## Ground truth captured (`/tmp/qemu_p38b.log`, `/tmp/qemu_p3a.log`,
`/tmp/qemu_p3d.log`)

- Sem 0x07bee080 layout (pre-Track-A):
  - `qHead = qTail = 0x07bede38` (= TCB+0x00, the pendNode)
  - `[+0x0C] = 0x008d8fd4` (vtable, same value across 3 sampled sems)
  - `class-byte` (top of word at +4) = 0x07
- TCB layout for `tFecEndRx` (verified by both static read and walker):
  - TCB starts at `0x07bede38`. pendNode is at TCB+0x00.
    tasksList-link is at TCB+0x20.
  - status `+0x3C`, priority `+0x40`, pSemId `+0x5C`,
    errno `+0x84`, REG_SET base `+0x130` (PC=`+0x130+0x8C`,
    LR=`+0x130+0x84`, SP=`+0x130+0x04`).
- Empty-queue sentinel: `*(0x008d951c) = 0x0099af68`
  (per SEM-SENTINEL); semFlush's hook fn errors out unless qHead
  matches this value (or sentinel+0x24 = `0x00995fe0`).
- kernelState `0x008d5c60` = 0 across vt=8..15s (scheduler in direct
  dispatch).

## Track B evidence

- pcap `/tmp/qemu_p3b.bin` (first 30 frames):
  - Frame 1: gratuitous ARP from guest `169.254.254.254`
  - Frames 2–3: ARP requests from QEMU host MAC `00:02:00:00:00:11`
    for `172.31.197.254` / `172.30.1.254` (slirp NAT internal)
  - Frames 4+: host-originated ARPs for `169.254.254.15`, no
    replies from guest after frame 1.
- `ss -t state all` after probes:
  - LISTEN sockets bound on host `0.0.0.0:9482`, `0.0.0.0:3111`.
  - CLOSE-WAIT pairs on probed ports — slirp accepted the SYN
    locally, guest BSP never accepted/read.

## Where the readyQ is NOT

131-hit BSS scan results saved in `/tmp/qemu_p3d.log` under
`READYQ-SCAN:`. No obvious second Q_HEAD adjacent to activeQHead
(`0x008d94e4..0x008d94e8`). Three suspicious "self-pair" hits at:

- `*(0x0090ebf4) = *(0x0090ebf8) = 0x07fe5dd0`
- `*(0x0090eca4) = *(0x0090eca8) = 0x07fe5868`
- `*(0x0090ed54) = *(0x0090ed58) = 0x07fe5300`

These look like Q_HEADs for some structure (not self-empty though —
the head and value differ). Possibly tickQ entries for each clock.
Worth a follow-up disasm pass.

The readyQ is most likely **a priority-bucketed multiQ** (256 slots
for priorities 0-255) rather than a single Q_HEAD. VxWorks classic
`readyQHead` is a `MULTI_Q` — an array of Q_HEADs indexed by priority.
That would not show up as a single Q_HEAD scan.

## Why pivot still has merit

The original premise — Firedrake daemons (`fdSockMgr`, `fdServer`,
`fdShExec`, etc.) are sufficient for stage-1 toolkit recognition —
remains untested but unfalsified. We didn't get past the wake step.
The toolkit-recognition test was the right *strategy* but blocked
by the same gate-9 wake mechanism.

The Track A patch removes the "did we model the wake correctly"
question from the path: TCB status is undeniably READY. So the next
session's blocker is clean: **find the readyQ enqueue function** and
either (a) call it from QEMU after the TCB writes, or (b) manually
splice into whatever data structure it touches.

## Recommended next moves

1. **Disasm the body around `0x002fe918`** (sem-block PC where
   tFecEndRx is stuck). The block routine calls some `windPendQGet`-
   alike that removes the task from readyQ. The reverse function
   `windReadyQPut` is the symbol we want.
2. **Disasm `0x002ff5c4..0x002ff7d4`** (semGive body, already
   sampled by SEM-HIST). When semGive successfully wakes a task,
   it adds it to readyQ — that call site reveals the enqueue fn.
3. **Or: instrument PC histogram during a context switch.** When
   sysClkInt fires and a higher-priority READY task exists, the
   scheduler should pick it. Sample PCs in the windExit body
   (runtime address unknown — bootrom is at `0x010d3640`, runtime
   is in `0x002xxxxx` range somewhere).
4. **Cleanup:** the `READYQ-SCAN` block can be deleted once we have
   the readyQ address; no further BSS-scanning value.

## Time / status

- Track A: ~60 min (longer than 45 min budget due to splice
  correction).
- Track B: ~10 min.
- Track C.1: skipped.
- Documentation + commit: ~20 min.

This session does not advance gate 9 toward closure but **falsifies
one cheap shortcut** (manual TCB write) and identifies the precise
remaining problem (readyQ enqueue).
