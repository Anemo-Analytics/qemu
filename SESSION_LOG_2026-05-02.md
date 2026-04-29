# Session 2026-05-02 — Phase 4b FS-hook hypercall: built, but unreached

Plan: in-message plan for this session (Phase 4b — real `/fs/` hook
via MMIO hypercall).

## TL;DR (session-end)

- **Phase A (confirm read NIP):** ✅. The plan's guess of `0x2b20a8`
  was **wrong** — that address is a small array setter (`if r3 < 2:
  return; globals[r3] = r4`). The real `read(fd, buf, n)` wrapper is
  at **`0x2b18fc`**. Found via VxWorks symbol table embedded in
  `.data` (file offset `0x7f60xx`, 16-byte `{name_va, fn_va, flags,
  0}` entries) — `iosOpen=0x2b2d9c`, `iosRead=0x2b2f00`,
  `iosWrite=0x2b300c`, `iosClose=0x2b2df0`. The thin POSIX wrappers
  (`open=0x2b13c8`, `read=0x2b18fc`, `close=0x2b17d4`) are what we
  hook — they each pass 3 args through to the matching `ios*`
  function and own ≥36 bytes of body.
- **Phase B (MMIO doorbell):** ✅. Doorbell installed at
  **MBAR+0x4000..0x401F** (= guest physical 0xF0004000). Layout:
  `cmd@0`, `arg0@4`, `arg1@8`, `arg2@C`, `result@10`, `errno@14`,
  `trace@18` (diagnostic). Implemented by extending the existing
  `mpc5200_mmio_read` / `mpc5200_mmio_write` callbacks; doorbell
  state lives inside `MPC5200State`.
- **Phase C (host syscall handlers):** ✅. `FS_OPEN`, `FS_READ`,
  `FS_CLOSE`, `FS_LSEEK`, `FS_FSTAT` implemented. fd-proxy table
  (16 slots) maps fake VxWorks fds (1000+i) ↔ host fds. Path
  translation: `/fs/<tail>` → `<root>/<tail>` (root from env
  `MPC5200_FS_ROOT` or default
  `/home/kasper/Vestas/bin/data_dump/firedrake/turbine_dump_node10_roye2/fs`).
  Anything not under `/fs/` returns -1/ENOENT. Every call logs one
  `FS_HOOK: ...` line to stderr.
- **Phase D (BSP entry-point patches):** ✅ **patches installed** at
  `0x2b13c8` (open), `0x2b18fc` (read), `0x2b17d4` (close). Each is
  a 9-instruction (36-byte) hypercall stub: lis/ori → r12 = MMIO
  base, write args + cmd, read result, blr. Uses r0/r12 as PPC ABI
  volatile scratch — LR untouched, so blr returns to the original
  caller.
- **Phase 4a fs-exists stub augmented with TRACE.** The existing
  8-byte stub at `0x13ffe4` (Phase 4a, 2026-05-01) was expanded to
  20 bytes: it now writes its first arg (the path pointer) to a
  diagnostic trace doorbell before returning 1. If 0x13ffe4 ever
  fires, we get an `FS_HOOK: TRACE "<path>"` log line.
- **Phase E (run + iterate):** 🟡. Code is in place, builds clean,
  runs without crash. **Zero `FS_HOOK:` lines fire** in 75 s of
  guest time. Diag-sampler (600 000 × 100 µs samples) confirms:
  none of the FS-gate / runmode / Vestas-app stations are reached.
  See "honest finding" below.

**Honest finding:** the Phase-4b infrastructure is correctly built,
but the BSP **never enters the codepath that would call it**.
Yesterday's premise (gate 7 = FS) was wrong: there is an upstream
gate that prevents `ctvxInit` / `ctvxInitTurbine` / runmode dispatch
from running at all.

## What changed in code

`hw/ppc/mac_newworld.c`:

1. New top-level definitions: doorbell layout (`FSHOOK_REG_*`),
   command IDs (`FS_OPEN`/`FS_READ`/`FS_CLOSE`/`FS_LSEEK`/`FS_FSTAT`),
   fd-proxy constants. `<unistd.h>`, `<fcntl.h>`, `<sys/stat.h>`,
   `<errno.h>` added.
2. New field `fshook` inside `MPC5200State`: doorbell registers +
   16-slot fd proxy table.
