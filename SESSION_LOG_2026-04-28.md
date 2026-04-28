# Session log — 2026-04-28

## In one page

**Started the day at gate 2.** Kernel boots, scheduler stable, BSP reaches FEC init but stalls before any BestComm activity. The bootrom direct-load is being explored as an alternative path.

**Ended the day with gate 3 closed and gate 4 implemented.** A real Ethernet frame from the BSP — a gratuitous ARP for `169.254.254.254` — successfully reaches the host via `qemu_send_packet`, captured in pcap.

Three single-line fixes carried the day:
1. **PHY BMCR `0x3101 → 0x3100`** — unblocked MII probe
2. **`SPR_MBAR = 0xF0000000`** — unblocked BSP's TCR address arithmetic
3. **TX walker via TaskBAR/TDT** (not via the BSS handle wrapper)

## How this fits the overall goal

The 12-gate roadmap is the path from "QEMU compiles" (gate 0) to "VMP6000 Toolkit performs a software load against our QEMU turbine" (gate 12). The first 6 gates are mechanical infrastructure (boot the BSP all the way to runmode); gates 7–9 are filesystem + app bringup; gates 10–12 are the actual toolkit-acceptance work which requires reverse-engineering Vestas-proprietary protocols.

**Today moved us from ~25% to ~33% along that bar.** Specifically:

| Before | After |
|---|---|
| ✅ Gates 0–2 done | ✅ Gates 0–2 done |
| 🟡 Gate 3 in progress (FEC init reached, no TX) | ✅ **Gate 3 done — frame on wire** |
| ⏳ Gate 4 unscoped | 🟡 **Gate 4 implemented** (RX hook installed) |
| ⏳ Gates 5–12 | ⏳ Gates 5–12 |

Three sessions ago we didn't have a working FEC device. Two sessions ago we couldn't get past the PHY scan. One session ago the BSP couldn't even compute MBAR addresses. Today we sent an actual Ethernet frame.

The remaining path:
- **Gates 5–6** are now ~half-day each — host FTP server already exists, PSC TX-to-stderr is wired, just need the BSP to retry / start FTP boot client.
- **Gate 7** (filesystem) is multi-day — needs ATA or TFFS modeling.
- **Gates 10–12** are the genuine long pole — weeks to months for toolkit acceptance.

## Today's chronology

### Phase 1 — bootrom investigation (early session)

**Question:** can we boot the embedded bootrom (un-stripped, full symbols) directly instead of the stripped runtime, to make debugging easier?

**Answer:** No. Dispatched 5 parallel agents (static disassembly + dynamic NIP sampler + comparison study). Confirmed:

- vxworks.out (runtime) fires DEC fine: 291 events / 4 s
- Bootrom never fires DEC: 0 events / 4 s
- Bootrom kernel reaches `kernelInit` and `windExit` but **never dispatches `usrRoot`** — `windLoadContext` UNREACHED
- CPU spends 98% of samples at `0x0100107c` (intUnlock idle)
- Fixing the bootrom scheduler wedge would cost 3–5 days for zero gate-3 benefit

**Outcome:** dropped bootrom direct-boot path. Stayed with vxworks.out. See `BSP_bootrom_scheduler_findings.md`, `BSP_comparison_study.md`.

**Diagnostic infrastructure built during this phase** (kept for the rest of the day):
- Fast NIP/MSR/DEC sampler (100 µs virtual time, 60+ stations across 0x100000+ runtime and 0x01000000+ bootrom)
- Top-30 NIP histogram
- BSP base-var dump

### Phase 2 — gate 3 unblock attempt 1 (PHY)

**Question:** why does the BSP, after FEC ETHER_EN, never write to TCR[2] to enable the BestComm TX task?

**Investigation:** Dispatched 4 parallel agents (m5200FecSdmaTaskInit disassembly, FecMiiProbe disassembly, IC/intConnect investigation, big-picture status). Discovery: the *real* `m5200FecMiiProbe` is at `0x0012f084` — calls `m5200FecMiiIsolate` which writes BMCR=0x0400 and polls for read-back. **Our PHY model returned BMCR=0x3101 unconditionally**, with the low bit set "to pass an `andi 0x3f` check" — but the BSP rejects PHYs whose reserved bits aren't zero.

**Fix:** PHY BMCR default `0x3101 → 0x3100`. Now BSP runs full MII basic check + ANAR config + LPA reads. ETHER_EN reached.

**Status:** TCR still not firing. Boot continues into `m5200FecPhyInit` then runtime runs, but TX path inactive.

### Phase 3 — gate 3 unblock attempt 2 (MBAR)

**Question:** the BSP reaches `TaskSetup_TASK_FEC_TX` (we observe `init_dma_image_TASK_FEC_TX`, common installer, post-TCR area all running), but the actual `sth r4, 4636(r8)` write at `0x20bcd0` doesn't land at MBAR+0x1220. Why?

