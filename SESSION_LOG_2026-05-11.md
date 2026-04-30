# Session 2026-05-11 — netJobAdd injection works, byte-level still empty

Plan continuation from `SESSION_LOG_2026-05-10.md`. Executed the
"pivot to netJobAdd injection" plan from the post-readyQ-unblock
context.

## TL;DR

- ✅ **netJobAdd sig-matched at runtime `0x0022c288`.** Plus
  `netTask` at `0x0022c0fc`, BSS `netJobInfo` at `0x00980a34`,
  `intLock`/`intUnlock` at `0x00138a68`/`0x00138a7c`. Cross-checked
  by full-bytes match of bootrom `netJobAdd` (0x010442dc, size 0x1b0)
  against runtime; bootrom and kernel diverge in absolute addresses
  but the prologue (0x40 bytes with branch + lis/addi/lwz masking)
  is unique.
- ✅ **tFecEndRx entry-PC at `0x0012e268`** (TCB+0x74). First
  blocking call: `semTake @ 0x002ff730` with sem from `r3+1248`.
  Struct layout discovered: sem at +1248, work-flag at +848 (must
  be 8 to enter work-path on wake), cleanup arg at +1252. Struct
  ptr back-computed = `0x07BED9B4` (sem 0x07bee080 stored at +1248).
- ✅ **Extended sysClkInt tail-patch shim** (33 insns @ `0x002acef0`).
  Two doorbells, in priority order: netjob_func (tail-call to
  netJobAdd with args) → sem_queue (existing qPriBMapPut wake).
  Each clears its doorbell before tail-call. Both preserve sysClkInt
  continuation in r12 / mtlr before tail-call.
- ✅ **Hypercall MMIO layout extended** (FSHOOK_REG_SIZE 0x20 → 0x40):
  - `0xF0004020` NETJOB_FUNC (doorbell — non-zero = active)
  - `0xF0004024..0x4034` NETJOB_ARG1..ARG5 (5 args r4..r8)
  - `0xF0004038` NETJOB_PROBE (host-readable; guest-stub writes
    `0xCAFEBABE` here when invoked)
- ✅ **Probe stub at `0x002acf80`** (13 insns / 52 bytes). Stores
  `*0x07BEDD04 = 8` (struct+848 work-flag), writes `0xCAFEBABE` to
  `NETJOB_PROBE`, calls `semGive(0x07bee080)`, returns.
- ✅ **Full chain proven: netJobAdd → netTask deferred-call works.**
  At vt=8s NETJOB-DOORBELL armed → next sysClkInt fired the shim →
  tail-called netJobAdd → enqueued our descriptor → semGave
  netTaskSem. tNetTask awoke, processed our deferred entry, called
  the stub via `blrl` (LR=`0x0022c220` = post-`blrl` site in netTask
  job-loop). Stub fired (NIP=`0x002acfa0` = stw probe-flag insn).
  `0xCAFEBABE` landed in MMIO. **Hard pass on the deferred-call
  primitive.**
- ✅ **RX-walker armed.** Each RX frame (in addition to existing
  sem_queue + readyQ-force writes) sets `s->fshook.netjob_func =
  0x002acf80`. Reactive injection per frame.
- ❌ **Byte-level reply still empty.** `nc 127.0.0.1 {2121, 9482,
  17185, 8080}` all timeout. pcap shows host ARPs unanswered (same
  as last session). 0 frames from `src 169.254.254.15` post-boot.
- 🟡 **Pinpointed sub-problem.** The wake-stub's `semGive(0x07bee080)`
  from netTask context completes (no exception trace), but tFecEndRx
  doesn't enter its main RX-processing path. After the wake-stub
  fires once, subsequent NETJOB-DOORBELL re-arms aren't picked up
  past vt~10s — sysClkInt itself stops firing reliably (same
  scheduler-degeneration symptom from last session).

## Plan outcome

Per the plan's plausible-outcomes section, this is **MIDDLE (~40%)**:
> netJobAdd callable, jobs enqueue, tNetTask processes them (PC
> moves into IP/network code), but no TX. Need to find the right
> function to pass — `muxReceive` / `ipReceive` / `endRcv` — for
> a future session.

Slight caveat: PC moves into our deferred function (which calls
semGive), not yet into IP/network code proper. The next-session
work is to swap the deferred-call function for one that actually
drives the RX path — see Next steps.

## Symbol-matching method (refined from last session)

Improved Python helper now correctly handles the runtime ELF's
file layout (LOAD at VA 0x100000, file off 0x80). Search restricted
to kernel `.text` (file 0x80..0x808390) instead of the embedded-
bootrom region past the LOAD segment. Mask logic:

