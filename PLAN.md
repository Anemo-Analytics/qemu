# Plan: Boot VxWorks MPC5200B in QEMU & connect the VMP6000 Toolkit

## End goal

**The VMP6000 Toolkit (running on a Windows VM) connects to our QEMU
turbine, recognises it as a CT6003, and successfully performs a
software-load operation against it** — i.e. the toolkit can push
firmware/parameters/binaries to QEMU as if it were a real turbine.

That's the validation that QEMU is "real enough". Everything below
ladders up to that.

---

## Roadmap & verification gates

Each row below is a discrete, demoable milestone with a concrete
verification command. We don't move on until the previous gate
verifies.

| # | Milestone | What you'd see | Verification | Status |
|---|---|---|---|---|
| **0** | QEMU builds, kernel loads | `vxworks.out` loaded at `0x100000`, CPU executes | `ninja -C build qemu-system-ppc` succeeds; kernel runs | ✅ |
| **1** | Stable scheduler, no exception loops | `-d int` shows only `DECR`/`EXTERNAL`, never `HV_EMU` or `PROGRAM` | `grep -oE "=> [A-Z_]+" /tmp/qemu_int.txt \| sort \| uniq -c` | ✅ |
| **2** | BSP reaches FEC init | FEC register writes show up — ECR reset, MAC programmed, MII clock divider, MII frame issued | grep `^FEC W` in QEMU log | ✅ (this session) |
| **3** | BestComm executor — TX | When BSP enables TCR[2]=0xC2, our executor walks the BD ring and `qemu_send_packet`s the frame | Wireshark/tcpdump on host loopback shows guest-originated TCP SYN to 169.254.254.252:21 | 🟡 NEXT |
| **4** | BestComm executor — RX | FTP server's SYN-ACK reaches the kernel; BSP sees frame in RX BD ring | FTP server logs accept the connection from guest IP `.254` (not just QEMU's startup probe) | ⏳ |
| **5** | FTP boot completes | Anonymous login OK, `ct6003/vxworks` retrieved fully | FTP log shows `RETR ct6003/vxworks` + transfer size = full file | ⏳ |
| **6** | First serial banner | Kernel emits PSC TX after image is loaded into RAM and runmode entered | `PSC_TX[…]: …` log lines appear for ASCII printable text | ⏳ |
| **7** | Filesystem available | `/ata0a/` mounts, `etc/startup.app` found | grep for `dosFsDevInit` success / `iosDevShow` output via monitor | ⏳ |
| **8** | Application boots | `startup.app` runs, Vestas turbine application initializes | banner says `Wind World ...` or task list shows turbine app names (`tApMain`, `tFirecrest`, etc.) | ⏳ |
| **9** | Network listener up | App opens AP / Firecrest / Firedrake ports | `nmap -p 8080,9482,...` from host shows listening ports | ⏳ |
| **10** | Toolkit recognizes turbine | VMP6000 Toolkit lists our QEMU under "available turbines" with the right board ID | Toolkit UI screenshot showing CT6003_Motherboard_V3 entry | ⏳ |
| **11** | Toolkit reads parameters | Toolkit reads a parameter (e.g. RatedPower) and gets a plausible value | Toolkit UI shows non-zero value, not "comm error" | ⏳ |
| **12** | **Toolkit performs software load** | Toolkit pushes a firmware/binary file via Firedrake/AP and the turbine accepts it | Toolkit UI shows "load successful" + QEMU logs the FTP/Firedrake receive | ⏳ **END GOAL** |

### Where we are: gate **2 just cleared, working on 3.**

That's about **25% along the bar to gate 12.** Gates 3–6 are
mechanical (~1–2 weeks). Gates 7–9 are scoping unknowns (days each
once the path is clear). Gates 10–12 are the **long pole** —
weeks to months because we have to reverse-engineer enough of
Vestas-proprietary protocols (AP, Firecrest, Firedrake, NEON) for
the toolkit to be satisfied.

### What's CONFIRMED vs HYPOTHETICAL right now

