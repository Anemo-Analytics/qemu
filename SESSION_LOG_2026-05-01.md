# Session 2026-05-01 — qemu-side `/fs/` hook scaffolding (Phase 4a)

Plan: see `PLAN.md` and the in-message plan for this session
(qemu-side function-hook to fake `/fs/` instead of emulating MPC5200
ATA).

## TL;DR (session-end)

- **Phase 0 (era pin-down):** ✅. Our `vxworks.out` is **2022 era**
  (VxWorks 5.5.1 + 2018 toolchain string + `_ZN4halo` and `Firedrake`
  C++ symbols). Source `/fs/` tree:
  `/home/kasper/Vestas/bin/data_dump/firedrake/turbine_dump_node10_roye2/fs/`.
- **Phase 1 (script survey):** ✅. 2022 `startup.app` is 4509 bytes of
  ASCII (CRLF). ~46 `ld 0,0,"/zlfs/bin/release_diab_ppc/<name>.out.zlfs"`
  calls. Path mapping: `/zlfs/...` is a separate virtual mount; the
  on-disk modules live under `/fs/bin/release_diab_ppc/` in the dump.
- **Phase 3 (file-IO NIPs):** ✅. Found via string-xref and disasm
  inspection — the right pieces, not the agent's guesses.
- **Phase 4a (existence-check bypass):** ✅ **patch installed** at
  `0x13ffe4`. Built and ran. **Verification inconclusive** — see
  caveats below.
- **Phase 4b (full open/read/close proxy):** ❌ deferred — requires
  either an MMIO-hypercall region or a PPC-translator extension.
  Multi-day work, not feasible this session.
- Gate 7: still 🟡 (mechanism in place, not verified).
- Gate 8: still ❌ (Vestas-mode flag `*(0x95a5a0)` never flipped).

## Key findings

### Two firmware eras locally — ours is 2022

`/tmp/vxworks_romfs/vxworks.out`:
- `VxWorks5.5.1` + `Copyright 1984-2003 Wind River Systems`
- toolchain path string `/home/vagrant/vxworks-toolchain/work.20181221.122002/...`
- C++ mangled symbols: `_ZN4halo11system_heap8allocateEj`, `Firedrake`
  class references → 2022-era (post-2018). Decompiled directory
  `firmware_v12.04.55/` is misleadingly named — its toolchain
  artefacts and module-name-fragment evidence are 2022, not the
  literal 2012 release version `12.04.55`.

Source-tree decision: `node10_roye2/fs/` (Ground/node10, complete
real-turbine snapshot, no decompression needed).

### The 2022 startup.app

ASCII text, 4509 bytes, CRLF terminators. Sets ~14 `putenv` lines for
node identity (`V_LOCAL_NODE=10`, `V_LOCAL_NODE_NAME=GND`,
`NEON_LIVE_DATA_SERVER_ADDRESS=172.30.0.10`, etc.), then 46 `ld 0,0,...`
calls for modules in `/zlfs/bin/release_diab_ppc/`. Critical chain:
`halo` → `vx_stdout_pipe` → `pantheon` → `neon` → `falcon` → ... →
`firecrest_server` → `iocore` → `CT296` → `BAM` → `bui` → ... → `dao_start`.

### File-IO entry points in `vxworks.out`

ELF: `.text` at vaddr 0x100000 / file offset 0x80; `vaddr = file_off + 0xfff80`.

| Function | NIP | How identified |
|---|---|---|
| File-existence helper (fopen+fclose check) | **0x0013ffe4** | Loads `r4 = 0x3DBAC0` = `"r"` mode string; calls 0x2a5508 + 0x2a4cac. Many call-sites including 0x14c7d4 (with `/fs/etc/startup.app` arg). |
| `fopen(path, mode)` | **0x002a5508** | Called by 0x13ffe4 with mode `"r"` (data at 0x3DBAC0 = `0x72 0x00 0x00 0x00` = `"r\0"`). |
| `fclose(FILE*)` | **0x002a4cac** | Called immediately after 0x2a5508 success — closes the just-opened FILE*. |
| `open(path, flags, mode)` (3-arg syscall) | **0x002b13c8** | Called from 0x1061c0 with `(path, 0, 0)` and `cmpwi cr7, r3, -1` validates fd. |
| Probable `close(fd)` | **0x002b17d4** | Called from 0x1061c0 success path with `r3 = fd`. |
| `execStartupScript` / script-runner | **0x001061c0** | Reaches the "Executing startup script %s" `printf`. The 2026-04-29 hint about `FUN_001471c4` was wrong — that NIP is mid-function, not an entry. |

Strings:
- vaddr 0x392078: `"Executing %s\0..."` (gate-7 banner format)
- vaddr 0x392088: `"/fs/etc/startup.app"`
- vaddr 0x39209D: `"Cannot find startup.app in runmode !"`

