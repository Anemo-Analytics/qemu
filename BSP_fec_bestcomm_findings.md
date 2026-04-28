# BSP wiring of MPC5200 FEC ↔ BestComm DMA

Investigation of `/tmp/vxworks_romfs/vxworks.out` to determine how the
Vestas BSP programs BestComm for FEC TX/RX. Required for the QEMU
`mpc5200-fec` device-model BestComm executor — answers "which task
slots, what registers, what data path".

## TL;DR

| Thing | Value | Source |
|---|---|---|
| FEC TX → SDMA task slot | **2** | `init_dma_image_TASK_FEC_TX` first store |
| FEC RX → SDMA task slot | **3** | `init_dma_image_TASK_FEC_RX` first store |
| FEC TX initiator # | 16 (0x10) | second store at +0x18 |
| FEC RX initiator # | 6 | second store at +0x18 |
| TCR[2] address | `MBAR+0x1220` | 2 × slot + 0x121C |
| TCR[3] address | `MBAR+0x1222` | 2 × slot + 0x121C |
| TCR enable pattern | `sth 0xC0\|slot` | `0xC2` for TX, `0xC3` for RX |
| FEC RX FIFO data port | `0xF0003184` | manual + BSP confirmation |
| FEC TX FIFO data port | `0xF00031A4` | manual + BSP confirmation |
| Task table base | `MBAR+0x1200` (TaskBAR) | manual; not seen explicitly written but expected in `sysHwInit` |

## BSP code path

- **`m5200FecSdmaTaskInit`** at runtime `0xac6xx` is the FEC ↔ BestComm
  binding function. Error strings:
  `m5200FecSdmaTaskInit: failed to setup TX task` /
  `... RX task`.
- Calls outer setup helpers:
  - `0x18bbd8` for TX → `init_dma_image_TASK_FEC_TX` (`0x18a3b0`) →
    common installer `0x18b848`
  - `0x18b9fc` for RX → `init_dma_image_TASK_FEC_RX` (`0x18a5f8`) →
    common installer `0x18b848`
- The BSP uses the **stock Freescale "MOTbcommlib" / BestComm API** —
  file names embedded in the binary include `bestcomm_api.c`,
  `dma_image.c`, `dma_image.reloc.c`, `load_task.c`,
  `tasksetup_bdtable.c`, `tasksetup_fec_rx_bd.c`,
  `tasksetup_fec_tx_bd.c`. This is the unmodified Freescale codebase.

## Task config pointer table (BSS)

Runtime addresses `0x008CFBF8 .. 0x008CFC18` hold one pointer per
BestComm task struct, populated at boot by `cacheDmaMalloc`:

| Address | Task |
|---|---|
| `0x8CFBF8` | TASK_PCI_TX |
| `0x8CFBFC` | TASK_PCI_RX |
| `0x8CFC00` | **TASK_FEC_TX** |
| `0x8CFC04` | **TASK_FEC_RX** |
| `0x8CFC18` | TASK_GEN_DP_0 |

These pointers are the gateway to the per-task config (BD ring base,
buffer sizes, etc.) — but the structs themselves are driver-private,
not QEMU-visible.

## Vestas-specific naming

- All FEC driver functions prefixed `m5200Fec*`
  (`m5200FecEndLoad`, `m5200FecInt`, `m5200FecSdmaTaskInit`,
  `m5200FecRestart`, `m5200FecRxDrain`, …)
- Source files: `m5200FecEnd.c`, `MPC5200/bsp/ct296_eth.c`
  (Vestas board name **CT296**)
- Task names: `tFecEndRx`, `tFecEndRecover`
- Strings: `"CT296 KeySwitch"`, `"Key switch out of position LOCAL"` —
  Vestas-specific guard that gates Ethernet on a physical key switch.
  May matter if we hit a hang where the BSP refuses to bring up the
  FEC.

## What QEMU does NOT need to model

- **BestComm microcode itself**: the 16-task microcode image
  (`init_dma_image_TASK_*` plus `dma_image.reloc.c` data) at
  `~0x18a000..~0x18bf00` is ~8 KB of host-PowerPC code that
  *generates* SDMA task descriptors. The descriptors get loaded into
  MBAR+0x8000 SRAM but **we don't have to interpret them** — only
  the FEC FIFO + TCR register surface needs to look right.
- Task descriptor internals beyond the basics for the BSP not to fault.
- TaskBAR write at `MBAR+0x1200` (any value accepted).

## Caveats / dead ends in the investigation

- MBAR is cached in a runtime global at `0x91xxxx`; not loaded from a
  hardcoded value. So we can't see the literal `0xF0000000` in the
  BSP code — only via manual + FIFO offsets.
- No explicit `stw` to TaskBAR (`MBAR+0x1200`) was located. Likely
  written once in `sysHwInit` early; chasing it would require
  disassembling the boot path.
- Slots **0** (PCI_TX) and **1** (PCI_RX) have init code present but
  the BSP may not actually instantiate them depending on board variant.
- `m5200FecStop` log strings ("Stop FEC TX&RX Tasks") confirm a
  teardown path that disables TCR[2]/TCR[3] — same register pattern
  with bit 0xC0 cleared.

## Recommended QEMU executor — minimal first cut

For the boot path to even attempt FTP, the FEC + BestComm surface
needs to satisfy:

1. **Accept TaskBAR write at `MBAR+0x1200`** — any value, ignore.
2. **Accept 16-bit `sth` writes at `MBAR+0x121C..0x123A`** (TCR0..15).
   Track which TCRs have bit `0x80` (or `0xC0`) set.
3. **TCR[2] = `0xC2`** → FEC TX task active. Until the executor
   actually moves bytes, just accept and remember.
4. **TCR[3] = `0xC3`** → FEC RX task active. Same.
5. **TX FIFO write at `0xF00031A4`** → for full packet flow, the
   executor must decode task descriptors in SRAM to find frame
   buffers in main RAM, DMA them in, and `qemu_send_packet`.
6. **RX path** → on `mpc5200_fec_receive` callback, executor must
   write the frame into the RX BD's data buffer in main RAM and
   fire the BestComm task-done IRQ.

The hard part of (5)+(6) is **finding the BD ring** without
interpreting BestComm microcode. Two viable paths:

- **A. Snoop the BSP's per-task config struct (0x8CFC00 / 0x8CFC04).**
  When BSP writes a non-NULL pointer there, we know the FEC TX/RX
  config struct address. Walk its fields (offsets defined by Freescale
  MOTbcommlib `tasksetup_fec_*_bd.c`) to find BD ring base.
- **B. Interpret enough of the SDMA microcode** to extract source/dest
  registers from a running task. More general but a lot of work.

Option **A** is concrete because we have specific BSS addresses to
watch. Recommended for the first cut.

## Sources

- `/tmp/vxworks_romfs/vxworks.out` (binary)
- `docs/MPC5200_BestComm_Chapter13.md`
- `docs/MPC5200_FEC_Chapter14.md`
- `BSP_park_findings.md`
