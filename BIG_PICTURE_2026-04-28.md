# BIG_PICTURE_2026-04-28.md

## Snapshot — QEMU MPC5200 turbine BSP

**End goal:** VMP6000 Toolkit on Windows VM connects to QEMU turbine, recognizes it as CT6003, performs a software load.
**Today:** gate 2 cleared, gate 3 (BestComm TX executor) ~60% in, blocked on a missing post-MII step in `m5200FecEndLoad`.

---

## A. 12-gate roadmap status

| # | Gate | Status | Notes |
|---|---|---|---|
| 0 | QEMU builds, kernel loads | ✅ done | baseline |
| 1 | Stable scheduler, only EXT/DECR | ✅ done | gate 1 |
| 2 | BSP reaches FEC init | ✅ done (this week) | 72 FEC writes, full PHY config |
| 3 | BestComm TX executor | 🟡 in progress | **blocker below** |
| 4 | BestComm RX executor | ⏳ pending | scaffolding ready, no exercise yet |
| 5 | FTP boot completes | ⏳ pending | host FTP server already in `scripts/mpc5200_boot_ftp.py` |
| 6 | First serial banner | ⏳ pending | PSC TX hook is in place; BSP just hasn't reached printf |
| 7 | Filesystem (`/ata0a`) mounted | ⏳ pending | placeholder `PLAN_Phase2.5_filesystem.md` |
| 8 | App boots (`startup.app`) | ⏳ pending | unscoped |
| 9 | Network listeners up | ⏳ pending | unscoped |
| 10 | Toolkit recognises turbine | ⏳ pending | longest pole — needs AP/Firecrest/Firedrake |
| 11 | Toolkit reads parameters | ⏳ pending | longest pole |
| 12 | Toolkit performs software load | ⏳ pending | **END GOAL** |

### Current blocker (gate 3)

- BMCR fix (`0x3101 → 0x3100`) unblocked PHY init
- BSP now runs full `m5200FecMiiBasicCheck` (ISOLATE-toggle, RESET, ANAR=0x01E1, BMSR poll, ANER/LPA reads) and reaches ECR.ETHER_EN + GADDR1 + R_CNTRL + X_CNTRL + MSCR
- **But still no TCR write in 60s wall-clock**
- `m5200FecEndLoad` aborts somewhere between MII basic check (`0x12c7f4`) and the SDMA TX call (`bl 0x20bbd8` at `0x12c878`)
- TX BD walker scaffolding is committed (`49b997db3f`) but never exercised

---

## B. What's been learned this week

### Confirmed findings

- BSP is generic IEEE 802.3 clause-22, **no vendor PHY ID check**
- BSP wedges at `m5200FecStart`'s **CT296 KeySwitch sentinel** (`0x12d390`); patched at runtime to NOP
- FEC TX = slot 2, RX = slot 3, initiators 16/6, TCR pattern `sth 0xC0|slot`
- BestComm BD stride is **8 bytes** (status u32 + skb_pa u32)
- Var-table lives in **SRAM via TDT**, not at `0x008CFC00`. The block at `0x008CFC00` is the **bcom_task HANDLE wrapper**
- `vxworks.out` boots cleanly, fires 291 DEC events / 4s

### Refuted hypotheses

| Hypothesis | Verdict |
|---|---|
| BSP parks waiting for FTP from `.253` at `0x207fe8` | REFUTED — `.252` is right |
| Boot mode uses FIFO PIO instead of DMA | REFUTED |
| MSR[IP] is wrong for the bootrom | REFUTED |
| DEC delivery is broken in QEMU | REFUTED |
| BSP polls `0x008CFC00` to find the var-table | REFUTED — it's the handle wrapper |

### Strategic decisions

- **A1 (FTP recovery via vxworks.out + FEC + BestComm) locked** as primary path
- **Bootrom direct-boot DROPPED** — scheduler wedges in `windExit`
- **Documentation rhythm formalized**

---

## C. Realistic remaining path

| Gate | Estimate | Risk |
|---|---|---|
| 3 (TX executor) | **half-day** to find the missing EndLoad gate, **1 day** to wire BD walker | low |
| 4 (RX executor) | **1 day** | low |
| 5 (FTP boot completes) | **half-day** | low |
| 6 (serial banner) | **half-day** | low |
| 7 (filesystem mount) | **multi-day** | medium |
| 8 (app boots) | **unknown** | medium |
| 9 (network listeners) | **unknown** | medium |
| 10 (toolkit recognition) | **multi-day to weeks** | **highest** |
| 11 (parameters read) | **weeks** | high |
| 12 (software load) | **weeks to months** | high |

### Top 3 risks

1. **Toolkit acceptance criteria undefined.** 1-month vs 6-month finish.
2. **Multi-node ARCnet necessity.** Single-node may not pass gate 11.
3. **App-boot dependencies.** May pull in unstubbed peripherals.

### Shortcut / pivot options

- **A3 (TFFS cold-start)**: model NAND, ~3-day cost.
- **A2 (pre-loaded vxworks.out via `-device loader`)**: skip FTP entirely.
- **Stub-only toolkit responses** based on `~/Documents/SharedWithXP/Toolkit/AP_PROTOCOL_REFERENCE.md`.

---

## D. This session's deliverables (2026-04-28)

### Concrete deliverables

- BMCR fix (0x3101 → 0x3100) unblocks PHY init
- TX BD walker scaffolding committed
- `BSP_var_table_findings.md` — debunked the BSS-pointer-as-var-table assumption
- `BSP_bootrom_scheduler_findings.md` — bootrom direct-boot dropped
- `BSP_comparison_study.md` — full vxworks.out vs bootrom analysis
- `BSP_existing_fec_code_audit.md` — confirmed code is ~80% there
- Fast NIP sampler infra (42 stations, top-30 histogram)
- 8+ parallel agents across two sessions producing findings

### Unfinished + open questions

- **Why no TCR write?** Gap: `0x12c7f4` (post-MII) → `0x12c878` (SDMA TX call)
- **TDT base in SRAM** — unknown
- **`0x48139fxx` residual pointers** — stale heap or virt-translate?
- **Other potential KeySwitch checks** may surface

---

## E. Recommended next steps (ranked)

1. **Find the missing EndLoad step (`0x12c7f4` → `0x12c878`).** Half-day. Disassemble the 130-byte window.
2. **Wire the TX BD walker once TCR fires.** Half-day.
3. **Locate TDT base in MBAR+0x8000 SRAM.** Half-day.
4. **Wire RX BD walker and `.receive` callback.** 1 day.
5. **Verify FTP boot end-to-end.** Half-day. Yields gates 4 + 5 + 6.
6. **Promote `PLAN_Phase2.5_filesystem.md`** for gate 7.
7. **In parallel (non-blocking):** start a Toolkit deep-read for gate 10.

---

**One-line summary:** PHY is unblocked, TCR isn't firing yet — narrow the gap between MII-init-end and SDMA-TX-call in `m5200FecEndLoad` and gate 3 falls. Gates 4-6 are mechanical from there. The real work starts at gate 10.
