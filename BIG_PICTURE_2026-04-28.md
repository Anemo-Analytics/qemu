# BIG_PICTURE_2026-04-28.md (final end-of-day v2)

## Snapshot — QEMU MPC5200 turbine BSP

**End goal:** VMP6000 Toolkit on Windows VM connects to QEMU turbine, recognizes it as CT6003, performs a software load.

**Today's headline:** **GATE 3 SUBSTANTIALLY DONE.** TX BD walker fully working — BSP's ARP frames hit the host network. SRAM TDT layout fully decoded. RX walker implemented. The kernel runs DEC, sysClkInt, FEC IRQ. Remaining: post-ARP BSP progress (FTP boot client startup).

---

## A. 12-gate roadmap status

| # | Gate | Status | Notes |
|---|---|---|---|
| 0 | QEMU builds, kernel loads | ✅ done | |
| 1 | Stable scheduler, only EXT/DECR | ✅ done | |
| 2 | BSP reaches FEC init | ✅ done | |
| 3 | **BestComm TX executor** | **✅ DONE — frame on wire** | TX walker reads TDT.var → bd_base/last/start → walks BDs → qemu_send_packet |
| 4 | **BestComm RX executor** | 🟡 **implemented, not yet exercised** | Hook installed in mpc5200_fec_receive; awaits real inbound frame |
| 5 | FTP boot completes | ⏳ pending | BSP doesn't proceed past 1st ARP yet |
| 6 | First serial banner | ⏳ pending | |
| 7 | Filesystem mounted | ⏳ pending | |
| 8 | App boots | ⏳ pending | |
| 9 | Network listeners | ⏳ pending | |
| 10 | Toolkit recognizes turbine | ⏳ pending | longest pole |
| 11 | Toolkit reads parameters | ⏳ pending | longest pole |
| 12 | **Toolkit performs software load** | ⏳ pending | **END GOAL** |

---

## B. Today's wins

### Critical fixes shipped (in order)

1. **PHY BMCR default 0x3101 → 0x3100** (`62c71062ec`) — unblocks BSP's MII probe
2. **SPR_MBAR = 0xF0000000** (`4d8ba5eb9f`) — unblocks BSP's TaskBAR / TCR address computation. *Single most important fix.*
3. **TX BD walker rewrite** (`3eb4785e5e`) — reads TaskBAR from register file, follows TDT[2].var chain in SRAM
4. **RX BD walker hook** (`295a58886e`) — symmetric implementation; `mpc5200_fec_set_rx_hook` routes inbound frames

### Diagnostic infrastructure expanded

- SRAM write logger filtered to skip the BSS-bzero pass (NIP 0x002053b8)
- TaskBAR write banner
- 60+ NIP stations including real PhyInit / MiiProbe / MiiBasicCheck addresses
- SPR_MBAR + `*(0x908918)` + `*(0x90851C)` BSS dump
- Top-30 NIP histogram

### Findings docs landed

- `BSP_var_table_findings.md` — handle-vs-var-table discovery
- `BSP_existing_fec_code_audit.md` — 8-commit roadmap
- `BSP_static_a*.md`, `BSP_comparison_study.md` — bootrom analysis
- `BSP_bootrom_scheduler_findings.md` — bootrom dropped
- `BIG_PICTURE_2026-04-28.md` (this doc)

---

## C. Today's runtime evidence

### TX walker output (proof of life)

```
*** BestComm TaskBAR WRITE: 0xf0008000  NIP=0x001329f4 LR=0x0011d2a8 ***
*** BestComm TCR[2] WRITE: val=0x00c2 (ENABLE) ***
*** BestComm TCR[3] WRITE: val=0x00c3 (ENABLE) ***
BestComm TX: TaskBAR=0xf0008000 var=0xf0008700 bd_base=0xf0009400 bd_last=0xf00095f8 bd_start=0xf0009400
BestComm TX:   BD[0] @0xf0009400 status=0x4c00003c skb_pa=0x07c06680 len=60 TFD
BestComm TX:   sending frame, len=60
BestComm TX: walk done, frames sent=1
```

### Pcap evidence

```
$ tcpdump -r /tmp/qemu_pcap*.bin -n
ARP, Request who-has 169.254.254.254 tell 169.254.254.254, length 46
```

A real **gratuitous ARP announcement** by the BSP claiming IP `169.254.254.254` — it has full PHY init, full MAC programming, full SDMA TX path active, and frame data flowing.

### BestComm SRAM layout decoded

