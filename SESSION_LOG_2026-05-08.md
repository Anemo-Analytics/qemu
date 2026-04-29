# Session 2026-05-08 — two-track push: gate 8 advances big, gate 9 narrows further

Plan: parallel tracks toward gate 12. Track 1 = chase gate-9 dispatch
hunt by dumping the class-table fn semFlush dispatches to at vt=10s.
Track 2 = make `fs-exists` (`0x0013ffe4`) honest — route through the
FS-hook hypercall instead of always returning 1, on the theory that a
real -1/+1 from `access()` flips the BSP off the safe-mode path.

Result: **Track 2 is a strong advance** — gate 8 task list grows from
~21 to **30** entries, BSP now actively probes the runtime FS and reads
config files. Track 1 narrows the gate-9 blocker further but doesn't
close it: the actual semFlush call site is the BSS-flag-gated hook fn
`0x002ffdd8`, not the class table. Both the hook fn and class[7] are
"validate-and-error" stubs that error out before reaching the workQ
enqueue at `0x00302598`.

## TL;DR

- ✅ **Track 2 (gate 8 advance) — FS_EXISTS hypercall installed.**
  `0x0013ffe4` now writes args to the doorbell and runs `access(F_OK)`
  on the host, returns `1` on hit and `0` on miss. BSP now sees an
  honest miss for `/fs/etc/force_safe_mode`, takes the runtime path,
  and proceeds to probe many more files. **22 new tasks** in the task
  list at vt=58s vs the 2026-05-07 baseline.
- 🟡 **Track 1 (gate 9) — class-table reading falsifies one
  hypothesis, identifies the surviving cause.** SEM-CLASS shows
  `class[7] @ 0x008d9580 = 0x002ffe68`. SEM-GLOBALS shows
  `*(0x0090879c) = 0x00000001`. semFlush's slow path checks that
  global at `0x002ff9a0`/`bne 0x2ff9d8` and tail-calls **the hook fn
  at `0x002ffdd8`, NOT the class table**, when the global is set.
  Disasm of `0x002ffdd8` reveals it's a "validate-and-error" stub
  that compares qHead to a sentinel `*(0x008d951c)` and errors out
  for non-matching states. The actual workQ enqueue is at
  `0x00302598` (called from `0x002ffe1c` after a successful validate),
  but our path errors out at the validation step. Class[7] at
  `0x002ffe68` is a similar validate-and-error stub.
- 🟡 **`tApMain`/`tFirecrest` still not seen by name.** BSP now opens
  `/fs/etc/fdconfig.def` and `/fs/etc/startup.fail` but never gets
  to `/fs/etc/startup.app`. Probably one more "I'm in safe-mode"
  signal somewhere, or the script runner is gated on a sem the BSP
  is still PEND'd on.

## Track 2 — FS_EXISTS hypercall (gate 8 advance)

### Code

`hw/ppc/mac_newworld.c`:

- `FS_EXISTS = 6` cmd code added after `FS_FSTAT = 5`.
- `fshook_handle_exists(s)` mirrors `fshook_handle_open` but uses
  `access(host, F_OK)` instead of `open()`. Returns `1` on hit, `0`
  on miss; logs both.
- Dispatched from `fshook_dispatch` (`case FS_EXISTS:` at line ~1748).
- Old 24-byte trace stub at `0x0013ffe4` removed; replaced with the
  same 36-byte 9-instruction hypercall stub used by open/read/close,
  via `stub[23] = FS_EXISTS` in the shared template block.

### Evidence

```
FS_HOOK: exists("/fs/etc/force_safe_mode") -> 0  (host=".../fs/etc/force_safe_mode": No such file or directory)
FS_HOOK: open("/fs/manifest.csv") -> fake_fd=1000 host_fd=19
FS_HOOK: open("/fs/etc/fdconfig.def") -> fake_fd=1000 host_fd=19
FS_HOOK: open("/fs/etc/startup.fail") -> fake_fd=1000 host_fd=19
FS_HOOK: open("/fs/etc/net.site") -> -1 (host ".../fs/etc/net.site": No such file or directory)
FS_HOOK: open("/fs/etc/zt.cfg") -> -1 (host ".../fs/etc/zt.cfg": No such file or directory)
FS_HOOK: open("/fs/etc/fdconfig.site") -> -1 (host ".../fs/etc/fdconfig.site": No such file or directory)
FS_HOOK: open("/fs/performance_data.bin") -> -1 (host ".../fs/performance_data.bin": No such file or directory)
```

