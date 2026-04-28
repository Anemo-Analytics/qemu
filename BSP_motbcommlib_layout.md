# MOTbcommlib FEC TX/RX struct layout (from Linux kernel)

Source: agent investigation of Linux kernel `drivers/dma/bestcomm/`
and `include/linux/fsl/bestcomm/`. All GPLv2, fine to reference for
QEMU device-model work (QEMU is also GPLv2).

Local mirrors of relevant files: `/tmp/fec.c`, `/tmp/fec.h`,
`/tmp/bestcomm.h`, `/tmp/bestcomm_priv.h`,
`/tmp/bcom_fec_rx_task.c`.

## Buffer Descriptor format — 8 bytes, NOT 16

```c
struct bcom_fec_bd {
    u32 status;
    u32 skb_pa;
};
```

Total stride = 8 bytes. (Earlier plan assumed 16-byte CPM/QUICC-style
BDs — that was wrong.)

### Status word flags

| Bit  | Mask         | Name                  | Direction | Meaning |
|------|--------------|-----------------------|-----------|---------|
| 30   | `0x40000000` | `BCOM_BD_READY`       | both      | Ownership: 1 = engine owns, 0 = CPU owns |
| 27   | `0x08000000` | `BCOM_FEC_TX_BD_TFD`  | TX        | Transmit Frame Done — last BD in frame |
| 27   | `0x08000000` | `BCOM_FEC_RX_BD_L`    | RX        | Last BD in frame |
| 26   | `0x04000000` | `BCOM_FEC_TX_BD_TC`   | TX        | Transmit CRC |
| 25   | `0x02000000` | `BCOM_FEC_TX_BD_ABC`  | TX        | Append Bad CRC |
| 0:10 | `0x000007ff` | `BCOM_FEC_RX_BD_LEN_MASK` | RX    | 11-bit length (RX only) |

## Per-task variable table — in BestComm SRAM

The "config struct" the BSP allocates is **really** the per-task var
table in BestComm SRAM. Located at `bcom_eng->tdt[N].var` (an offset
into MBAR+0x8000 SRAM).

### RX layout (24 bytes)

| Offset | Field       | Description |
|--------|-------------|-------------|
| 0x00   | `enable`    | (u16*) TCR[task] address |
| 0x04   | `fifo`      | FEC RFIFO_DATA phys addr (`0xF0003184`) |
| 0x08   | **`bd_base`** | **Ring base (phys addr)** |
| 0x0C   | `bd_last`   | `bd_base + (num_bd-1)*bd_size` |
| 0x10   | `bd_start`  | Current BD (init = bd_base) |
| 0x14   | `buffer_size` | RX buffer size |

### TX layout (28 bytes — different from RX!)

| Offset | Field       | Description |
|--------|-------------|-------------|
| 0x00   | `DRD`       | Phys addr of self-modified DRD |
| 0x04   | `fifo`      | FEC TFIFO_DATA phys addr (`0xF00031A4`) |
| 0x08   | `enable`    | (u16*) TCR[task] address |
| 0x0C   | **`bd_base`** | **Ring base (phys addr)** |
| 0x10   | `bd_last`   | `bd_base + (num_bd-1)*bd_size` |
| 0x14   | `bd_start`  | Current BD |
| 0x18   | `buffer_size` | Set by µcode per-packet |

### Number of BDs

**No count field.** Derive from the var-table fields:
```
num_bd = (bd_last - bd_base) / 8 + 1
```

## Task Descriptor Table (TDT)

The TDT is the master table indexed by task slot. Layout per entry
(`bcom_tdt`, 32 bytes per entry):

```c
struct bcom_tdt {
    u32 start;        /* desc start */
    u32 stop;         /* desc end (inclusive) */
    u32 var;          /* var-table base — your hook point */
    u32 fdt;
    u32 exec_status;
    u32 mvtp;
    u32 context;
    u32 litbase;
};
```

The TDT lives at `MBAR + 0x1100` typically (after the TaskBAR write
at `MBAR + 0x1200` — but this is engine-private, the TDT is
elsewhere). Actual TDT base TBD from runtime observation.

## Initiator IDs

Per Linux kernel (may differ from Vestas BSP — see findings):

- `BCOM_INITIATOR_FEC_RX = 3`
- `BCOM_INITIATOR_FEC_TX = 4`

**Vestas BSP uses different values** per
`BSP_fec_bestcomm_findings.md`:
- FEC_TX initiator = 16 (0x10)
- FEC_RX initiator = 6

The slot numbers also differ:
- Linux: dynamic
- Vestas: hardcoded slot 2 (TX), slot 3 (RX)

## QEMU executor strategy

For our QEMU BestComm executor:

1. Watch for `sth` writes to TCR[2] (`MBAR+0x1220`) and TCR[3]
   (`MBAR+0x1222`) with bit `0xC0` set → task enable.

2. For the enabled task, look up `tdt[N].var` (in SRAM at
   `MBAR+0x8000+offset`). The TDT base offset within SRAM needs
   to be discovered — likely written during BSP init.

3. Read the var-table at the SRAM offset:
   - For TX (slot 2): `bd_base = var[3]`, `bd_last = var[4]`,
     `bd_start = var[5]`, `buffer_size = var[6]`
   - For RX (slot 3): `bd_base = var[2]`, `bd_last = var[3]`,
     `bd_start = var[4]`, `buffer_size = var[5]`

4. Walk the BD ring (8-byte stride) starting at `bd_start`:
   - Check BCOM_BD_READY ownership
   - For TX: `dma_memory_read(skb_pa, len)` → `qemu_send_packet`
   - For RX: copy incoming frame to `skb_pa`, set length in status
     word, set BCOM_FEC_RX_BD_L if last frame, clear READY
   - Wrap at `bd_last`

5. Fire BestComm task-done IRQ via the FEC's IRQ output line.

## Open questions

- TDT base address within MBAR+0x8000 SRAM — where exactly does
  the BSP write it? Need runtime observation or `sysHwInit`
  disassembly.
- Whether the Vestas BSP's microcode uses the same var-table
  layout as Linux or has Vestas-specific tweaks. Most likely
  unmodified (per BSP investigation findings).
