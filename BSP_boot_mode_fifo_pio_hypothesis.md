# Boot-mode FEC: PIO via FIFO mailbox, not BestComm DMA

**Status: REFUTED** — see "Verification result" section at bottom.
The hypothesis below is preserved for context, but the conclusion
turned out to be wrong. The boot-mode kernel DOES use BestComm; the
real question (still open) is why `m5200FecEndLoad` doesn't run in
our QEMU trace.

## The hypothesis

The boot-mode kernel `vxworks.out` does **not** use BestComm DMA tasks
for FTP. It uses **direct polled PIO** through the FEC's Tx/Rx FIFO
data ports:

- `MBAR+0x31A4` — `TFIFO_DATA` (TX FIFO data port)
- `MBAR+0x3184` — `RFIFO_DATA` (RX FIFO data port)
- `MBAR+0x31A8` — `TFIFO_STATUS`
- `MBAR+0x3188` — `RFIFO_STATUS`

The manual quote *"Interrupt driven data movement from the processor
is not supported"* (page 14-1) applies to **interrupt-driven** access.
**Polled** PIO via the FIFO data registers is permitted and is what
the boot kernel actually does.

## Evidence supporting the hypothesis

### 1. NIP-tagged BestComm writes (commit `305cc20a33`)

Every BestComm config write observed comes from **early sysHwInit /
BCom_init**, never from the FEC driver region:

| Offset (MBAR) | Value | NIP | LR | Likely caller |
|---|---|---|---|---|
| 0x1241 | 0x07 | `0x0011d25c` | `0x0011d1f0` | BCom_setSourcePri |
| 0x1242 | 0x07 | `0x0011d260` | `0x0011d1f0` | BCom_setSourcePri |
| 0x123F | 0x05 | `0x0011d268` | `0x0011d1f0` | BCom_setSourcePri |
| 0x1240 | 0x06 | `0x0011d270` | `0x0011d1f0` | BCom_setSourcePri |
| 0x124C | 0x04 | `0x0011d278` | `0x0011d1f0` | BCom_setSourcePri |
| 0x123C | 0x03 | `0x0011d280` | `0x0011d1f0` | BCom_setSourcePri |
| 0x1200 | 0xF0008000 | `0x001329F4` | `0x0011D2A8` | BCom_init (TaskBar) |
| 0x1218 | 0xFFFFFFFF | `0x00132960` | `0x001329FC` | BCom_init (mask) |
| 0x1214 | 0xFFFFFFFF | `0x0013296C` | `0x001329FC` | BCom_init (pending) |
| 0x1218 | 0xEFFFFFFF | `0x001328C8` | `0x001329FC` | BCom_init (mask) |
| 0x123C | 0x07 | `0x00132A14` | `0x00132A0C` | BCom_init |
| 0x1212 | 0x0001 | `0x00132A24` | `0x00132A0C` | BCom_init (PtdControl) |

None of these NIPs are anywhere near the FEC driver region
(`0x12c000+`). The BSP NEVER executes inside `m5200FecEndLoad`
(entry at `0x12c184`) or `m5200FecSdmaTaskInit` (inline at
`0x12c5d0`+).

### 2. CPU is permanently in idle loop

NIP sampling at every SLT timer tick (60 Hz) shows CPU oscillating
between `0x00207F6C` and `0x00207FE8` — the windExit reschedule loop
per Daniele's diagnosis. Never observed executing in `0x12cXXX` /
`0x12dXXX` / `0x20bXXX` (FEC driver / TaskSetup_FEC region).

LR consistently `0x001762B8` — kernel scheduler return.

### 3. `m5200FecEndLoad` is a runtime END driver

Per agent investigation, `m5200FecEndLoad` is registered in the END
function table at `0x008F7F40`. It's invoked synchronously from
`muxDevLoad` / `endLoad`, which are part of the **VxWorks application
network stack init** during `usrAppInit`.

In boot mode, the BSP is waiting for FTP — application init hasn't
happened yet. EndLoad would only run AFTER the application is
loaded from FTP and starts up.

### 4. The BSS task config pointers were misleading

Earlier we thought `[0x008CFC00] = 0x009A3BBC` (FEC TX cfg) and
`[0x008CFC04] = 0x009A4C68` (FEC RX cfg) being non-null indicated
TaskCreate ran. Per agent investigation, **these are statically
initialized in `.data`** at link time. They prove nothing about
runtime behavior. Still appears in our snoop log — but the BSP
never actually wrote them.

## Implications for QEMU implementation

### Old plan (now wrong)

Build a BestComm task executor:
- Watch TCR[2]/[3] writes
- Walk per-task var-table in SRAM
- Walk BD rings in main RAM
- DMA bytes between RAM and FEC FIFO

This is **3–5 days of work** and **completely unnecessary for boot
mode.**

### New plan (correct one)

Model the FEC TX/RX FIFOs as a packet mailbox:

**TX path:**
1. BSP writes word-by-word to `MBAR+0x31A4` (TFIFO_DATA)
2. Accumulate bytes into a frame buffer in our device state
3. BSP signals end-of-frame somehow (probably via TFIFO_CNTRL or
   X_CNTRL.TX_FORCE_DONE)
4. On EOF, call `qemu_send_packet` with accumulated bytes
5. Reset frame buffer

**RX path:**
1. In `mpc5200_fec_receive` callback, store incoming frame in our
   internal RX FIFO buffer
2. Set RFIFO_STATUS to indicate data available
3. Fire FEC IRQ (already wired)
4. BSP reads `MBAR+0x3184` (RFIFO_DATA) word-by-word until EOF
5. BSP marks RX BD/FIFO consumed