Other fs-exists probes the BSP issues that the translator rejects
because they're not under `/fs/`:

- `/factory/osxversion`, `/turbine/osxversion`,
  `/zlfs/boot/vxworks.romfs.zlfs`

These return `0` (not under `/fs/`) which is the correct semantic for
the BSP — those mounts don't exist.

### Task list at vt=58s

```
[ 0] tExcTask         prio=  0 PEND  ...
[ 1] tLogTask         prio=111 PEND  ...
[ 2] CpuloadLow       prio=255 READY ...
[ 3] CpuloadHigh      prio=  2 DELAY ...
[ 4] confLogMsg       prio=111 DELAY ...
[ 5] tLed             prio= 61 PEND+TO ...
[ 6] tWatchdog        prio=  0 DELAY ...
[ 7] tNetTask         prio= 99 PEND  sem=0x00980a48
[ 8] tFecEndRecover   prio=110 PEND+TO ...
[ 9] tFecEndRx        prio= 29 PEND  sem=0x07bee080  ← still stuck (gate 9)
[10] tPortmapd        prio=100 PEND
[11] tMountd          prio=100 PEND
[12] tNfsd            prio=110 PEND
[13] tNfsd3           prio=115 PEND
[14] tNfsd2           prio=115 PEND
[15] tNfsd1           prio=115 PEND
[16] tNfsd0           prio=115 PEND
[17] tSntpsTask       prio=100 PEND
[18] tWdbTask         prio=  3 PEND
[19] tShutHook        prio= 61 PEND
[20] tRtcControl      prio= 62 PEND+TO
[21] CronTask         prio=250 PEND+TO
[22] tDcacheUpd       prio=250 DELAY
[23] tArcService      prio= 11 PEND+TO
[24] tArcFastRx       prio=  1 PEND
[25] tArcRcv          prio= 49 PEND
[26] tCapReceive      prio= 50 PEND
[27] tCapTimeout      prio= 50 PEND
[28] tNAT             prio= 99 PEND+TO
[29] tTimeLog         prio=200 DELAY
```

Compared to 2026-05-07, the new family includes the Arc protocol
(`tArcService`, `tArcFastRx`, `tArcRcv`), Cap protocol (`tCapReceive`,
`tCapTimeout`), `tNAT`, `tTimeLog`, `tDcacheUpd`. The Firedrake daemon
family from 2026-05-03 (`fdUpg`, `fdSockMgr`, etc.) isn't in this
specific dump but is likely later in the run; the activeQ wraps so
60-task lists need stitching to be exhaustive.

## Track 1 — gate-9 dispatch hunt

### SEM-CLASS dump (Step 1.A)

```
SEM-CLASS: class table base = 0x008d9564
SEM-CLASS:   class[0] @ 0x008d9564 = 0x002fffbc
SEM-CLASS:   class[1] @ 0x008d9568 = 0x002ffe68
SEM-CLASS:   class[2] @ 0x008d956c = 0x002fffbc
SEM-CLASS:   class[3] @ 0x008d9570 = 0x002ffe68
SEM-CLASS:   class[4] @ 0x008d9574 = 0x002ffe68
SEM-CLASS:   class[5] @ 0x008d9578 = 0x002ffe68
SEM-CLASS:   class[6] @ 0x008d957c = 0x002ffe68
SEM-CLASS:   class[7] @ 0x008d9580 = 0x002ffe68
```

Two distinct entries: `0x002fffbc` and `0x002ffe68`. tFecEndRx (class
byte 0x07) → `0x002ffe68`.