**Confirmed (evidence in repo):**
- Kernel boots and reaches windExit idle loop (Daniele's diagnosis)
- App tasks block on anonymous FTP boot from `169.254.254.252` —
  user `anonymous`, password `test@cotas.dk`, file `ct6003/vxworks`
- FEC requires BestComm DMA — not optional (manual page 14-1)
- BSP reaches FEC init and runs the full register init sequence
  (this session — log captured)
- FEC TX uses BestComm task slot 2 (TCR @ MBAR+0x1220), RX uses
  slot 3 (TCR @ MBAR+0x1222), enable pattern `sth 0xC0|slot`
  (BSP investigation)
- Cold-start via `tffs=0,0(0,0)` bootline exists as alt path

**Hypothetical (not yet validated):**
- That FTP boot, once it succeeds, leads to runmode without
  additional gating (RTC sanity? key-switch check? Daniele noticed
  a `CT296 KeySwitch` string that gates Ethernet on a physical
  hardware key)
- That a single-node Ground configuration is enough for the
  toolkit to be satisfied — multi-node ARCnet may be required
- That the AP/Firecrest/Firedrake stack can be stubbed without
  fully implementing each protocol's state machine
- That the toolkit accepts our QEMU as a turbine purely on
  network-protocol grounds (vs. some hardware identity check we
  haven't surfaced)

### Biggest unknowns (in rough order of risk to the end goal)

1. **Toolkit acceptance criteria.** We have decompiled toolkit code
   (per CLAUDE.md: AP_PROTOCOL_REFERENCE.md, COMPLETE_CONNECTION_FLOW
   docs in `~/Documents/SharedWithXP/Toolkit/`) but we haven't yet
   reverse-engineered the *minimum* set of protocol responses that
   convince it we're real. This is gate 10–11 work and could be the
   difference between a 1-month and 6-month finish line.
2. **Multi-node necessity.** If the toolkit needs Ground + Top + Hub
   responses simultaneously, single-node PoC won't pass gate 11. We
   may need ARCnet emulation as a hard dependency.
3. **Application boot path.** Even after the kernel boots, getting
   `etc/startup.app` to actually run the Vestas application stack
   may surface dependencies on hardware we haven't modeled
   (CAN, GPIO, ATA, RTC time-of-day, etc.).
4. **BestComm executor complexity.** Near-term blocker for gate 3.
   Plan: snoop BSS pointer at `0x008CFC00` (FEC TX config) for the
   BD ring base, then walk the ring. Could be 1 day or 5 depending
   on Freescale MOTbcommlib config-struct layout.

### What this means in practice

- **Gates 3–6 (next 1–2 weeks):** mechanical, well-scoped. We have
  the manual, BSP findings, and a working scaffolding. Strong
  confidence we land this.
- **Gates 7–9 (couple weeks):** medium uncertainty. Filesystem
  mounting depends on Daniele's still-pending FS investigation;
  application bringup may surface new gaps.
- **Gates 10–12 (months):** the real frontier. Vestas-proprietary
  protocols. The shape of the work depends entirely on what the
  toolkit demands — could be "5 message types" or "comprehensive
  signal namespace + parameter store + live data stream".

### Two big shortcut options to keep in mind

- **Cold-start via `tffs=0,0(0,0)`**: model NAND flash instead of
  FEC+FTP, populate it with the Røye2 dump → skip gates 3–6 entirely
  and jump straight to a runmode boot from disk. Tradeoff: NAND
  modeling is its own ~3-day effort. Worth re-evaluating if BestComm
  executor turns out harder than expected.
- **Pre-loaded vxworks.out**: skip the FTP altogether and load the
  *runtime* image directly via `-device loader,file=...`, bypassing
  the boot-mode protocol. Daniele's findings hint this may be
  possible by changing the bootline. Could save another week.

---

## Strategic decisions log

Append-only. New entries at the top. One line per decision.

- **2026-04-28** — Bootrom direct-boot **DROPPED** as a path forward.
  Implemented a fast NIP/MSR/DEC sampler in `mac_newworld.c` (100 µs
  virtual time, 42 stations, top-30 histogram). Dynamic data shows
  the bootrom reaches `kernelInit` and `windExit` but **never
  dispatches `usrRoot`** — `windLoadContext` UNREACHED, `vxDecSet`
  UNREACHED, CPU spends 98% of samples at `0x0100107c` (intUnlock
  body / kernel idle loop). vxworks.out by contrast hits `sysClkInt`
  (DEC ISR) cleanly under the same QEMU. The bootrom scheduler wedge
  is upstream of the comparison-study's predicted window — fixing it
  would require deep VxWorks kernel-scheduler investigation
  (3–5 days+) for no gate 3 benefit. See
  `BSP_bootrom_scheduler_findings.md`. Stay with path A1
  (vxworks.out + FTP boot via FEC + BestComm executor).
- **2026-04-28** — Comparison study (`BSP_comparison_study.md`) RESOLVED
  the "DEC fires for vxworks.out but not bootrom" mystery. Both binaries
  share identical DEC code (`sysClkEnable → vxDecSet @ mtspr 22`, single
  arming site reached from `usrRoot+0x44`). The bootrom never reaches
  it: usrRoot wedges in the 5-instruction window
  `0x010d6108..0x010d613c` (memInit → memAddToPool → usrMmuInit →
  sysClkConnect → sysClkRateSet → sysClkEnable). MSR[IP] hypothesis
  REFUTED (entry MSR=0x2002, IP=0). "DEC delivery broken in QEMU"
  REFUTED (vxworks.out fires DEC fine, 291 DECR / 4 s). Decision: keep
  path A1 (FTP recovery boot via vxworks.out) as primary; bootrom
  direct-load is unblocked only by a ½-day NIP-trip breakpoint study to
  pinpoint the wedge step. See `BSP_comparison_study.md`.
- **2026-04-28** — Chose path A1 (FTP recovery boot) over A2 (pre-loaded
  runtime) and A3 (tffs cold-start). Reason: richest test surface;
  BestComm executor needed for gate 9 anyway so not wasted; cost ~2
  extra weeks vs A2. See
  `docs/superpowers/specs/2026-04-28-strategy-and-doc-rhythm-design.md`.
- **2026-04-28** — Adopted documentation rhythm: living PLAN.md +
  per-gate commit discipline + this decisions log + per-person plans
  retire when their work is done. Strategy spec captures full reasoning.
- **2026-04-28** — Daniele's diagnosis track complete; findings landed
  in `BSP_park_findings.md`. `PLAN_Daniele.md` retired (kept in repo
  for history; no longer load-bearing).

---

## Doc map (when you come back cold)

What to read in order, after a week away:

1. **`PLAN.md`** (this file) — current gate position + decisions log
2. **Most recent findings doc** — raw evidence for current state
   - Latest: `BSP_bootrom_scheduler_findings.md` (bootrom scheduler wedge pinpointed; bootrom path dropped)
   - Prior: `BSP_comparison_study.md` (vxworks.out vs bootrom DEC mystery resolved)
   - Prior: `BSP_fec_bestcomm_findings.md` (gate-3 wiring intel)
3. **Whichever per-person plan is active** — current work checklist
   - Active: `PLAN_Kasper.md` (steps 1,2,4,5,6 done; step 3 in progress)
   - Retired: `PLAN_Daniele.md` (his work is done)
   - Placeholder: `PLAN_Phase2.5_filesystem.md` (promote when reaching gate 7)

PLAN.md is the living spec — if a section here contradicts older docs,
PLAN.md wins. Findings docs are *frozen evidence* (citations), not
maintained.

---

## Where we are (2026-04-28)

Kernel loads, executes, scheduler runs, all peripheral stubs init clean.
EXT/DECR exception flow stable — only `EXTERNAL` and `DECR` in the
`-d int` log, no `HV_EMU` or `PROGRAM`.

**Confirmed by Daniele's diagnosis (`BSP_park_findings.md`):** kernel
idles in `windExit`'s reschedule loop polling `kernelState` at RAM
`0x908310`. All application tasks are blocked waiting for anonymous
FTP boot to complete from **`169.254.254.252`** (target boots at
`.254`). Credentials: `anonymous` / `test@cotas.dk`. File:
`ct6003/vxworks`. The previously-suspected "park" at `0x207fe8` was
the saved-NIP from the windExit fast-exit `isync` — a red herring.

Working stubs in `hw/ppc/mac_newworld.c`:
- IC: SLT1 active-source + PerEnc read-to-clear at 0x524
- I2C2 X1226 + AT24Cxx state machine, slaves 0x50/0x57/0x6f
- BestComm/SDMA register file + 16 KiB internal SRAM (per manual §13.13)
- PSC TX-ready stub (no real chars yet)
- EXT delivery gated on populated handler at 0x500

What's missing to unblock boot: a real FEC + BestComm DMA executor +
host-side anonymous FTP serving `ct6003/vxworks`. See `PLAN_Kasper.md`.

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
- `docs/MPC5200_FEC_Chapter14.md` — Fast Ethernet Controller (Ch 14)
- `docs/MPC5200_BestComm_Chapter13.md` — SDMA / BestComm (Ch 13)

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