- `op==18` (b/bl): mask LI 26-bit field, keep AA/LK
- `op==16` (bc): mask BD 14-bit field, keep BO/BI/AA/LK
- `op==15` (lis/addis): mask 16-bit imm
- `op==14` (addi): mask 16-bit imm
- `op∈{32..47}` (lwz/stw/lhz/etc.): mask 16-bit displacement

Found:

| Symbol             | Bootrom      | Runtime      |
|--------------------|--------------|--------------|
| netJobAdd          | `0x010442dc` | `0x0022c288` |
| netTask            | `0x01044150` | `0x0022c0fc` |
| netJobInfo (BSS)   | `0x011f750c` | `0x00980a34` |
| intLock            | `0x01001058` | `0x00138a68` |
| intUnlock          | `0x0100106c` | `0x00138a7c` |
| m5200FecInt        | `0x010e3b44` | `0x0012a7f0` |
| semGive            | (—)          | `0x002ff5c4` |

Failed sig-matches (only matched in embedded-bootrom region of
runtime ELF, not kernel `.text`): `m5200FecPollReceive`,
`m5200FecRxDrainIsr`, `m5200FecSend`, `m5200FecRestart`,
`ipReceiveRtn`, `muxReceive`, `endRcvRtnCall`. These functions
likely exist in the kernel under different names, or the runtime
build inlined them.

## Job-descriptor layout (verified from runtime netTask disasm)

netJobAdd called as `netJobAdd(func, arg1, arg2, arg3, arg4, arg5)`
(r3..r8). Stores:

| Offset | Field   |
|--------|---------|
| `+0x00` | next    |
| `+0x04` | func    |
| `+0x08` | arg1    |
| `+0x0c` | arg2    |
| `+0x10` | arg3    |
| `+0x14` | arg4    |
| `+0x18` | arg5    |

Then semGives `netJobInfo+12` (= 0x00980a40, the netTaskSem). When
the immediate-call path is taken (no free desc), netJobAdd loads
the args r3..r7 from offsets 4..20 and `mtlr r0; crclr 6; blrl`s
the func — so the deferred function receives `(arg1, arg2, arg3,
arg4, arg5)` with 5 args (r3..r7), not 6.

netTask's deferred-call site at `0x0022c1f8..0x0022c220`:
```
22c1f8: lwz  r9, 4(r31)     ; func ptr
22c1fc: addi r29, r29, 1
22c200: lwz  r3, 8(r31)     ; arg1
22c204: lwz  r4, 12(r31)
22c208: mtlr r9
22c20c: lwz  r5, 16(r31)
22c210: lwz  r6, 20(r31)
22c214: lwz  r7, 24(r31)
22c218: crclr 6
22c21c: blrl                ; <- our stub runs here
22c220: lwz  r0, 0(r31)     ; <- LR observed in our probe (= 0x0022c220)
```

## Shim layout (`0x002acef0`, 33 insns / 132 bytes)

```
+0x00 mflr r12
+0x04 bl   intUnlock                          ; 0x00138a7c
+0x08 lis  r10, 0xF000
+0x0C ori  r10, r10, 0x4000                   ; FSHOOK base
+0x10 lwz  r3,  0x20(r10)                     ; netjob_func
+0x14 cmpwi r3, 0
+0x18 beq  +0x40                              ; -> sem-queue path
+0x1C..2C lwz r4..r8 from 0x24..0x34(r10)     ; args 1..5
+0x30 li   r0, 0
+0x34 stw  r0, 0x20(r10)                      ; clear netjob_func
+0x38 mtlr r12
+0x3C b    netJobAdd                          ; tail-call → 0x0022c288

+0x40 lwz  r3, 0x1c(r10)                      ; sem_queue (existing)
+0x44 mtlr r12
+0x48 cmpwi r3, 0
+0x4C beq  +0x80                              ; -> blr
+0x50 li   r9, 0
+0x54 stw  r9, 0x1c(r10)                      ; clear sem_queue
+0x58 lis  r9, 0x07BF; +0x5C addi r9, r9, -0x21C8 ; TCB
+0x60 li   r0, 0
+0x64 stw  r0, 0x3C(r9)                       ; TCB.status = 0
+0x68 stw  r0, 0x5C(r9)                       ; TCB.pSemId = 0
+0x6C lis  r3, 0x009A; +0x70 addi r3, r3, -0x50A8 ; readyQ
+0x74 mr   r4, r9                             ; TCB
+0x78 li   r5, 29                             ; priority
+0x7C b    qPriBMapPut                        ; tail-call → 0x002cd8e0

+0x80 blr                                     ; no-doorbell path
```