### SEM-GLOBALS dump (Step 1.C)

```
SEM-GLOBALS: 0x008d5c60 kernelState            = 0x00000000
SEM-GLOBALS: 0x0091879c sem-glob-91879c        = 0x00000000
SEM-GLOBALS: 0x0090879c sem-glob-90879c        = 0x00000001  ← controls hook
SEM-GLOBALS: 0x009083e8 sem-glob-9083e8        = 0x00000000
SEM-GLOBALS: 0x008f8388 workQ-ctl-8f8388       = 0x00000500
SEM-GLOBALS: 0x0091877c sem-glob-91877c        = 0x00000000
```

`*(0x0090879c) = 1` is the critical one. semFlush at `0x002ff9a0`
reads it, finds it non-zero, and calls the hook fn at `0x002ffdd8`
INSTEAD OF the class-table dispatch.

### Disasm of the hook fn at `0x002ffdd8` (Step 1.B)

```
0x002ffdd8: stwu  r1, -0x10(r1)
0x002ffddc: mflr  r0
0x002ffde0: stw   r0, 0x14(r1)
0x002ffde4: mr    r4, r3                ; r4 = sem_id
0x002ffde8: lis   r9, 0x8e
0x002ffdec: lwz   r11, 0(r4)            ; r11 = *(sem_id+0) = qHead
0x002ffdf0: lwz   r9, -0x6ae4(r9)       ; r9 = *(0x008d951c) = sentinel
0x002ffdf4: cmpw  r11, r9
0x002ffdf8: beq   0x2ffe1c              ; -> dispatch path
0x002ffdfc: cmpwi r11, 0
0x002ffe00: beq   0x2ffe10              ; -> errno set
0x002ffe04: lwz   r0, 0x24(r9)
0x002ffe08: cmpw  r11, r0
0x002ffe0c: beq   0x2ffe1c              ; -> dispatch path
0x002ffe10: bl    0x2acb40              ; errno helper
0x002ffe14: lis   r0, 0x3d
0x002ffe18: b     0x2ffe4c              ; common error tail
0x002ffe1c: ; ── DISPATCH (only entered when qHead matches sentinel) ──
0x002ffe1c: lbz   r0, 4(r4)             ; class byte
0x002ffe20: lis   r9, 0x8e
0x002ffe24: addi  r9, r9, -0x6a5c       ; r9 = 0x008d95a4 (SECOND class table)
0x002ffe28: slwi  r0, r0, 2
0x002ffe2c: lwzx  r3, r9, r0            ; r3 = *(0x008d95a4 + class*4)
0x002ffe30: cmpwi r3, 0
0x002ffe34: beq   0x2ffe44              ; -> errno set
0x002ffe38: bl    0x302598              ; ── workQ enqueue ──
0x002ffe3c: li    r3, 0
0x002ffe40: b     0x2ffe58              ; success
0x002ffe44..58: error tail (writes errno, returns -1)
```

For tFecEndRx: `qHead = 0x07bede38` (real TCB+0x20). Sentinel at
`*(0x008d951c)` is some BSS address. Neither comparison matches, so
the branches go: `r11 != sentinel` → fall through, `r11 != 0` → fall
through, `r11 != *(sentinel+0x24)` → fall through, drops into the
errno path at `0x002ffe10`. **No workQ enqueue happens.**

### Disasm of `0x00302598` (the workQ enqueue, never reached)

Confirmed via radare2: stwu/mflr/save r26-r31 prologue, then
`mr r27, r3 ; mr r26, r4` to save (fn_ptr, sem_id) args, intLock
(`bl 0x138a68`), bump byte counter at `*(0x00908854)`, store
`r27, r26` into a slot at `0x009a5a24 + counter*4`, intUnlock
(`bl 0x138a7c`), clear `*(0x009083e8)`. Classic workQ producer.

