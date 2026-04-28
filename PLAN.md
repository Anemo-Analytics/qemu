# Plan: Boot VxWorks MPC5200B in QEMU

**End goal:** VOT (Vestas Online Toolkit) connects to QEMU and treats it as
a live CT6003 turbine controller. Single-node Ground (Node 10) PoC first;
multi-node ARCnet later.

---

## Where we are (2026-04-28)

Kernel loads, executes, scheduler runs, all peripheral stubs init clean.
EXT/DECR exception flow stable — only `EXTERNAL` and `DECR` in the
`-d int` log, no `HV_EMU` or `PROGRAM`.

**Parked at NIP `0x207fe8`** waiting for an event we don't deliver.
Hypothesis: `vxworks.out` is in **boot mode at link-local
169.254.254.254** waiting for a technician laptop at 169.254.254.253 to
FTP application binaries onto flash. Strong but unverified.

Working stubs in `hw/ppc/mac_newworld.c`:
- IC: SLT1 active-source + PerEnc read-to-clear at 0x524
- I2C2 X1226 + AT24Cxx state machine, slaves 0x50/0x57/0x6f
- BestComm/SDMA register file + 32 KiB internal SRAM
- PSC TX-ready stub (no real chars yet)
- EXT delivery gated on populated handler at 0x500

What's missing to unblock the park: depends on the verdict from
Daniele's diagnosis (see `PLAN_Daniele.md`).

---

## Two parallel tracks

Work is split so Kasper + Daniele can advance in parallel without
stepping on each other. Same repo, same Claude Code setup, different
files.

### Daniele — diagnosis

**Goal:** falsify or confirm the FTP-wait hypothesis. Answer "what is
the BSP actually waiting for at `0x207fe8`?" with evidence.

**Spec:** [`PLAN_Daniele.md`](PLAN_Daniele.md)

**Deliverable:** one markdown report (`BSP_park_findings.md`) with a
verdict at the top (CONFIRMED / REFUTED + the real wait condition).

**Touches:** docs only, no code. Branch: `mpc5200-diagnosis` off
current `mpc5200-stub`.

### Kasper — FEC implementation

**Goal:** working `mpc5200-fec` QEMU device + host-side FTP plumbing.
First serial banner from the kernel = pass.

**Spec:** [`PLAN_Kasper.md`](PLAN_Kasper.md)

**Deliverable:** new `hw/net/mpc5200_fec.c`, integrated, observable
packet flow. Pass = kernel emits PSC TX bytes; fail = still parked,
pivot using Daniele's findings.

**Touches:** `hw/net/*`, `hw/ppc/mac_newworld.c`. Branch: continue on
`mpc5200-stub`.

### How they integrate

| Daniele's verdict | Effect on Kasper's track |
|---|---|
| FTP wait confirmed | Charge ahead, already on the right path |
| BestComm DMA wait | Pivot to BestComm task executor before FEC |
| RTC time-of-day wait | Fix X1226 RTC counter readback first |
| GPT timer wait | Model GPT block, return to FEC after |
| Proprietary boot protocol | Keep FEC device, replace vsftpd with custom shim |
| Filesystem mount failure  | Pivot to disk-image + ATA model (Phase 2.5) instead of FTP |

If Daniele finishes first and contradicts the hypothesis, Kasper saves
days of wasted FEC work. If confirmed, Kasper's track was correct
anyway.

---

## Open questions / unknowns

Live questions we don't have answers to yet. Folded into existing
tracks where they fit; promoted to standalone investigations only when
necessary.

### How is `/ata0a/` mounted on the real controller?

Node 10 files were dumped from a working turbine on Røye2 via the
Vestas **Firedrake** protocol, but we don't know how VxWorks mounts
that filesystem on the real hardware: which device (ATA controller?
TrueFFS? CompactFlash via the MPC5200 ATA block?), which FS type
(`dosFs`, `tffsDrv`, raw `iosDevAdd`?), what partition layout, what
the BSP's mount call site looks like.

**Folded into Daniele's track** as a secondary task: same toolchain
(symbol search + disassembly of `vxworks.out`), and the answer may
overlap with the park diagnosis — if VxWorks parks on a failed mount
rather than an FTP wait, the verdict on `0x207fe8` flips entirely.

**Phase 2.5 — actual mount in QEMU:** see
[`PLAN_Phase2.5_filesystem.md`](PLAN_Phase2.5_filesystem.md).
Gated on Daniele's findings — shape of the work depends on whether
the kernel boots from FS or from FTP-pushed binaries.

### How does the BSP decide boot-mode vs runmode?

Boot mode = wait for FTP push at 169.254.254.254. Runmode = boot from
`/ata0a/` and run `etc/startup.app`. What flips the switch on real
hardware? Possibilities:
- Hardware jumper / GPIO pin
- Filesystem state (e.g. presence of a marker file)
- Bootline parameter in the boot ROM
- Some I/O register we're stubbing as zero

If we can force runmode by populating `/ata0a/` correctly, we may not
need to model the FTP push at all. Folded into Daniele's track.

### BestComm DMA dependency for FEC packet flow

If the FEC uses BestComm DMA tasks for TX/RX rather than direct
register MMIO (the same pattern that blocks PSC), then Kasper's FEC
device won't actually move packets without a BestComm task executor.
Already noted in `PLAN_Kasper.md` risks. If observed during
integration, escalate to Phase 2b (BestComm task model).

### RTC time-of-day plausibility check

VxWorks BSPs often refuse to boot if the RTC reads a time before some
sanity threshold (e.g. before 2000-01-01). Our X1226 stub returns
zeroed RTC bytes. If Daniele's diagnosis points at an RTC read loop,
this is the fix.

---

## Reference docs (in repo)

- `docs/MPC5200_Users_Guide.pdf` — full 732-page MPC5200UG Rev 3.1
- `docs/MPC5200_FEC_Chapter14.pdf` — Fast Ethernet Controller (Ch 14)
- `docs/MPC5200_BestComm_Chapter13.pdf` — SDMA / BestComm (Ch 13)

Key finding from page 14-1: **the FEC depends on BestComm DMA for all
data transfer** ("Interrupt driven data movement from the processor is
not supported"). Promotes BestComm task execution from "risk" to
"required architecture" in `PLAN_Kasper.md`.

---

## Build & run (canonical)

```bash
cd /home/kasper/qemu
ninja -C build qemu-system-ppc

timeout 5 ./build/qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null -d int 2>/tmp/qemu_int.txt
grep -oE "=> [A-Z_]+" /tmp/qemu_int.txt | sort | uniq -c
# expect only DECR and EXTERNAL — current clean baseline
```

`vxworks.out` lives at `/tmp/vxworks_romfs/vxworks.out` (same path on
both machines per shared dump layout).

---

## Phase 3+ (out of scope, for context)

Once boot completes:
- App-layer protocol stubs: Firecrest, AP, Firedrake, NEON
- VOT connects to single-node Ground (Node 10)
- Multi-node ARCnet for full controller stack