```
TaskBAR (MBAR+0x1200) = 0xF0008000

TDT (16 entries × 32 bytes at offset 0x000):
  TDT[2] (FEC TX):  start=0xf000825c stop=0xf00082b4 var=0xf0008700
  TDT[3] (FEC RX):  start=0xf00082b8 stop=0xf00082e4 var=0xf0008780

TX var-table @ 0xf0008700 (Linux MOTbcommlib bcom_fec_tx_var):
  +0x00 DRD       = 0xf00082ac
  +0x04 fifo      = 0xf00031a4 (FEC TFIFO_DATA)
  +0x08 enable    = 0xf0001220 (TCR[2])
  +0x0C bd_base   = 0xf0009400 (BD ring in SRAM)
  +0x10 bd_last   = 0xf00095f8
  +0x14 bd_start  = 0xf0009408 (advanced past BD[0])
  +0x18 buf_size  = 0x000005f2 (1522 = MTU)

RX var-table @ 0xf0008780 (bcom_fec_rx_var):
  +0x00 enable    = 0xf0001222 (TCR[3])
  +0x04 fifo      = 0xf0003184 (FEC RFIFO_DATA)
  +0x08 bd_base   = 0xf0009600
  +0x0C bd_last   = 0xf00097f8
  +0x10 bd_start  = 0xf0009600
  +0x14 buf_size  = 0x000005f2

BD format: u32 status; u32 skb_pa; (8 bytes BE)
  64 BDs per ring. RX BDs all initialised with READY=1 + skb_pa pointing
  to DRAM at 0x07C17420 spaced 0x640 apart (1522 + 70).
```

---

## D. Open question for next session

**Why does the BSP only send 1 ARP and stop?** In 60s wall-clock and 180s wall-clock runs, only the gratuitous ARP fires. Possible causes:

1. **BSP's FTP boot task hasn't been spawned yet.** After IP probe, the BSP needs to start its boot client. May require more virtual time or a specific event.

2. **TX completion IRQ missing.** BSP's intConnect(0x27) wires SDMA TX completion ISR. We tried setting IntPending bit + IC EXT but BSP didn't progress. Probably the IC PerStat encoding for SDMA needs work — currently we return 0x40000000 (SLT) and 0x25240000 (FEC); SDMA needs its own value.

3. **BSP needs ARP REPLY for its own announce.** Some IP probe variants wait for a duplicate-IP timeout (~2s). If implemented as semTake with timeout, BSP wakes after timeout and proceeds.

### Next-session task (estimated 1 day)

1. **Decode IC PerStat encoding for SDMA Main IRQs** — what value does BSP's EXT handler want at 0x524 to dispatch SDMA task IRQ?
2. **Once IRQ fires, observe BSP's progression** — likely ARP for FTP server, SYN to .252:21, FTP transfer
3. **Verify RX walker** when slirp responds to the FTP-server ARP
4. **Then gates 5 + 6 fall together**

---

## E. Realistic remaining path (updated)

| Gate | Estimate | Risk |
|---|---|---|
| 3 | ✅ DONE | – |
| 4 (RX walker) | ✅ implemented; **half-day** to verify with first inbound frame | low |
| 5 (FTP boot) | **half-day** once BSP starts retrying ARP | low |
| 6 (serial banner) | **half-day** | low |
| 7 (filesystem) | **multi-day** | medium |
| 8 (app boots) | **unknown** | medium |
| 9 (network listeners) | **unknown** | medium |
| 10 (toolkit recognition) | **multi-day to weeks** | **highest** |
| 11 (parameters read) | **weeks** | high |
| 12 (software load) | **weeks to months** | high |

### Top 3 risks (unchanged)

1. **Toolkit acceptance criteria undefined**
2. **Multi-node ARCnet possibly required**
3. **App-boot may surface new peripheral dependencies**

---

## F. Recommended next session

**Priority 1:** Figure out IC PerStat encoding for SDMA Main IRQ source. Once SDMA task-done IRQ fires correctly, BSP should naturally proceed past the first ARP.

**Priority 2:** If priority 1 is hard, try faking an ARP reply from slirp side or sending an unsolicited frame to the BSP — verifies RX walker.

**Priority 3:** Look at BSP's boot-line config (env vars, FTP target, retries). Maybe the BSP is configured for a different FTP server or different protocol.

---

**One-line summary:** Gate 3 closed — TX BD walker ships ARP frames to wire. RX walker installed. Single remaining riddle: BSP doesn't retry / doesn't progress to FTP boot. Next session: SDMA IRQ encoding + RX-walker exercise + first real banner.