The wake side is the consumer that drains this queue — likely
`workQDoWork` or similar, dispatched by `windExit`. Per 2026-05-07,
"workQAdd defers wake; windExit/workQ-drain never dispatches it" was
the surviving cause. Now we know **why** the wake was deferred:
because validation passed and dispatch reached `0x302598`. Wait —
in our case validation FAILS, so dispatch never reaches `0x302598`.

That contradicts 2026-05-07's reading. Possible reconciliation:
2026-05-07's `0x002ff884:1` SEM-HIST hit on semFlush prologue could
be from a DIFFERENT semFlush caller (not our shim) where validation
DID pass. Our 2026-05-08 SEM-HIST shows only `0x002ff5c4:5`,
`0x002ff730:2`, `0x002ff9a0:1` — so the shim's semFlush call
sampled the slow-path entry at `0x002ff9a0` once, but neither the
prologue nor the epilogue. Open question: did the shim's call
actually return cleanly, or did it tail-call somewhere unexpected?

### Class fn at `0x002ffe68` (class[7] in the first table)

Bypassed by the BSS-flag check, but for the record: same shape as
the hook fn — `lwz r3, 0(r3)`, `cmpw r3, sentinel`, three error
paths, no wake path. `class[7]` is essentially a stub for sem
classes the kernel doesn't know how to flush directly. The "real"
class fn is in the SECOND class table at `0x008d95a4`.

### SEM-CLASS2 reading: class[7] in the second table is NULL

Follow-up dump after the initial run, added `SEM-CLASS2:` and
`SEM-SENTINEL:` to the vt=10s one-shot:

```
SEM-CLASS2:  class[0] @ 0x008d95a4 = 0x00300110
SEM-CLASS2:  class[1] @ 0x008d95a8 = 0x00000000
SEM-CLASS2:  class[2] @ 0x008d95ac = 0x00300110
SEM-CLASS2:  class[3] @ 0x008d95b0 = 0x00000000
SEM-CLASS2:  class[4] @ 0x008d95b4 = 0x00000000
SEM-CLASS2:  class[5] @ 0x008d95b8 = 0x00000000
SEM-CLASS2:  class[6] @ 0x008d95bc = 0x00000000
SEM-CLASS2:  class[7] @ 0x008d95c0 = 0x00000000
SEM-SENTINEL: *(0x008d951c) = 0x0099af68 ; *(sentinel+0x24) = 0x00995fe0
```

**Class[7] in the SECOND table is `0x00000000`** — even if we forced
the validation in the hook fn through to dispatch, the lookup at
`0x002ffe2c` (`lwzx r3, r9, r0` with r0 = 7<<2) yields `r3 = 0`,
`cmpwi r3, 0` succeeds, and the path hits the error tail at
`0x002ffe44`. The firmware **literally has no wake fn registered
for sem class 7** in either table.

Class[0] and class[2] both register `0x00300110`. That's likely
the workQ-deferred wake for "binary sem" / "counting sem" in the
windView-style class system. The class byte (`lbz r0, 4(r31)`)
reading 0x07 for all our sems is suspicious — for sems in the
0x07xxxxxx address range, the qTail top byte happens to be 0x07,
so the byte-at-+4 reading is reading a pointer fragment, not a
real class index. The actual sem class is probably encoded in
the `[+0x0C] = 0x008d8fd4` vtable pointer (constant across all
three sems we measured), not the class byte.

So the dispatch via class byte is a dead path for our sems. The
real wake is dispatched via the `*(sem+0xC) = 0x008d8fd4` vtable.
We haven't disassembled what `0x008d8fd4` points at yet — that's
the next investigation.

## Surviving cause for gate 9 (refined)

The wake fails inside `0x002ffdd8` at the validation step. Three
candidate fixes ranked by likelihood:

1. **Set `*(0x0090879c) = 0`.** Bypasses the hook, lands directly in
   the first class table at `0x008d9564`. But class[7] there
   (`0x002ffe68`) has the SAME validate-and-error pattern, just with
   one extra deref at the start. So this likely just shifts the
   error site, not solves it.

