# Bootrom scheduler wedge — pinpointed (2026-04-28 session)

**TL;DR:** The bootrom reaches `kernelInit` but **never transitions from
`windExit` to `windLoadContext`** — meaning the kernel idles forever
without dispatching `usrRoot`. CPU spends **98% of samples at
`0x0100107c`** (intUnlock body) — the kernel idle loop.

**Recommended action:** Stay with path A1 (vxworks.out + FTP boot).
Bootrom direct-load is a dead end without significant kernel-scheduler
investigation that exceeds the cost/benefit of gate 3.

---

## Method

Added a **fast diagnostic sampler** (100 µs virtual time, 30 000
samples = 3 s of virtual time) to `hw/ppc/mac_newworld.c`:
- NIP/MSR/DEC SPR snapshot per sample
- Boot-station first-hit detection over ~42 named stations
- Top-30 NIP histogram across full bootrom .text (`0x01000000..0x011f4000`)

This is a sibling timer to `mpc5200_tick`, fires from boot (no startup
delay), and disables itself after 30 000 samples to keep logs bounded.

## Key data

### Stations reached (bootrom)

| Station | Status |
|---|---|
| `excVecInit` (`0x010f2154`) | ✅ |
| `sysHwInit` (`0x010e0b60`) | ✅ |
| `m5200IntrInit` (`0x010dc1a0`) | ✅ |
| `sysGpioHwInit` (`0x010dc7e8`) | ✅ |
| `sysSerialHwInit` (`0x010ddfec`) | ✅ |
| `sysSdmaInit` (`0x010e9e5c`) | ✅ |
| `usrKernelInit` (`0x010d4e54`) | ✅ |
| `taskLibInit` (`0x010c8a18`) | ✅ |
| `taskHookInit` (`0x010adacc`) | ✅ |
| `semCLibInit` (`0x010c5ce4`) | ✅ |
| `semMLibInit` (`0x010c71a4`) | ✅ |
| `wdLibInit` (`0x010cbcc0`) | ✅ |
| `msgQLibInit` (`0x010c358c`) | ✅ |
| `qInit` (`0x010b4d68`) | ✅ |
| `workQInit` (`0x010ce1f8`) | ✅ |
| `memPartLibInit` (`0x010a896c`) | ✅ |
| `cacheEnable` (`0x010a1bcc`) | ✅ |
| `kernelInit` (`0x010c33c8`) | ✅ |
| `windExit` (`0x010d3640`) | ✅ (1 sample) |

### Stations NOT reached (bootrom)

| Station | Status |
|---|---|
| `windLoadContext` (`0x010d3860`) | ❌ |
| `taskCode` (kernel idle / first task body) | ❌ |
| `usrRoot` (`0x010d60f8`) | ❌ |
| `usrMmuInit` body | ❌ |
| `sysClkConnect` body | ❌ |
| `sysHwInit2` body | ❌ |
| `sysClkRateSet` body | ❌ |
| `sysClkEnable` body | ❌ |
| **`vxDecSet`** (DEC arm) | ❌ |
| `vxDecReload`, `sysClkInt` | ❌ |

### Top-30 NIP histogram (bootrom, 30 000 samples)

| NIP | Samples | Symbol |
|---|---|---|
| `0x0100107c` | **29 496 (98%)** | **intUnlock body** |
| `0x010c5240` | 26 | semCLibInit tail |
| `0x010f3024` | 23 | excVecInit/excConnect area |
| `0x010f2fa0` | 21 | excVecInit |
| `0x01037570` | 19 | vxMsr family |
| `0x010f2f34` | 19 | excVecInit |
| `0x010ce430` | 17 | workQInit area |
| ...followed by sub-20 counts | | |

### Comparison: vxworks.out (runtime, working)

`vxworks.out` is loaded at `0x00100000` so the bootrom-targeted
histogram doesn't cover its addresses. But station 41
(`VX: sysClkInt @ 0x00117fd0..0x001180ff`) **WAS HIT** with
MSR=`0x00001030` (MSR_ME | MSR_IR | MSR_DR) — the runtime fully boots,
arms DEC, and runs the DEC ISR.

## Diagnosis

The bootrom kernel reaches `kernelInit`. `kernelInit` calls
`taskInit(usrRoot)` and `taskActivate(usrRoot)`, which **should** mark
usrRoot ready and trigger a context switch via `windExit`.
We see `windExit` hit ONCE — kernel does enter the dispatch path —
but `windLoadContext` is **never** reached. So `windExit` returns
without dispatching, and the kernel falls into its busy-idle loop:

```
while (1) {
  intUnlock();   // 0x0100106c — sets MSR.EE=1; allows EXT to fire
  intLock();     // 0x01001058 — clears MSR.EE
}
```

