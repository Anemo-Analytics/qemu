# Session 2026-05-03 — usrRoot stall fixed, FS hook fires, Vestas stack alive

Plan: in-message plan for this session ("Find & unblock the usrRoot
stall"). Followed Phases A–E.

## TL;DR (session-end)

- **Phase A (bracket the stall):** ✅ via stack-walker, NOT the
  bl-station instrumentation. Adding 18 single-instruction stations
  at `0x107a08..0x107a68` did nothing — the 100µs sampler is too
  coarse to catch one-shot `bl` insns. Switched approach: extended
  the per-second task-list dumper to walk tRootTask's PowerPC
  back-chain when status==PEND. That gave the full call chain in
  one tick. Result: tRootTask is `bl usrToolsInit` (0x107a4c) →
  function 0x1076b0 (final bl) → banner-printer 0x106594 (3rd
  printf call) → printf wrapper `0x2acee8` → fioFormatV `0x2ad5f4`
  → ... → kernel sem block at `0x2fe918`. **The stall is in
  printf, not in any CAN init.** Items 1–13 of the 18 usrRoot calls
  succeeded; item 14 (`bl usrToolsInit`) is the blocker.
- **Phase B (root-cause):** ✅. The first two printfs in 0x106594
  went through; the third blocks. PSC1 W trace shows the BSP did
  PSC1 *config* writes (mode/CR/CSR), but **no actual TX data byte
  has been written**. So the stall is upstream of the UART itself,
  inside the iosLib `write()` path waiting on a sem that's only
  posted by a PSC1 TX-empty IRQ — and our QEMU has zero PSC IRQ
  wiring. The first 2 printfs likely fit a small queue; the 3rd
  stalls when the queue drains too slowly (drain rate = 0).
- **Phase C (fix):** ✅ **printf no-op patch** at `0x2acee8`. Two
  instructions: `li r3, 0; blr`. Bypasses the entire blocking
  iosWrite path. Direct PSC1 TX writes (sysSerialHwInit etc.) still
  go through and echo via the existing PSC1 W +0x0c|+0x40 trap.
  Wiring real PSC1 IRQs is bigger than this session — printf no-op
  gets us moving.
- **Phase C.5 (timing fix):** ✅ moved `mpc5200_apply_keyswitch_patches()`
  from first SLT-tick (delayed 1s of guest time) to the first
  diag-sample (1µs). Patches were going in *after* tRootTask had
  already entered printf. Now they install before any vxworks code
  runs.
- **Phase D (verify):** ✅ **all pass criteria met, plus stretch.**
  - 30+ `FS_HOOK:` lines fire including:
    - `open("/fs/manifest.csv") -> fake_fd=1000` + 9× `read()` calls
      pulling 8.7 KiB of real bytes from the host fs
    - `open("/fs/etc/fdconfig.def") -> fake_fd=1000` + 1× `read()`
      pulling 734 bytes
    - `open("/fs/etc/startup.fail") -> fake_fd=1000` + 1× `read()`
      (4 bytes — the BSP's failure counter)
  - Stretch goal hit: **Firedrake task family alive** — `fdUpg`,
    `fdSockMgr`, `fdMsgHandler`, `fdServer`, `fdNgbors`, `fdShExec`
    all in the live task list (35 tasks total, up from 21).
  - Other new tasks: `tArcRcv`, `tCapReceive`, `tCapTimeout`, `tNAT`,
    `tTimeLog`, `tOsStatusService`, `tRandom`, `tShutHook`,
    `tRtcControl`, `CronTask`, `tShell`.

## What changed in code

- `hw/ppc/mac_newworld.c`:
  - 18 boot-station entries for usrRoot's bl call sites (kept for
    documentation even though they don't reliably fire — the
    stack-walker is the better tool).
  - tRootTask back-chain stack-walker in the per-second task-list
    dumper (only triggers for `tR*`-named tasks in PEND state, so
    no impact on other dumps).
  - `printf_stub` (`li r3, 0; blr`) installed at `0x2acee8` from
    `mpc5200_apply_keyswitch_patches`.
  - Patch installer moved from `mpc5200_tick` (first fire delayed
    1 s by design) to the first call of `mpc5200_diag_sample`
    (fires at 1µs).

No changes to `run.sh`. No changes to symbol/disasm files. No new
files.

## Verification

```bash
cd /home/kasper/qemu
ninja -C build qemu-system-ppc
LOG=/tmp/qemu_5c.log TIMEOUT=75 ./run.sh
grep -c 'FS_HOOK:' /tmp/qemu_5c.log              # ~60
grep '^FS_HOOK: open("/fs' /tmp/qemu_5c.log      # multiple host fs hits
grep -E 'fdUpg|fdSockMgr|fdServer' /tmp/qemu_5c.log | head
```

Pass = ≥1 `FS_HOOK:` line. Stretch = `fd*` daemons in task list.
Both hit.

## Gate-status board (delta)

- **Gate 6 (banner):** ❌ unchanged. Direct PSC1 banner still not
  emitted — and now we explicitly suppress all printf so the BSP's
  log goes to the void. Side effect of Phase C; acceptable trade.
- **Gate 7 (FS available):** ✅ **closed.** `/fs/...` is reachable
  from the BSP via the hypercall hook; the BSP reads real config
  data (manifest.csv, fdconfig.def, startup.fail) into guest RAM.
- **Gate 8 (Vestas app boots):** 🟡 **in flight.** Firedrake daemons
  are alive (`fdUpg`, `fdSockMgr`, `fdMsgHandler`, `fdServer`,
  `fdNgbors`, `fdShExec`). Not yet seeing `tApMain` / `tFirecrest`
  by name, but that may be a naming/loading thing — startup.app's
  loadable modules aren't being processed yet (no `/fs/etc/startup.app`
  open observed; the BSP took a different early-config path).
- **Gate 9 (network listeners):** likely improved (fdServer / fdSockMgr
  alive) — not re-verified this session.

## What's next

1. **Make `fs-exists` (0x13ffe4) honest.** Right now it returns 1
   for every path. The BSP probes `/fs/etc/force_safe_mode`; if we
   say it exists, the BSP enters safe-mode and never opens
   `/fs/etc/startup.app`. Trivial fix: route `fs-exists` through
   the same FS-hook doorbell so it does a real `access()`.
2. **PSC1 TX IRQ wiring.** Replacing the printf no-op with a proper
   IRQ would restore boot-banner visibility AND avoid suppressing
   diagnostics for the rest of the BSP.
3. **`/zlfs/` translation + zlib decompression** for the
   `loadModule` path used by startup.app's `ld` lines.

## Risks accepted

- **printf no-op silences logs.** Anything the BSP `printf`s past
  this point is invisible. Acceptable: we have the FS-hook and task
  dumper for diagnostics. Will revisit when the IRQ work is done.
- **Stack-walker is heuristic.** Triggers on `tR*` tasks in PEND;
  could miss a future stall in a task with a different name. Easy
  to widen.