Path through the boot loop (function containing 0x14c4ec / 0x14c6e8):
1. Loop-top poll at 0x14c6e8 calls `bl 0x1108e8` (mode/runmode probe).
2. When the probe returns 2 (RUNMODE), code at 0x14c7d4 loads
   `/fs/etc/startup.app` into r3 and calls `bl 0x13ffe4` (existence
   check) at 0x14c7e0.
3. Branch at 0x14c7e8 — if r3==0, jump to 0x14c828 (failure print
   "Cannot find..."). Else, fall through to success path 0x14c7ec.
4. Success path prints `"Executing %s ..."` (format @ 0x392078)
   then calls `bl 0x1061c0` at 0x14c820 — which actually runs the
   script (open, read, vshell-parse, exec).

The retry-counter referred to in yesterday's analysis is somewhere in
the path 0x14c4ec → 0x14c6e8 — beyond scope this turn.

### Patch installed (Phase 4a)

In `mpc5200_apply_keyswitch_patches()`:

```
0x0013ffe4: 38 60 00 01    li   r3, 1
0x0013ffe8: 4e 80 00 20    blr
```

Stubs the existence-check helper to always return "exists=1". The
function had not yet set up a stack frame, so a bare `blr` is safe;
caller's saved LR untouched. Patch applied at first SLT timer tick,
same as the existing keyswitch and spawn-gate patches.

Boot stations also added for: 0x13ffe4, 0x14c7d4 (call site),
0x14c7ec (success branch), 0x14c828 (failure branch), 0x1061c0
(script-runner), 0x2b13c8 (open), 0x2a5508 (fopen).

Diag-sampler window extended from 3 s → 60 s of virtual time.

### Verification — inconclusive

Two independent verification gaps prevented a clean gate-7 ✅/❌:

1. **No BSP serial-output capture.** `run.sh` uses
   `-display none -serial null`. Real `printf`s never reach stderr.
   PSC1 emulation in `mac_newworld.c` is byte-counter only —
   captured 6 PSC*+0x00 byte writes total, all 0x13/0x07 control
   bytes during PSC init. The BSP routes runtime printfs somewhere
   else (probable: `tWdbTask` console via WDB on port 17185, or to
   a `logFd` ring buffer in BSS). Not a serial path we currently
   sniff.
2. **Diag sampler is too coarse.** It fires every 100 µs of virtual
   time. A 2-cycle stub function (the patched 0x13ffe4) almost never
   coincides with a sample. All 7 of the new FS-related stations
   came back UNREACHED after 60 s of sampling. This is a
   limitation of the sampler, not evidence that the code path
   isn't taken.

Indirect evidence we *do* have:
- Vestas-mode flag `*(0x0095a5a0)` stays at `0x00000000` for the full
  90 s run. No transition. So whatever the patch did, it didn't
  cascade through to spawn the Vestas tasks. Consistent with: the
  banner may print, but the actual `bl 0x1061c0` call still fails
  (no real `/fs/`, no modules to load).
- Task list at t=58 s: same 21 generic VxWorks tasks as before. No
  `tApMain`, `tFirecrest`, `tFiredrake`, `tNeon`. Gate 8 still ❌.

**Honest assessment:** the patch installation is correct, and the
disasm analysis pinning the existence-check at 0x13ffe4 is solid.
Whether the patch caused a behavioural delta is unknown without a
serial path. To make further progress, the next session must either:
(a) hook PSC1 emulation to capture char-level TX writes, or
(b) hook the WDB console output, or
(c) implement Phase 4b (real `open`/`read`/`close` proxy) so modules
actually load, providing observable downstream effects.

## Decisions for next session

- **Don't spend more time on indirect verification of 4a.** The
  scaffolding is in place. Focus next on (a) serial capture or
  (c) Phase 4b directly. (c) is the larger lift but actually moves
  gate 7 → ✅ and unblocks gate 8.
- **For Phase 4b, prefer the MMIO-hypercall approach** over PPC
  translator changes: define an unused MBAR sub-region (e.g.
  `0xF0004xxx`), patch the entry of `open`/`read`/`close`/`lseek`
  with a small PPC stub that stores arg regs to MMIO offsets and
  reads result back. The MMIO write callback in C does the host
  syscall against `node10_roye2/fs/<tail>`. This reuses the same
  dispatch path already used for I2C2/EEPROM and BestComm registers.
- **Stay 2022-era.** Don't pull from `Phoenix_v12.04.55` zlfs
  archives — risks toolchain/symbol mismatch with the loaded
  `vxworks.out`.

## Files touched this session

| File | Change |
|---|---|
| `hw/ppc/mac_newworld.c` | (1) `mpc5200_apply_keyswitch_patches`: added 8-byte patch at 0x13ffe4 (`li r3,1; blr`). (2) `g_boot_stations[]`: 7 new stations for FS-gate observability. (3) `mpc5200_diag_sample`: extended sampler from 3 s → 60 s. |
| `SESSION_LOG_2026-05-01.md` | this file. |
| `PLAN.md` | gate 7 status comment refreshed. |

No changes to `run.sh` this session.
