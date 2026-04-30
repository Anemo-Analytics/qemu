# Plan: get one Vestas service module loaded under QEMU

Updated 2026-04-30 after Phase-3 FSHOOK trace + three-angle subagent
investigation (script-runner / loader / .zlfs format).

## What we now know vs what we still don't

### Known (high confidence)

- **The BSP's actual file-access sequence on boot** (Phase-3 evidence,
  /tmp/qemu_p3.log):
  1. `open("/fs")`, then close
  2. `open("/fs/manifest.csv")`, 9 reads (~8.7 KB), close
  3. `open("/fs/etc/fdconfig.def")`, 1 read (734 B), close
  4. `open("/fs/etc/startup.fail")`, 1 read (4 B = `"0 0\n"`)
  5. `exists("/fs/etc/force_safe_mode")` → MISS
  6. **silence for 115 s.** No `startup.app`, no `.zlfs`, no
     `loadMemTextAlloc`-shaped activity.

- **Script-runner location** is already known and tracked:
  `g_boot_stations[]` has `0x001061c0` labeled "VX: script-runner entry
  (post-banner)" (mac_newworld.c:1071). It **does not fire** in our
  current boot — confirming the wedge is upstream of the script-runner
  call site, not inside it.

- **`startup.fail` content is `"0 0\n"`** — fail counter zero, so the
  BSP should NOT be in recovery mode. The decision tree to take the
  startup.app branch should succeed.

- **The .zlfs format is just zlib** with a 16 KiB block index:
  `7a 6c 66 73 <ver> <uncompressed_size> <block_offsets[]> <zlib_blocks>`.
  Magic = ASCII "zlfs". Driver in bootrom.elf as `zlibFs*` family
  (zlibFsInit @ 0x010f7000, zlibFsOpen @ 0x010f6f10, zlibFsRead @
  0x010f6888). Standard zlib payload (`78 da` deflate header).

- **Pre-decompressed `.out` files exist in the dump** at
  `/home/kasper/Vestas/bin/data_dump/firedrake/turbine_dump_node10_roye2/elf_decompressed/`
  — 82 ET_REL PowerPC relocatables, ready for `loadModule()`. We can
  bypass .zlfs entirely for first proof-of-load.

- **`atspMaster(0)` is non-blocking** SNTP-master spawn (per protocol
  expert). It does not wait on a network peer; it just installs an SNTP
  responder and returns. So even with our network wedged, the script
  would not hang at line 1 of startup.app — `atspMaster` itself is fine.

- **`vx_stdout_pipe.out` is the cleanest first-load target**:
  zero Vestas dependencies (only kernel + halo if loaded first), opens
  TCP port 2323, telnet-able for proof-of-life. NOT
  `central_log_service` — that one needs `neon` C++ symbols and would
  fail at link-time without the chain `halo → neon → central_log`.

### Unknown (must investigate before committing to Stage 1 implementation)

- **Runtime VA of `loadModule` / `loadMemTextAlloc` in vxworks.out.**
  vxworks.out is stripped (1 symbol). bootrom.elf has full symbols
  but is a DIFFERENT binary at a DIFFERENT load address (0x01000000 vs
  0x00100000). The two binaries are not rebases of each other —
  byte-content at the same logical offsets is completely different.
  We have to **sig-match** the loader prologue from bootrom.elf into
  vxworks.out manually (same workflow that established
  netJobAdd @ 0x0022c288 and qPriBMapPut @ 0x002cd8e0 — neither was
  found by a runtime loop, both are static analysis).

- **Whether bootrom code is still callable at runtime.** bootrom.elf
  is loaded at 0x01000000 and would normally be discarded once
  vxworks.out takes over. But if 0x010xxxxx is still mapped after
  boot, we could call `bootLoadModule @ 0x010b039c` directly without
  needing the vxworks.out sig-match. **Cheap to test:** read 4 bytes
  from 0x010b039c via cpu_physical_memory_read in mpc5200_tick at
  vt=15s and compare to bootrom.elf's content at file offset 0xb039c.
  Match → bootrom callable; mismatch → bootrom got overwritten.

- **Why the BSP doesn't reach the script-runner.** The wedge is
  upstream of `0x001061c0`. Likely candidates (need NIP sampling):
  - `tRootTask` parked in `taskDelay` (PC=0x001762b8 in current dumps)
    waiting on a condition that never arrives — possibly the network
    stack settling (which is wedged per Phase 1/2 outcome).
  - A `safe-vs-normal` decision dispatcher upstream of the script-runner
    that interprets `startup.fail = "0 0\n"` differently than expected.
    `"0 0\n"` is two numbers, not one — could be `<fail_count> <safe_count>`
    or `<consecutive_fails> <total_fails>`.

- **Whether vx_stdout_pipe needs `halo.out` resident.** It's listed as
  load #2 in startup.app (after halo). The protocol expert flagged this
  as "should link cleanly" — but didn't verify. Need to cross-check
  vx_stdout_pipe.out's relocation table (`readelf -r`) against halo's
  symbol exports.