3. New helpers (lines ~960–1230):
   `fshook_root` (env lookup), `fshook_read_guest_path`
   (NUL-terminated guest-string reader), `fshook_translate`
   (`/fs/` → host root remap), `fshook_alloc_slot`,
   `fshook_lookup_slot`, plus the five `fshook_handle_*` syscall
   handlers and the `fshook_dispatch` switch.
4. MMIO read/write callbacks now intercept offsets `0x4000..0x401F`
   first (fall-through unchanged afterwards). Writing to `cmd@0`
   triggers the dispatcher; writing to `trace@0x18` logs without
   side effects.
5. `mpc5200_apply_keyswitch_patches` installs three 36-byte
   hypercall stubs and one extended 20-byte trace stub:
   - `0x002b13c8` ← FS_OPEN stub
   - `0x002b18fc` ← FS_READ stub
   - `0x002b17d4` ← FS_CLOSE stub
   - `0x0013ffe4` ← `li r3, 1; blr` + TRACE doorbell write

The boot-station table (`g_boot_stations[]`) wasn't changed — the
existing stations at `0x13ffe4`, `0x14c7d4`, `0x1061c0`, `0x2b13c8`
are exactly what we need to verify reachability, and they all
remain UNREACHED.

## Why no `FS_HOOK:` lines fired

After 75 s of guest time, the boot-station diag sampler reports:

```
UNREACHED [58] 0x00100920 : VX: gate-fn entry (0x100920)
UNREACHED [62] 0x0014adc0 : VX: error-print xref to 0x962e2c
UNREACHED [63] 0x0013ffe4 : VX: fs-exists check entry (stubbed)
UNREACHED [64] 0x0014c7d4 : VX: startup-loop call to fs-exists
UNREACHED [67] 0x001061c0 : VX: script-runner entry (post-banner)
UNREACHED [68] 0x002b13c8 : VX: open() entry
UNREACHED [69] 0x002a5508 : VX: fopen() entry
```

The whole runmode / Vestas-app / FS-gate region of `vxworks.out` is
never executed. Live task list at t=58 s shows the 21 standard
VxWorks daemons (`tRootTask`, `tExcTask`, `tLogTask`, `tWatchdog`,
`tNetTask`, `tFecEndRecover`, `tFecEndRx`, `tPortmapd`, `tMountd`,
`tNfsd`*, `tFtpdTask`, `tSntpsTask`, `tWdbTask`, ...) — and that's
it. The BSP boots, brings up FEC + BestComm + 21 daemons, and
**parks in idle**. There is no gate that we've identified that, when
flipped, makes it transition into runmode init.

## Reverse-engineering breadcrumbs left for the next session

While tracing the call graph this session:

| Address | Symbol | Notes |
|---|---|---|
| `0x14bc14` | `ctvxInitTurbine` | Calls `0x13ffe4` (existence check) at `0x14c4a8`, then `0x1061c0` (script runner) at `0x14c4e8`. **One direct caller**: `0x14d04c`. |
| `0x14cfac` | `ctvxInit` | Calls `ctvxInitTurbine` at `0x14d04c`. **Zero direct `bl` callers** in `.text` — must be invoked indirectly (function-pointer table / task spawn / init array). |
| `0x10cfb8` | (unnamed) | Builds an array of init-fn pointers including `ctvxInit` (0x14cfac) and ~12 others, calls `0x1b6838` = `tffsSocketSetAccessHandlers`. **Zero direct callers** in `.text`. |
| `0x14c8a8` | `ctvxInitFactory` | Sibling. |
| `0x14ce34` | `add_ethernet_alias_if_needed` | Sibling. |
| `0x10c474` ... | `arcnet*`, `MountArcNetNode`, `MountAsAccessPoint` | Nearby code suggests the factory/runmode logic is part of an ARCnet-or-IP node-mode dispatch. The BSP needs to **decide** "I'm runmode N" before any of this fires. |

What we don't know yet: who decides runmode and stores it where. The
strings `"Starting VxWorks in runmode(%s)"` (`0x392054`),
`"Starting VxWorks in bootmode"` (`0x391fa9`), `RUNMODE` /
`RUNMODE=%d` (`0x39e6f8` area) are present and clearly come from
this dispatcher, but their xrefs aren't direct 4-byte literals — the
compiler split them via `lis/addi`. None of them appear in the
captured PSC1 output (which is empty), so the dispatcher hasn't even
emitted its banner.

Hypothesis worth testing next session:

1. **Boot-mode is read from EEPROM/NVRAM and defaults to "bootmode"
   on a blank boot.** The X1226 EEPROM model returns 0xFF for
   uninitialised bytes — if the runmode byte lives there and the
   BSP sees 0xFF, it stays in "bootmode = idle daemons only".
   Concrete test: trace the few seconds around when the boot
   stations stop firing to find the EEPROM read that gates runmode
   selection. The I2C2 / X1226 model already logs every read; check
   whether any high-EEPROM-offset reads happen in the 30..58 s window.
2. **Runmode is set via boot-line / VxWorks bootargs.** VxWorks
   normally reads a bootline from NVRAM giving `"<dev>(0,0)host:file
   e=ip h=server u=user pw=pwd o=other"`. The BSP may parse `o=` for
   "runmode=2" or similar. We pass nothing, so the BSP falls through.
3. **Runmode is set via an environment variable injected by an
   external init step we're skipping.** `usrAppInit` may be reading
   a variable left by an earlier (currently-unmodelled) hardware
   path: ARCnet probe, CAN bus query, or a serial console command.

## What gate-7 actually depends on (revised)

From this session's evidence, gate 7 isn't FS-availability — it's
**"BSP enters runmode init"**. The Phase-4b FS hook is downstream
of that. Order of work to actually flip gate 7:

1. Find the runmode selector / storage. Likely candidates: bootline
   parser, an EEPROM byte we're returning wrong, or a CAN/ARCnet
   probe that's returning failure.
2. Make it return "runmode 2" (or whatever the on-disk
   `startup.app` script implies — node 10 = Ground, so probably
   one of the GND-default modes).
3. *Then* `ctvxInit` runs, calls `ctvxInitTurbine`, hits 0x13ffe4
   (our trace fires → first `FS_HOOK: TRACE` line in the log), then
   0x1061c0, then our open/read/close hooks.

The Phase-4b code doesn't need to change for that — it's already
set up. We just need to trip the upstream gate.

## Status on plan gates

- **Gate 6 (banner):** ❌ unchanged. PSC1 captured zero printable
  bytes for the runmode/bootmode banner.
- **Gate 7 (FS available):** 🟡 **scaffolding complete; awaiting
  upstream gate.** Phase 4b hooks installed, all three (open / read
  / close) are 9-instruction hypercall stubs landing in our QEMU-side
  dispatcher. The host `/fs/` tree is real and reachable. What's
  missing is the upstream BSP code that calls `open(/fs/...)` — see
  above.
- **Gate 8 (Vestas app boots):** ❌ unchanged. Vestas-mode flag
  `*(0x95a5a0)` stays 0; `tApMain`/`tFirecrest`/`tFiredrake` not
  spawned.

## Deferred / out-of-scope for next session

- `/zlfs/` host-side tree wiring. Path-translation accepts only
  `/fs/...` right now. Once the BSP can read `/fs/etc/startup.app`,
  the script's `ld 0,0,"/zlfs/bin/release_diab_ppc/halo.out.zlfs"`
  lines will need either a `/zlfs/` mount or an additional
  translation rule. Trivial extension to `fshook_translate`.
- `loadModule` / zlfs decompression. The script's `ld` calls invoke
  `loadModule` which expects ELF blobs; `.out.zlfs` is a
  zlib-compressed wrapper. Deferred per plan.
- VxWorks `struct stat` exact layout. `FS_FSTAT` writes only
  `st_size` at offset 0x20 of the guest stat buf — fine for the
  first BSP that asks, but we haven't seen the BSP exercise this
  yet.

## Verification

Build, run, look for `FS_HOOK:` and station hits:

```bash
cd /home/kasper/qemu
LOG=/tmp/qemu_4b.log TIMEOUT=75 ./run.sh
grep -E "FS_HOOK"        /tmp/qemu_4b.log   # zero lines (this session)
grep -E "STATION HIT"    /tmp/qemu_4b.log   # 12 hits, all FEC/SDMA
grep -E "UNREACHED \[6[3-9]" /tmp/qemu_4b.log
```

The zero FS_HOOK lines + UNREACHED on stations 63–69 is the proof
that the missing gate is *upstream* of the FS layer.

## Files touched

- `hw/ppc/mac_newworld.c` — added FS-hook hypercall infrastructure
  + extended fs_exists stub with diagnostic trace.

No changes to `run.sh`, no changes to `PLAN.md` (will be updated in
the docs commit).
