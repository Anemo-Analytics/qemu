# BIG_PICTURE_2026-04-28.md (UPDATED end-of-day)

## Snapshot — QEMU MPC5200 turbine BSP

**End goal:** VMP6000 Toolkit on Windows VM connects to QEMU turbine, recognizes it as CT6003, performs a software load.

**Today's headline:** Gate 3 hardware-side unblocked. **TCR[2]=0xC2 / TCR[3]=0xC3 fire, BestComm tasks engage, our TX BD walker triggers, vxDecSet runs, FEC IRQ system functional.** Remaining: BD ring layout fix.

---

## A. 12-gate roadmap status (updated)

| # | Gate | Status | Notes |
|---|---|---|---|
| 0 | QEMU builds, kernel loads | ✅ done | baseline |
| 1 | Stable scheduler, only EXT/DECR | ✅ done | gate 1 |
| 2 | BSP reaches FEC init | ✅ done | 72 FEC writes, full PHY config |
| 3 | BestComm TX executor | 🟡 **75% done** — TCR fires, walker triggers, ring layout TBD | |
| 4 | BestComm RX executor | ⏳ pending | symmetric, scaffolding ready |
| 5 | FTP boot completes | ⏳ pending | host FTP server already in `scripts/mpc5200_boot_ftp.py` |
| 6 | First serial banner | ⏳ pending | PSC TX hook in place |
| 7 | Filesystem mounted | ⏳ pending | placeholder plan |
| 8 | App boots | ⏳ pending | unscoped |
| 9 | Network listeners | ⏳ pending | unscoped |
| 10 | Toolkit recognizes turbine | ⏳ pending | longest pole |
| 11 | Toolkit reads parameters | ⏳ pending | longest pole |
| 12 | **Toolkit performs software load** | ⏳ pending | **END GOAL** |

---

## B. Today's wins (2026-04-28)

### Critical fixes shipped

1. **PHY BMCR default 0x3101 → 0x3100** (commit `62c71062ec`)
   Unblocks BSP's MII probe path (which rejects PHYs with non-zero reserved bits in BMCR low 6).

2. **SPR_MBAR = 0xF0000000** (commit `4d8ba5eb9f`)
   QEMU's generic G2 init resets MBAR to 0; MPC5200 expects `0xF0000000`. With this fix, BSP's `vxMBarGet()` returns the right value, and `TaskSetup_TASK_FEC_TX/RX` correctly compute TCR addresses. **Single most impactful change of the session.**

### Diagnostic infrastructure

- Fast NIP/MSR/DEC sampler (100 µs virtual time, 60+ stations across bootrom + vxworks runtime)
- Top-30 NIP histogram across 8 MB of .text
- BSP base-var dump (`*(0x908918)` and SPR_MBAR)
- TX BD walker scaffolding hooked to TCR[2] write
- TCR write detector with enable-bit decoding

### Findings docs (committed)

- `BSP_bootrom_scheduler_findings.md` — bootrom direct-boot dropped (kernel scheduler wedge in windExit)
- `BSP_var_table_findings.md` — BSS@0x008CFC00 is task HANDLE wrapper, not var-table; real var-table via TDT in SRAM
- `BSP_existing_fec_code_audit.md` — 8-commit incremental roadmap for gate 3
- `BSP_static_a1_vxworks.md` + `BSP_static_a2_bootrom.md` + `BSP_comparison_study.md`
- `BIG_PICTURE_2026-04-28.md` (this doc)

### Strategic decisions

- **A1 (FTP recovery via vxworks.out + FEC + BestComm) locked**
- **Bootrom direct-boot DROPPED** — scheduler wedges
- **Documentation rhythm formalized**

---

## C. Today's runtime evidence

### Proof gate-3 hardware works

```
BSP: SPR_MBAR = 0xf0000000
BSP: *(0x908918) (MBAR base?) = 0xf0000000
*** BestComm TCR[2] WRITE: val=0x00c2 (ENABLE) ***
*** BestComm TCR[3] WRITE: val=0x00c3 (ENABLE) ***
BestComm TX: walk var=0x009a3bbc bd_base=0xf0008048 bd_last=0xf000804c bd_start=0xf0008058
BestComm TX: BD ring fields look bogus, abort walk
```