Each path clears its doorbell BEFORE tail-call so subsequent ticks
don't re-fire on the same arg. Tail-calls use `b` (no link) with
`mtlr r12` first so the called function's blr returns to sysClkInt
continuation.

## Probe stub (`0x002acf80`, 13 insns / 52 bytes)

```
+0x00 lis  r9, 0x07BE; +0x04 ori r9, r9, 0xDD04   ; r9 = 0x07BEDD04
+0x08 li   r10, 8
+0x0C stw  r10, 0(r9)                              ; *(struct+848) = 8
+0x10 lis  r9, 0xF000; +0x14 ori r9, r9, 0x4038   ; NETJOB_PROBE MMIO
+0x18 lis  r10, 0xCAFE; +0x1C ori r10, r10, 0xBABE
+0x20 stw  r10, 0(r9)                              ; probe = 0xCAFEBABE
+0x24 lis  r3, 0x07BE; +0x28 ori r3, r3, 0xE080   ; sem ID
+0x2C bl   semGive                                 ; 0x002ff5c4
+0x30 blr
```

Verified by readback at boot: probe @ `0x002acf80` =
`3d2007be 6129dd04 39400008 91490000` (matches first 4 insns).

## Ground truth from `/tmp/qemu_byte_test.log`

- `NETJOB-PROBE: flag flipped, value=0xcafebabe` fires at vt=4-9s
  depending on arming time (NIP=`0x002acfa0` = probe+0x20 = the
  `stw r10, 0(r9)` writing 0xCAFEBABE to MMIO; LR=`0x0022c220`
  = netTask post-blrl).
- TCB-WATCH at vt=8.5s: `tFecEndRx ... status=0x0 READY ... PC=0x002fe918`
  — task wakes via the sem-queue qPriBMapPut path, but its saved
  PC stays at semTake's block PC. Stays READY but un-dispatched
  through vt=30s.
- Without sem-queue arming (netjob alone), tFecEndRx eventually
  flips PEND→READY at ~vt=12.5s with sem=0x07bee080 still set —
  partial wake from another path, but again not dispatched.
- pcap from final 40s run with host probes: 1 frame from src
  `00:1b:f0:00:00:0a` (the boot-time gratuitous ARP). 0 frames
  during the probe window. Hard fail on Track 2 byte-level goal.

## Why semGive in netTask context doesn't drive TX

Hypotheses (in order of plausibility, no direct evidence yet):

1. **semGive vtable-dispatches via workQ-deferred path.** The
   semGive @ 0x002ff5c4 body (lines 0x2ff5d8..0x2ff608) does:
   `andi. 0, r31, 1; bt 0x2ff610` (deferred type — early return),
   else look up vtable @ `0x008e95a4 + (sem.type & flags)*4`,
   `mtlr; crclr 6; blrl`. If our sem 0x07bee080's vtable entry is
   the workQ-deferred giver (same path that broke last session's
   semFlush experiments), the give NEVER drains in our run.

2. **tFecEndRx's saved register state inconsistent.** The kernel's
   semTake epilogue checks `cmpwi r3, 1` at `0x002fe918`. If our
   wake (qPriBMapPut hack via shim) leaves saved r3 ≠ 1, the task
   takes the false-branch to function epilogue and returns from
   semTake with whatever value — bypassing the work-flag check at
   `r31+848`. Then the task either exits or enters cleanup.

3. **struct+848 flag races with kernel.** Our stub writes 8 there
   in netTask context. If the kernel re-initializes the struct (or
   tFecEndRecover restarts tFecEndRx between vt=14s and our wake
   takes effect), the flag gets zeroed. tFecEndRx's flag check
   sees 0, takes cleanup path.

4. **sysClkInt stops firing past ~vt=11s.** Logged behavior: shim's
   `lwz r3, 0x20(r10)` (netjob_func read) fires every ~12ms until
   vt~11.5s, then never. Coincides with PSC1/4/5 init bursts. Some
   kernel state (maybe MSR.EE=0 holding from a critical section)
   prevents DEC delivery. This compounds problem #1: even if a
   NETJOB-DOORBELL re-arm at vt=14s would otherwise drive TX, the
   shim never runs to pick it up.

## Files modified

