# MPC5200 Vestas BSP — live tracker

Updated 2026-04-30. Read top-down: stage gates first, then current
blockers, then next concrete actions.

For the deep version of the service-loadup plan (subagent synthesis,
risks, all the alternatives weighed), see
`.claude/plans/vestas-service-loadup.md`.

## Stage gates

| Stage | Status | Pass criterion |
|---|---|---|
| 0. BSP boots to bootstrap | ✓ done | manifest.csv, fdconfig.def, startup.fail all read |
| 1. ONE Vestas module loaded | ⏳ next | new entry in task-list dump (TCB index > 20) |
| 2. Loaded module is functionally alive | ⏳ blocked on net | TCP listener on its port answers SYN-ACK |
| 3. First Vestas service handshake | ⏳ | AP `LogonRequest` → `LogonReply` with GUID |
| 4. Dependency chain | ⏳ | halo + neon + central_log all loaded, IPC traffic |
| 5. Full startup.app runs | ⏳ | Either BSP's own script-runner fires OR all 82 modules pre-loaded |

## Current blockers (in priority order)

### Blocker A — BSP script-runner is wedged BEFORE reaching `0x001061c0`

**Evidence:** Phase-3 FSHOOK trace shows the BSP completes the
"is-this-recovery-boot?" decision (reads startup.fail → 0 fails;
checks force_safe_mode → missing) and then **never** opens
startup.app. Script-runner station at 0x001061c0 (already in
g_boot_stations[]) does not fire. tRootTask is parked in `taskDelay`
(saved PC = 0x001762b8) for the entire 120 s run.

**Workaround being pursued (Stage 1 plan):** side-step the
script-runner with a QEMU hypercall that calls `loadModule()` directly
in netTask context — same doorbell mechanism the FEC RX path uses.

### Blocker B — network downstream wedge (post-Phase-1)

**Evidence:** netjob_func sticks at 0x002acf80 from vt=8s onward;
sysClkInt shim never clears it; tFecEndRx body station hits exactly
ONCE in the whole 120 s run despite 7 RX events. Result: no SYN-ACK
ever leaves the guest, FTP banner test stays at Tier 3.

**Hypothesis:** shim's first read of FSHOOK MMIO 0xF0004020 sees
stale 0 (cache coherency between host MMIO writes and guest CPU
reads), so the netjob branch never executes the clear instruction at
shim+0x2C.

**Impact:** blocks Stage 2's network-listener verification. Does NOT
block Stage 1 (which proves load-success via task-list dump only,
no network needed).

### Blocker C — runtime VA of `loadModule` is unknown

**Evidence:** vxworks.out is stripped (1 symbol); bootrom.elf has the
symbol (`bootLoadModule @ 0x010b039c`) but is a different binary at a
different load address (0x01000000 vs 0x00100000). The two are not
rebases of each other (byte-content at the same logical offsets is
completely different).

**Cheap-test path (next concrete action):** check if bootrom code
remains mapped at runtime. If yes, `bootLoadModule @ 0x010b039c` is
callable as-is. If no, we sig-match the prologue from bootrom.elf into
vxworks.out file (~90 min static-analysis work).

## Next concrete action — Stage 1 prereq 1

**Add a one-time bootrom-mapping probe** at the existing TCB-WATCH
cadence in mpc5200_tick (around tick_count==60*30):

```c
uint8_t br[16];
cpu_physical_memory_read(0x010b039c, br, sizeof(br));
fprintf(stderr,
    "BOOTROM-PROBE: bootLoadModule@0x010b039c first 16B = %02x...%02x\n",
    br[0], ...);
```

Compare to `xxd -s 0xb039c -l 16 /tmp/vxworks_romfs/bootrom.elf`.

- **Match → bootrom callable**, Stage 1 prereq 1 satisfied for free,
  proceed to build the load-doorbell stub (~60 min).
- **Mismatch → bootrom got overwritten**, fall to sig-match path
  (~90 min) — extract bootLoadModule prologue from bootrom.elf and
  scan vxworks.out file for it.

Time estimate: 10 min including build + boot.

## Recently completed

- 2026-04-30 `f6b3dc3545` Phase 3 — FSHOOK observability (FS_OPEN /
  FS_EXISTS / FS_READ tags + per-fd read rate-limit)
- 2026-04-30 `a023913203` Phase 1 — probe-stub LR-loop fix
  (mflr/mtlr around bl semGive); tNetTask no longer self-loops
- 2026-04-30 `f78014673b` route hostfwd to guest's actual static IP
  169.254.254.254
- 2026-04-30 `4d48de5e14` walk tRootTask back-chain in DELAY too,
  not just PEND
- 2026-04-30 `c5db848bcf` shim per-sem dispatch wakes tFec or tRoot
  on bit 9 of sem ID

## What we are explicitly NOT doing

- Not fixing Blocker A (wedged script-runner) by rewriting the BSP's
  shell parser. Side-stepping is faster and reversible.
- Not implementing .zlfs decompression in QEMU. Pre-decompressed
  `.out` files exist in the dump (82 of them at
  `…/elf_decompressed/`); use those.
- Not chasing Blocker B's cache-coherency hypothesis until Stage 1
  is green. Confirming Stage 1 first tells us whether the hypercall
  doorbell path even works at all — same primitive both blockers
  depend on.
- Not loading `central_log_service.out.zlfs` first (despite a prior
  version of the plan suggesting it). It depends on neon C++ symbols
  and would fail link-time without halo+neon already resident.
  `vx_stdout_pipe.out` is the right Stage-1 target — minimal deps,
  TCP-port verifiable once network unblocks.