### Stations reached (vxworks.out)

| Station | NIP hit | Meaning |
|---|---|---|
| `m5200FecEndLoad` body | 0x0012c460 | EndLoad runs |
| `m5200FecMiiProbe` (real) | 0x0012f0b0 | PHY probe in progress |
| `m5200FecPhyInit` (real) | 0x0012f4e8 | PHY init scan running |
| `m5200FecMiiBasicCheck` | 0x0012eda4 | basic-check loop |
| `m5200FecMiiRead` | 0x0012ec3c | MII reads happening |
| `init_dma_image_TASK_FEC_TX` | 0x0020a3b0 | TX SDMA microcode init |
| `SDMA common installer` | 0x0020b8e0 | shared SDMA setup |
| `SDMA TX setup post-TCR` | 0x0020bdd8 | **past TCR write** |
| `SDMA RX setup body` | 0x0020ba70 | RX setup running |
| `vxDecSet (runtime)` | 0x00207a18 | **DEC armed!** |
| `sysClkInt` | 0x001180bc | **DEC ISR running!** |

### Counts

- 72 FEC register writes (full init + PHY scan + MAC + multicast)
- 7 TCR writes (TX/RX enable + re-arms)
- 6 TX BD walker invocations
- 16 FEC IRQ transitions (real edge-driven IRQ delivery)

---

## D. Remaining for gate 3

The TX BD walker triggers but immediately aborts with "BD ring fields look bogus":

```
bd_base=0xf0008048 bd_last=0xf000804c bd_start=0xf0008058
```

These addresses are in MBAR+0x8000 (SRAM), not in main DRAM. They look like **pointers into the SDMA descriptor area in SRAM, not BD ring base addresses**. The Vestas BSP's task handle layout differs from Linux MOTbcommlib (which expects bd_base at handle+0x0C in DRAM).

### Next-session task (estimated 1 day)

1. **Snoop SRAM writes during boot** — log every write to `0xF0008000..0xF000BFFF` with NIP/value to find where the BSP populates the TDT and var-tables.
2. **Identify TDT base** — TaskBAR (`MBAR+0x1200`) currently reset value `0xFC003000` (per existing init); verify what BSP actually writes there.
3. **Decode the actual var-table** — at `tdt[N].var` (offset +0x08 of each 32-byte TDT entry).
4. **Re-engineer the walker** — read `bd_base` from the real var-table (in SRAM), then walk 8-byte BDs in main DRAM.

---

## E. Realistic remaining path

| Gate | Estimate | Risk |
|---|---|---|
| 3 (TX walker BD layout) | **half-day to 1 day** | low — issue understood |
| 4 (RX walker) | **1 day** | low |
| 5 (FTP boot) | **half-day** | low — host FTP server exists |
| 6 (serial banner) | **half-day** | low |
| 7 (filesystem) | **multi-day** | medium — needs ATA or TFFS |
| 8 (app boots) | **unknown** | medium |
| 9 (network listeners) | **unknown** | medium |
| 10 (toolkit recognition) | **multi-day to weeks** | **highest** |
| 11 (parameters read) | **weeks** | high |
| 12 (software load) | **weeks to months** | high |

### Top 3 risks

1. **Toolkit acceptance criteria undefined**
2. **Multi-node ARCnet possibly required**
3. **App-boot may surface new peripheral dependencies**

---

## F. Recommended next session

1. **TDT/var-table snooping** to fix BD walker
2. **Verify TX path end-to-end** — once walker reads correct BD addresses, frames should show on host tcpdump
3. **RX walker symmetric implementation**
4. **FTP boot smoke test**

If TX path works first try, gates 4-6 may all fall in one session.

---

**One-line summary:** SPR_MBAR fix unlocked gate 3. Hardware is now fully wired: TCR enables fire, BestComm tasks engage, FEC IRQs flow, DEC armed, scheduler running. Remaining work for gate 3 is decoding the Vestas-specific BD ring layout — half-day estimated.