- **Whether `loadModule()` itself works without the script-runner's
  preparation.** startup.app first calls `loadMemTextAlloc(0x400000)`
  + `memPartAddToPool(memSysPartId, ..., 0x400000)` + `symReserve(0x600000)`.
  These set up a memory partition for ELF text. If we call `loadModule`
  before any of that runs, it might fail with out-of-memory.

## Stage gates

Honest about each gate's prerequisites. Don't commit to a stage until
the prior stage is green.

### Stage 0 — current state ✓

- [x] BSP boots to bootstrap (manifest.csv, fdconfig.def, startup.fail)
- [x] FSHOOK proxy works for open/exists/read/close
- [x] Probe-stub LR fix landed (Phase 1)
- [x] FSHOOK observability tagged for grep (Phase 3)
- [ ] First Vestas service module loaded — **NOT YET**

### Stage 1 — load ONE module, see ONE new task ⏳

**Pass criterion:**
- Task-list dump at vt≥30s shows a new entry (TCB indexed > 20 in
  current baseline) with a non-VxWorks-canon name (anything not in
  the existing 21-task baseline).

**Stretch (if network unwedged): TCP listener on the loaded module's port.**

**Prereqs (in order):**

1. **Resolve `loadModule` runtime VA.** Two paths in priority order:
   - 1a. Test if bootrom code is still mapped at runtime (one-line
     check, no commit). If yes, use `bootLoadModule @ 0x010b039c`.
   - 1b. If bootrom is gone, sig-match `bootLoadModule`'s 16-byte
     prologue from bootrom.elf into vxworks.out file and compute the
     runtime VA from the file offset + 0x00100000 base.

2. **Confirm memory partition has room for one ET_REL ELF.** Check
   memSysPart free pool BEFORE script-runner has run. If insufficient,
   pre-call `loadMemTextAlloc` from the same hypercall. (The memory
   partition is the most likely "loadModule fails silently" trap.)

3. **Implement the load doorbell.** New register in fshook struct
   `loadmod_path` (host writes vx-style path, e.g. `"/elf/vx_stdout_pipe.out"`).
   Inserted-stub picks it up from netTask context (using the existing
   netjob doorbell mechanism — same pattern as the wake stub, but
   pointing at a new stub that calls `loadModule(path, LOAD_ALL_SYMBOLS)`).

4. **Stage the file.** Two options:
   - 4a. (preferred) Add a separate FSHOOK root for elf_decompressed/
     so the path `/elf/vx_stdout_pipe.out` resolves without
     polluting the firedrake fs root.
   - 4b. Hard-symlink `elf_decompressed/vx_stdout_pipe.out` into the
     firedrake fs root so `/fs/bin/release_diab_ppc/vx_stdout_pipe.out`
     resolves directly.

**Risks specific to Stage 1:**

- `loadModule` requires a registered sym table; if symReserve hasn't
  run, name-resolution fails and the load returns NULL. Mitigation:
  call `symReserve(0x600000)` from the same hypercall stub before
  `loadModule`.
- vx_stdout_pipe might depend on `halo` symbols. If load fails with
  "unresolved external", load `halo.out` first. (Halo is the Vestas
  base library — small, safe to load eagerly.)
- The new task's TCB might land at a high address that the existing
  task-list walker doesn't enumerate. Confirm the walker iterates the
  full activeQHead chain (mac_newworld.c:2113-2174 — already known to
  work for 21 tasks).

**Honest unknowns at Stage 1:**

- Even if loadModule succeeds, the module's init function might not
  taskSpawn (some modules just register hooks). Watch for both: a new
  task AND any new file-create / TCP-bind activity.

### Stage 2 — verify the loaded module is functionally alive ⏳

**Pass criterion:** distinct evidence beyond "task exists" — the
loaded module DOES something measurable.

For `vx_stdout_pipe`: TCP SYN-ACK from guest on port 2323 (probed via
`nc -w 5 127.0.0.1 23232` after hostfwd is added).

**Blocker:** Stage 2 needs the network downstream wedge fixed
(Phase-1/2 outcome — `netjob_func` stays sticky after first dispatch,
SYN-ACK never leaves the guest). So Stage 2 is **gated on resolving
the netjob_func clear-after-dispatch issue**. See "Parallel network
fix" below.

If we can't unwedge the network in time, Stage 2's pass criterion
becomes file-system-only: does the loaded module's init function
write anything via FSHOOK? (Some modules write log files. Some don't.)

### Stage 3 — first Vestas service that does protocol work ⏳

Pass criterion: AP service on port 8008 responds to a SOAP
`LogonRequest` with a `LogonReply` containing a `ClientRequestHandle`
GUID (per protocol expert). This is the canonical "we have a real
Vestas controller responding".

Prereqs: Stage 2 green; halo + ap_service modules loaded; ap_service
init successfully binds(8008).

### Stage 4 — full dependency chain ⏳

Halo → neon → central_log_service. Each load chained off the previous
load's symbol-export availability. Pass criterion: 5+ Vestas tasks in
the task-list dump, neon's IPC bus shows traffic.

### Stage 5 — startup.app runs end-to-end ⏳

Either: (a) the BSP's own script-runner finally fires (network unwedged
+ tRootTask escapes its delay), or (b) we side-step it permanently and
load all 82 modules via repeated hypercalls in script-derived order.