**Investigation:** the BSP's TaskSetup function computes the TCR address as `*(0x908918) + slot*2 + 0x121C`. The BSS variable at `0x908918` is initialised by `vxMBarGet()` which reads SPR 638 (MBAR). With SPR_MBAR=0 (QEMU's generic G2 reset value), the TCR write goes to physical low-RAM `0x1220` instead of MBAR+0x1220.

**Fix (single line):** in `mac_newworld_init`:

```c
mpc5200->cpu->env.spr[SPR_MBAR] = 0xF0000000;
```

Re-applied in the SLT tick handler in case CPU reset clobbers it.

**Result — instant unblock:**

```
BSP: SPR_MBAR = 0xf0000000
BSP: *(0x908918) (MBAR base?) = 0xf0000000
*** BestComm TCR[2] WRITE: val=0x00c2 (ENABLE) ***
*** BestComm TCR[3] WRITE: val=0x00c3 (ENABLE) ***
```

DEC armed, `sysClkInt` running, FEC IRQ delivered. **Gate 3 hardware unblocked.**

### Phase 4 — gate 3 walker

**Question:** the TX walker fires (TCR enable triggers it) but reports "BD ring fields look bogus":

```
BestComm TX: walk var=0x009a3bbc bd_base=0xf0008048 bd_last=0xf000804c bd_start=0xf0008058
```

`bd_base/last/start` all in MBAR+0x8000 SRAM range — not in DRAM as Linux MOTbcommlib expects. What's the actual layout?

**Investigation:** added SRAM write logging (filtered to skip the BSS bzero pass at NIP `0x002053b8`). Captured the full TDT setup:

```
*** BestComm TaskBAR WRITE: 0xf0008000 ***
SRAM W +0x000 sz=4 val=0xf0008200  ; TDT[0].start
SRAM W +0x004 sz=4 val=0xf0008230  ; TDT[0].stop
SRAM W +0x008 sz=4 val=0xf0008600  ; TDT[0].var
...
SRAM W +0x040 sz=4 val=0xf000825c  ; TDT[2].start (FEC TX)
SRAM W +0x044 sz=4 val=0xf00082b4  ; TDT[2].stop
SRAM W +0x048 sz=4 val=0xf0008700  ; TDT[2].var ★ FEC TX VAR-TABLE
...
SRAM W +0x70c sz=4 val=0xf0009400  ; TX var.bd_base ★ BD RING
SRAM W +0x710 sz=4 val=0xf00095f8  ; TX var.bd_last
SRAM W +0x714 sz=4 val=0xf0009400  ; TX var.bd_start
SRAM W +0x704 sz=4 val=0xf00031a4  ; TX var.fifo (TFIFO_DATA!)
SRAM W +0x708 sz=4 val=0xf0001220  ; TX var.enable (TCR[2])
```

**Critical insight: Vestas BSP uses unmodified Linux MOTbcommlib layout.** TaskBAR=`0xF0008000`, 16 TDT entries × 32 bytes, then microcode descriptors, then var-tables, then BD rings — all in SRAM. BD `skb_pa` fields point into DRAM where the actual frame buffers live.

**Fix:** rewrote `mpc5200_bestcomm_walk_tx` to follow the chain via TaskBAR (read from our `bestcomm[]` register file) → `TDT[2].var` → `bd_base/bd_last/bd_start`. Then walk 8-byte BDs, copy `skb_pa` payload via `dma_memory_read`, call `qemu_send_packet`, clear READY, advance cursor, fire EIR.TXF.

**Result — frame on wire:**

```
BestComm TX:   BD[0] @0xf0009400 status=0x4c00003c skb_pa=0x07c06680 len=60 TFD
BestComm TX:   sending frame, len=60
```

```
$ tcpdump -r /tmp/qemu_pcap.bin -n
ARP, Request who-has 169.254.254.254 tell 169.254.254.254, length 46
```

**Gate 3 closed.**

### Phase 5 — gate 4 (RX walker)

Symmetric implementation. `mpc5200_fec_set_rx_hook` installs a callback in `mpc5200_fec_receive` that routes inbound frames to the BestComm RX walker. Walker:

1. Reads TaskBAR from `s->bestcomm[]`
2. Follows `TDT[3].var` to RX var-table at `0xF0008780`
3. Reads `bd_base` (RX layout: at offset +0x08, not +0x0C like TX)
4. Finds the BD at `bd_start`, copies frame to its `skb_pa` (DRAM)
5. Sets status = `(len & 0x7FF) | BCOM_FEC_RX_BD_L`
6. Clears READY, advances cursor, fires EIR.RXF

**Status:** implemented but not yet exercised. Slirp doesn't respond to gratuitous ARP (correct — it's an announce, not a request), so no inbound frames yet to test against.

## Today's commits

In chronological order:

1. `bf7512310b` — bootrom direct-boot dropped (5-agent investigation)
2. `9fbd0d3c8b` — gate 3 hw unblocked docs
3. `4d8ba5eb9f` — **fec: SPR_MBAR=0xF0000000** (single most important fix)
4. `49b997db3f` — bestcomm: TX BD walker scaffolding (handle-based, soon replaced)
5. `4a8d7f32f5` — bestcomm: rename BSS-pointed wrapper to TASK_HANDLE
6. `62c71062ec` — fec: phy bmcr default 0x3100
7. `478f8c2946` — docs: gate 3 partial progress
8. `3eb4785e5e` — **bestcomm: TX walker reads TDT in SRAM, ships 60-byte frame to wire**
9. `10846c2530` — diag: SRAM write logging filtered to skip BSS bzero pass
10. `295a58886e` — **bestcomm: RX walker hook**
11. `48ca6c1526` — bestcomm: fire IntPending bit + IC EXT after TX (experimental)
12. `f961efad70` — docs: gate 3 closed
13. (this commit) — comprehensive documentation

## Open question for next session

**Why does the BSP only send 1 ARP and stop?**

In 60 s wall-clock and 180 s wall-clock runs (~7-22 s virtual time), only the gratuitous ARP fires. Hypotheses:

1. **SDMA task-done IRQ at vec 0x27 not delivered properly.** BSP installs an ISR for SDMA TX completion via `intConnect(0x27, ...)`. We tried setting IntPending bit + IC EXT but BSP didn't progress. The IC PerStat encoding for SDMA Main IRQ source needs work — currently we encode SLT (`0x40000000`) and FEC peripheral (`0x25240000`); SDMA needs its own value.

2. **BSP's FTP boot task hasn't been spawned.** After IP probe, BSP needs to start its boot client. May need more virtual time or a specific kernel event.

3. **BSP needs a duplicate-IP timeout.** Some IP probe variants wait ~2 s after announce. If implemented as `semTake` with timeout, BSP wakes and proceeds.

### Recommended next-session task (estimated 1 day)

1. **Decode IC PerStat encoding for SDMA Main IRQ** — what value does the BSP's EXT handler want at `MBAR+0x524` to dispatch SDMA task IRQs?
2. **Once TX completion IRQ fires, observe BSP's progression** — likely ARP for FTP server (`169.254.254.252`), TCP SYN to `:21`, FTP transfer
3. **Verify RX walker** when slirp responds to the FTP-server ARP — first inbound frame end-to-end
4. **Then gates 5 + 6 fall together** — FTP boot completes + first serial banner

## Document index

The session produced the following permanent reference docs:

- **`PLAN.md`** — living roadmap + decisions log (UPDATED with gate 3 status)
- **`BIG_PICTURE_2026-04-28.md`** — end-of-day snapshot
- **`SESSION_LOG_2026-04-28.md`** — this doc, comprehensive session chronology
- **`BSP_sram_layout_findings.md`** — definitive SRAM/TDT/var-table reference (NEW)
- **`BSP_bootrom_scheduler_findings.md`** — bootrom dropped (Phase 1)
- **`BSP_var_table_findings.md`** — handle-vs-var-table investigation (now superseded by sram_layout)
- **`BSP_existing_fec_code_audit.md`** — 8-commit roadmap that guided implementation
- **`BSP_static_a1_vxworks.md`** + **`BSP_static_a2_bootrom.md`** + **`BSP_comparison_study.md`** — bootrom analysis
- Plus 10+ pre-existing `BSP_*_findings.md` docs

## Strategic implications

**Gate 3 closing is a non-trivial proof point.** The fact that we can drive a real Ethernet frame from the BSP through QEMU validates:

- Our FEC device model (CSR + MII + IRQ wiring)
- Our BestComm SDMA model (register file + SRAM + TaskBAR/TDT chain)
- Our PHY model (`0x3100` BMCR default + persistent writes)
- Our IC/EXT IRQ delivery
- Our DEC/sysClkInt scheduling
- The MBAR SPR fix
- The general "Vestas BSP runs correctly under QEMU" thesis

Everything from gates 4–9 should now be a question of plumbing, not architecture. **Gates 10–12 remain the genuine unknown** — the toolkit's acceptance criteria (what minimum AP/Firecrest/Firedrake stub set convinces it that QEMU is a real CT6003) — and that work is well-scoped via the existing decompiled toolkit code at `~/Documents/SharedWithXP/Toolkit/`.

If I had to put a date on gate 12 today, I'd say **6–10 weeks** with sustained work, conditional on:
- Toolkit acceptance not requiring multi-node ARCnet (gate 11 risk)
- Application-layer protocols stub-able from existing decompilation (gate 10–12 main risk)
- No surprise hardware dependencies in `etc/startup.app` (gate 8 risk)

Worst case if any of those bite: **3–6 months**.