2. **Investigate the second class table at `0x008d95a4`.** The hook
   fn dispatches through it after validation passes (`0x002ffe2c`:
   `lwzx r3, r9, r0`). Read `*(0x008d95a4 + 7*4) = *(0x008d95c0)` at
   runtime — that's the actual workQ-bound wake fn for class 7.
   If we tail-call that directly with (sem_id, ...) we sidestep both
   the validation AND the workQ. Risk: that fn may itself bail on a
   different state.

3. **Change `semgive_addr` to a non-semFlush wake primitive.** The
   user's plan flagged this as Track 1.D fallback. Candidates:
   `semBGiveKern` / `semCGiveKern` / `semMGiveKern` — search for
   them by signature in vxworks.out. Risk: may also hit workQAdd
   defer if they share the same wake-path infrastructure.

4. **Patch the validation at `0x002ffdf8/0x002ffe0c` to always
   take the dispatch branch.** Forces the workQ enqueue at
   `0x302598` to happen unconditionally. Then we still depend on
   the workQ drain — back to 2026-05-07's surviving cause.

## What's permanently in code

`hw/ppc/mac_newworld.c`:

- `FS_EXISTS = 6` cmd code (line 131).
- `fshook_handle_exists(s)` (~line 1696) — `access(F_OK)` proxy with
  the same path-translation/logging shape as `fshook_handle_open`.
- `fshook_dispatch` switch updated with `case FS_EXISTS:` (line 1748).
- Old 24-byte trace stub at `0x0013ffe4` removed; replaced with the
  shared 36-byte hypercall stub in the FS_OPEN/FS_READ/FS_CLOSE
  template block (line ~510, `stub[23] = FS_EXISTS`).
- One-shot SEM-CLASS dump in `mpc5200_tick` at vt=10s (8 entries of
  the first class table at `0x008d9564`).
- One-shot SEM-GLOBALS dump in `mpc5200_tick` at vt=10s (six BSS
  globals related to sem state).
- Post-patch verify printf updated: `exists=0x13ffe4` added to the
  install-summary line; old "stubbed /fs/etc/startup.app" wording
  removed.

## Recommended next-session moves

1. **Disasm `0x008d8fd4`** — the constant `[+0x0C]` vtable pointer
   shared by all three sems we measured. This is almost certainly
   the real class object; it points to a structure of fn ptrs
   including `give`, `flush`, and `take`. Reading the first ~32
   bytes of the structure should reveal the wake fn directly.
   ~10 min.

2. **Disasm `0x00300110`** — the workQ-deferred wake registered for
   sem classes 0 and 2 in the SECOND class table. If our sems
   actually classify there (via the vtable, not the class byte),
   this is the wake we'd need to call. If it's a thin wrapper
   over the workQ enqueue at `0x00302598`, we still face the
   workQ-drain problem; if it dispatches differently, there's
   hope. ~15 min.

3. **Switch the shim's tail-call to call the wake via the vtable**
   (or via class[0] of the second table — `0x00300110`). ~15 min.

4. **Verify via TCB-WATCH.** If `tFecEndRx` unblocks, gate 9 closes.

5. **If wake completes but workQ drain still fails**, document via
   the workQ probe noted in 2026-05-07's recommended next-session
   moves — sample `intCnt` and the workQ head/tail.

Track 2 didn't unlock `tApMain` directly. The BSP probes a lot of
config files now but never opens `/fs/etc/startup.app`. Likely needs
one more fs-exists probe to flip another safe-mode flag, or
`startup.app` is gated on something that needs gate 9 closed first.
Document which paths the BSP queries via the existing `FS_HOOK:`
trace; one of them is probably the next "trivial follow-up".

Pragmatic option if neither (1) nor (2) lands a fix: **manually
unblock `tFecEndRx` from QEMU C code.** Write directly to the TCB
to flip status from 0x2 PEND to 0x0 READY, dequeue from the sem's
DLL, requeue on activeQ. Bypasses the firmware's wake machinery
entirely — guaranteed to work but doesn't model the real semantics.
Useful as a diagnostic to confirm "the wake was the only blocker"
before sinking more time into protocol-level fixes.