## Parallel work needed: network downstream wedge

Stage 2+ needs the network to actually answer back. The Phase-1 LR
fix solved the probe-stub self-loop, but:

- `netjob_func` (FSHOOK reg 0x20) is set continuously to `0x002acf80`
  from vt=8s onward and the sysClkInt shim NEVER clears it. The shim
  does have a clear (`stw r0, 0x20(r10)` at +0x2C), but apparently the
  netjob branch in the shim is never entered.
- Hypothesis: `bl intUnlock` at shim+0x04 clobbers r12 (PPC ABI: r12
  is volatile across calls). Then `mtlr r12` at shim+0x30 stores
  garbage to LR before the tail-call. The shim's netjob path crashes
  on return from netJobAdd → never gets to clear netjob_func again.
  But this doesn't explain why the FIRST run-through doesn't clear it
  either. So either the `cmpwi r3, 0; beq +0x20` at shim+0x14/+0x18 is
  always taking the BEQ (sem-queue path) — meaning the shim's first
  read of `*(r10+0x20)` returns 0 even though we wrote a non-zero
  value via FSHOOK MMIO. Possible cause: cache coherency between FSHOOK
  MMIO writes (host side) and CPU reads (guest side) — the shim might
  see stale 0.

This is a separate investigation — not blocking for Stage 1 (which
needs no network), but blocking for Stage 2 stretch goal.

## Concrete first action (smallest possible step)

**Before any code:** validate whether bootrom code (0x010xxxxx) is
still mapped after vxworks.out boots. One-line addition to
mpc5200_tick at the existing TCB-WATCH cadence:

```c
if (tick_count == 60 * 30) {
    uint8_t br[16];
    cpu_physical_memory_read(0x010b039c, br, sizeof(br));
    fprintf(stderr,
        "BOOTROM-PROBE: bootLoadModule@0x010b039c first 16B = "
        "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x"
        " (expect first 4B from bootrom.elf file offset 0xb039c)\n",
        br[0],br[1],br[2],br[3], br[4],br[5],br[6],br[7],
        br[8],br[9],br[10],br[11], br[12],br[13],br[14],br[15]);
}
```

Compare to `xxd -s 0xb039c -l 16 /tmp/vxworks_romfs/bootrom.elf`. If
match → bootrom is callable, Stage 1 prereq 1a is satisfied for free.
If not → fall to 1b (sig-match in vxworks.out, more work).

**Time:** 5 min to add + 2 min boot run + 30 s comparison.

## Time budget (revised)

| Step | Est | Confidence |
|---|---|---|
| Bootrom-callable probe (one-line check) | 10 min | high |
| 1a. If bootrom callable: build the load hypercall stub | 60 min | medium |
| 1b. If not: sig-match `bootLoadModule` in vxworks.out | 90 min | medium |
| Stage 1 verification (task-list shows new entry) | 30 min | high |
| Stage 2 (network unwedge) | 4 hours | LOW — separate bug |
| Stage 3 (first AP service handshake) | 3 hours | medium IF 2 green |

Total to Stage 1 alone: ~2 hours. Stages 2-3 require the network fix.

## What we are explicitly NOT doing

- **Not implementing .zlfs decompression in QEMU.** Pre-decompressed
  `.out` files exist; use them. The native path (zlibFs driver in
  bootrom) is interesting but Stage 4+ work.
- **Not fixing the BSP's wedged script-runner** in this thread.
  Side-stepping with hypercalls is faster and gives us per-module
  control. The script-runner fix is a separate plan tied to whatever
  the upstream wedge actually is (most likely the network downstream
  wedge — startup.app's first useful work depends on the network
  being up).
- **Not loading central_log_service first.** Despite what the prior
  version of this plan said — it depends on `neon` C++ symbols and
  would fail link-time without halo+neon already resident.

## Critical files

| Path | Role |
|---|---|
| `hw/ppc/mac_newworld.c` | Add bootrom-probe diagnostic; later add `loadmod_path` doorbell + load-stub |
| `/tmp/vxworks_romfs/bootrom.elf` | Source of `bootLoadModule` prologue + zlibFs driver symbols |
| `/tmp/vxworks_romfs/vxworks.out` | Sig-match target if bootrom not callable |
| `/home/kasper/Vestas/bin/data_dump/firedrake/turbine_dump_node10_roye2/elf_decompressed/vx_stdout_pipe.out` | Stage-1 first-load target |
| `/home/kasper/Vestas/bin/data_dump/firedrake/turbine_dump_node10_roye2/elf_decompressed/halo.out` | Likely-needed dependency for any Vestas module |
| `/tmp/qemu_p3.log` | Phase-3 baseline FSHOOK trace — keep for diff against post-load runs |