The CPU sits at `intUnlock+0x10` (`0x0100107c`) 98% of the time.
External interrupts (SLT timer, ~45 EXT events / 4 s) fire and are
processed, but the EXT handler does not unblock `usrRoot` (because
`usrRoot` was never made ready in the first place — or was made
ready but `windExit` cannot find it).

## Why DEC never fires

DEC is armed exactly once, by `sysClkEnable → vxDecSet (mtspr 22)`,
called from `usrRoot+0x44`. `usrRoot` is never dispatched. Therefore
DEC is never written. `cpu->env.spr[SPR_DECR]` remains `0x00000000`
(reset value) — confirmed at every NIP sample.

## Why vxworks.out works

The runtime image has structurally identical scheduler code (per
static analysis A1: identical `mtmsr` count, identical `rfi` count,
identical `mtspr 22` count). But its `usrInit` is much shorter — it
**skips the bootrom's sync-handshake spinlock** and a different early
init dispatcher. Critically, the runtime's kernel scheduler is built
differently (`tickAnnounce`/`windTickAnnounce` did not signature-match
between the two binaries), so the runtime *does* dispatch its root
task and arm DEC.

## Hypotheses for the bootrom scheduler failure (RANKED)

### Rank 1: `taskActivate` doesn't actually mark usrRoot ready
`kernelInit` calls `taskActivate(usrRoot)` which adds usrRoot to the
ready queue. If the ready-queue data structure is malformed (e.g. a
priority-bitmap or readyQHead pointing at uninitialised BSS), the
add silently no-ops. Subsequent `windExit` finds no ready task and
idles.

**Test:** stations on `taskActivate` (`0x010c8a18` lib area), look
for the writes to `tickQHead@0x01204780` and `readyQ` heads.

### Rank 2: `windExit` finds usrRoot ready but the priority compare fails
The kernel idle is itself a "task" (`taskCode` at `0x010d3690`,
also UNREACHED). If the bootrom never installs the idle task and
`windExit` searches for the highest-priority READY task expecting
the idle task to exist as a fallback, it might enter a degenerate
loop.

### Rank 3: Bootrom-handshake side-effect we missed
The bootrom's `usrInit` has a sync-handshake spinlock checking magic
values (`0x12348765` at `[r7+0x1c30]`, `0x5a5ac3c3` at `[r8+0x1c34]`).
Phase A2 said these are pre-initialised in `.data`. But
`usrInit_entry` station (`0x010d8718..0x010d8807`) was UNREACHED in
our sampler — meaning it ran fast (single-instruction blip beats our
100 µs sampler). If something further in the path got skipped because
of an early return, the kernel state could be incomplete.

## What this changes about the strategy

The comparison study (`BSP_comparison_study.md`) scored three
outcomes. With this finding:

- **Outcome 1 (single missing register/SPR):** REFUTED. Many stations
  reached → CPU executing fine. Wedge is in scheduler logic, not a
  hardware register.
- **Outcome 2 (architectural difference):** **CONFIRMED.** The bootrom's
  kernel scheduler relies on something our QEMU doesn't provide.
  Either (a) the bootrom expects a chain-load mechanism we haven't
  done, or (b) the bootrom expects a hardware response (e.g. a
  specific peripheral IRQ) to wake `usrRoot`.
- **Outcome 3 (TFFS cold-start):** still on the table as fallback if
  vxworks.out + FTP boot proves harder than expected.

## Next-action recommendation

**Do not pursue bootrom direct-boot further.** Stay with path A1
(vxworks.out + FTP boot). Reasons:

1. vxworks.out **demonstrably boots** — DEC fires (291 events / 4 s),
   FEC inits, scheduler dispatches its root task, sysClkInt runs.
2. The bootrom scheduler wedge would require deep investigation of
   VxWorks kernel internals (taskActivate/windExit semantics) — likely
   3–5 days minimum.
3. Path A1 unblocks gate 3 (BestComm executor) which is well-scoped
   and yields toolkit-relevant progress.

## Diagnostic infrastructure preserved

The fast NIP sampler (`mpc5200_diag_sample`) and station/histogram
infrastructure remain in `hw/ppc/mac_newworld.c`. They are valuable
for any future PPC binary investigation under this QEMU. Stations
include both bootrom and vxworks.out boot-relevant addresses.

## Files

- This finding: `BSP_bootrom_scheduler_findings.md`
- Code: `hw/ppc/mac_newworld.c` (fast sampler + 42 stations + histogram)
- Logs:
  - `/tmp/qemu_bootrom_diag5.txt` — bootrom run with full diagnostics
  - `/tmp/qemu_vxworks_diag.txt` — vxworks.out comparison run