**Status registers:**
- `RFIFO_STATUS` (`MBAR+0x3188`) — must reflect "frame available"
  when we have data
- `TFIFO_STATUS` (`MBAR+0x31A8`) — must reflect "FIFO has space"
  most of the time
- `RFIFO_LWF_PTR`, `TFIFO_LWF_PTR` — last write frame pointer; may
  matter for the BSP's frame-boundary detection

**Estimated effort:** 1–2 days of focused work vs. 3–5 days for
BestComm executor.

## Open questions for verification

1. **Does the boot kernel actually access `MBAR+0x31A4` /
   `MBAR+0x3184`?** Static binary analysis — search disassembly for
   direct loads/stores to those addresses, or constants `0x31A4` /
   `0x3184` in the boot kernel's FTP code path.

2. **What's the EOF marker?** Some FEC FIFO designs use a write to a
   separate "frame end" register; some encode it in the data stream;
   some rely on FIFO_CNTRL bits.

3. **Is there an alternative path we're missing?** Maybe the boot
   kernel uses some bootloader-only DMA mechanism we haven't
   identified. Or maybe it uses a "small" BestComm config that
   bypasses the full task setup.

4. **Could `m5200FecEndLoad` be running but our NIP sampling missing
   it?** Sampling at 60 Hz is coarse. A fast EndLoad call could
   complete between samples. But — TCR writes wouldn't be missed by
   our MMIO logging (which catches every access, not sampled).

## Verification agents dispatched

See task list — agents running to verify or refute the hypothesis
before we commit to the FIFO-mailbox implementation.

## Files referenced

- `/tmp/vxworks_romfs/vxworks.out` — VxWorks boot kernel
- `BSP_post_phy_init_findings.md` — context for m5200FecEndLoad
- `BSP_fec_bestcomm_findings.md` — initial (now-superseded)
  expectation of BestComm executor
- `BSP_motbcommlib_layout.md` — BD format etc. (for runtime, not
  boot mode)

---

## Verification result (added 2026-04-28, after agents returned)

### Agent 1 (FIFO-PIO hypothesis) — REFUTED

**Direct disassembly search of `vxworks.out` plus an embedded bootrom
ELF found inside its `.data` segment:**

- 0 direct CPU loads/stores to FEC FIFO data registers
  (`0x31A4`, `0x3184`) in either binary
- 0 absolute-address constants `0xF00031A4` / `0xF0003184`
- 0 `addis rX,0,0xF000` followed by load/store in `0x3000..0x3400`

Where `0x31A4` and `0x3184` DO appear, in identical patterns in both
binaries (runtime `0x12C85C`/`0x12C8E0` and bootrom `0x10E695C`/
`0x10E69E0`), they are: the FIFO physical addresses being **written
into a stack-resident task-init structure** and passed to
`BCom_*TaskSetup` calls. The CPU never directly touches the FIFO;
BestComm SDMA does.

**Both `vxworks.out` and the embedded bootrom use the same
BestComm-DMA-based `m5200FecEndLoad` driver.**

`m5200FecPollSend` / `m5200FecPollReceive` exist (at `0x12B0E4` /
`0x12AFA8`) but "polled" only means "no semaphore/task wait" — they
still spin on **buffer descriptors** which BestComm SDMA fills, not
on FIFO registers directly.

### Agent 2 (TCR-write code paths) — verified

8 static TCR-write instructions in the entire image, all reachable
**only** through `m5200FecEndLoad → TaskSetup_FEC_TX/RX`. No
alternative code path. Confirmed `sysSdmaInit` is at `0x1329d4`
(matches our `0x132xxx` NIP cluster), called once from
`sysHwInit`-style helper at `0x11d180`. `m5200FecEndLoad` confirmed at
`0x12c184`. `m5200FecSdmaTaskInit` is inlined at `0x12c834` (not
`0x12c5d0` as earlier agent said).

### New finding: embedded bootrom ELF

`vxworks.out` contains a **second VxWorks bootrom ELF embedded in
`.data`** at file offset `0xbc2ad0`, ~2.2 MB, with `.text` at vaddr
`0x01000000`. Strings: `/romfs/bootrom_chain_mmc.elf`. Extracted
locally to `/tmp/bootrom_extracted.elf`.

Both binaries share the same `m5200FecEndLoad` driver pattern.

### Implication

The "FIFO PIO" theory was wrong. The path to packet flow IS through
BestComm. So our original plan — build a BestComm executor — is
correct after all. We just hit it later.

But the question we surfaced remains valid: **why does
`m5200FecEndLoad` never execute in our QEMU trace?** Possibilities
the agent listed:

1. `muxDevLoad`/`endLoad` only runs after `usrNetBootConfig`
   succeeds, which depends on earlier system services (NVRAM TLV,
   key switch, etc.). The boot may be wedged earlier.
2. If BestComm task images don't load (no SRAM-loaded microcode,
   unmapped peripheral), `m5200FecSdmaTaskInit` returns failure,
   `muxDevLoad` is never reached.
3. There's another KeySwitch or similar gate we haven't found.

### Updated next steps

- We DO need a BestComm executor (this work isn't wasted)
- We ALSO need to find why `m5200FecEndLoad` doesn't run — fixing
  the executor alone won't help if the BSP never gets there
- Two-track: (a) build BestComm executor (original plan continues),
  (b) investigate what gates `m5200FecEndLoad` from running

The `BSP_motbcommlib_layout.md` and `BSP_fec_bestcomm_findings.md`
docs are NOT superseded after all — they describe what we'll need
when we resume the executor work.