- `hw/ppc/mac_newworld.c`:
  - `FSHOOK_REG_SIZE` extended 0x20 → 0x40 + new reg defs
  - `MPC5200State.fshook` adds `netjob_func`, `netjob_args[5]`,
    `netjob_probe`
  - MMIO read/write switch extended for new regs (NETJOB_PROBE
    write logs "NETJOB-PROBE: flag flipped …" to stderr)
  - `mpc5200_apply_keyswitch_patches`: stub_words[33] replaces the
    21-insn shim; new probe_words[13] written at 0x002acf80;
    post-patch verify dumps both
  - Boot-stations table: added entries for 0x002acf80, 0x0022c288
    (netJobAdd), 0x0022c0fc (netTask)
  - Periodic timer: ENTRY-PROBE doc dump at vt=2s; NETJOB-DOORBELL
    arming at vt=8/9/10s if doorbell idle; NETJOB-WATCH every 0.5s
    vt=4..20s
  - RX-walker (in `mpc5200_bestcomm_rx_hook`): arms NETJOB doorbell
    on every RX frame (in addition to existing sem_queue +
    READYQ-FORCE-RX writes)

No commits taken. All changes uncommitted on `mpc5200-stub` per
global CLAUDE.md (no auto-commit).

## Next steps

In dependency order, biggest leverage first:

1. **Find m5200FecPollReceive's runtime VA via call-site analysis.**
   m5200FecInt is at runtime `0x0012a7f0`. Its body at +0x18 reads
   `lwz r9, 700(r3)` (FEC MMIO base) — confirming its arg is an
   END_OBJ-with-FEC-MMIO struct. Following calls forward from
   `0x0012a7f0` should lead to `m5200FecPollReceive`-equivalent
   code in the runtime. Once found, swap the deferred function
   from our wake-stub to that. Replaces the semGive hypothesis
   with a direct "process the BD ring now" call.

2. **Find the right END_OBJ for m5200FecInt.** Our struct ptr
   `0x07BED9B4` (back-computed from tFecEndRx's sem) has +700 =
   `0x0000B032` (small int, NOT FEC MMIO). m5200FecInt's r3 is a
   different struct. Search runtime BSS / heap for any addr X
   where `*(X+700)` is in `0xF0003000..0xF0003400` (FEC MMIO). That
   X is m5200FecInt's expected arg.

3. **Plan B fallback (if (1) and (2) blocked):** synthesize ARP
   replies entirely in QEMU's RX walker. When we see an ARP
   request for `169.254.254.15`, build the reply frame in-place
   and inject via the FEC TX BD ring (or directly to the slirp
   side via QEMU's own NIC API). Less authentic but unblocks
   toolkit-recognition (Track C.2).

4. **Investigate sysClkInt-stop-past-vt~11s.** Independent of the
   netjob path. If we can keep sysClkInt firing past the PSC init
   bursts, we get a much wider window for reactive injection
   (RX-walker netjob arming would actually be picked up). Look at
   MSR.EE state during the dead window — if EE=0 holds, find what
   set it and didn't restore.

5. **Toolkit-recognition test (Track C.2).** Still blocked until
   any guest-originated frame leaves the run. Stretch.

## Time

- ~3h. Sig-matching: ~30 min (had to figure out the runtime ELF
  file-offset / VA mapping; wrong load base wasted the first two
  attempts). TCB.entry probe + struct dump: ~30 min. Shim build +
  probe stub + MMIO regs: ~60 min. RX-walker integration + iterating
  on arming time / disabling sem-queue / re-enabling: ~60 min.

## Risks captured

- The new shim is bigger (132 bytes vs 84) and lives in the freed
  printf-body region at `0x002acef0..0x002acf73`. The probe stub
  occupies `0x002acf80..0x002acfb3`. If the runtime ever re-uses
  that printf body (e.g. via a future patch we apply elsewhere),
  collisions. Currently the boot-stations table covers it.
- The probe stub's write to `*0x07BEDD04 = 8` happens unconditionally
  on every netjob-fire. If `0x07BEDD04` is invalidated (struct
  freed) by a kernel restart, this is a stray write. Probably
  benign because the heap region 0x07BE0000+ is reserved for
  tFecEndRx-related state, but worth re-checking if we see odd
  crashes.
- semGive(0x07bee080) from netTask context might recursively
  trigger more deferred work that re-enters our shim (sem_queue
  doorbell could be set by another path). The two-path priority
  in the shim handles this — netjob wins, sem_queue waits a tick.
  No observed loop in the current run.
- Hard-coded addresses in the wake-stub (`0x07BEDD04`, `0x07bee080`)
  are runtime-specific. If the BSP build changes (different turbine
  firmware revision), all of these need re-derivation. Same risk as
  last session.
