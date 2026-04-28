# MPC5200 BestComm SRAM layout (Vestas BSP) — definitive

**Verified:** 2026-04-28 via QEMU SRAM-write trace + agent disassembly cross-check.
**Status:** authoritative — supersedes earlier guesses in `BSP_var_table_findings.md`.

## TL;DR

The Vestas BSP uses the **unmodified Freescale MOTbcommlib layout** for everything below the `bcom_task` handle. Only the handle wrapper (heap-allocated, kept at BSS@`0x008CFC00` for TX, `0x008CFC04` for RX) is somewhat Vestas-specific. The TDT and var-tables in BestComm SRAM match the Linux kernel `bestcomm_priv.h` and `fec.c` definitions exactly.

## Memory map

```
0xF0000000  ┌─────────────────────────────┐  MBAR
            │ MPC5200 IMMR registers      │
0xF0001200  │   SDMA register file        │  TaskBAR (32-bit BE) at 0x1200
0xF0001214  │   IntPending                │  TASK[N] bit at (16+N)
0xF0001218  │   IntMask                   │
0xF000121C  │   TCR0 / TCR1               │  16-bit per task
0xF0001220  │   TCR2 / TCR3               │  TX / RX (FEC slots)
0xF0001222  │     ↑ BSP writes 0x00C2     │
            │       (AutoStart|HighEn|AS=2)│
            │       to enable TX task     │
0xF000123C  │   IPR (initiator priority)  │
            │   ...                       │
0xF0003000  │   FEC registers             │
0xF0003184  │   FEC RFIFO_DATA            │  RX data port
0xF00031A4  │   FEC TFIFO_DATA            │  TX data port
            │   ...                       │
0xF0008000  ┌─────────────────────────────┐  Internal SRAM (16 KB)
            │ TDT @ TaskBAR (= 0xF0008000)│  16 entries × 32 B = 512 B
            │   TDT[0]: PCI TX            │
            │   TDT[1]: PCI RX            │
            │   TDT[2]: FEC TX            │  ← our hook
            │   TDT[3]: FEC RX            │  ← our hook
            │   ... (16 slots total)      │
0xF0008200  │ Microcode descriptors       │  copied by init_dma_image_*
            │   (per-task PC/DRD blocks)  │
0xF0008600  │ Function descriptors (FDT)  │
0xF0008700  │ TX var-table (28 B)         │  Linux bcom_fec_tx_var
0xF0008780  │ RX var-table (24 B)         │  Linux bcom_fec_rx_var
0xF0008800+ │ Other task descriptors      │
            │ (PCI TX/RX, GenDP, etc.)    │
0xF0009400  │ TX BD ring (64 × 8 B = 512) │  status + skb_pa
0xF0009600  │ RX BD ring (64 × 8 B = 512) │
0xF000C000  └─────────────────────────────┘
```

## TaskBAR (MBAR+0x1200, 32-bit BE)

Hardware reset value: `0xFC003000` (chip-internal SRAM physical address).
Vestas BSP rewrites it to `0xF0008000` (start of internal SRAM at MBAR+0x8000).

**Where:** `bcom_load_image` from MOTbcommlib, at vxworks.out address ~`0x001329f4`.
**Trigger:** runs once during sysHwInit2, via the BSP's BestComm bring-up.

## TDT entry layout (32 bytes — same as Linux `bcom_tdt`)

```c
struct bcom_tdt {
    u32 start;        /* +0x00  PA of first descriptor word in SRAM     */
    u32 stop;         /* +0x04  PA of last descriptor word              */
    u32 var;          /* +0x08  PA of var-table base in SRAM   ★ hook  */
    u32 fdt;          /* +0x0C  PA of function descriptor table         */
    u32 exec_status;  /* +0x10  engine-private (PC, status)             */
    u32 mvtp;         /* +0x14  engine-private                          */
    u32 context;      /* +0x18  engine-private context save             */
    u32 litbase;      /* +0x1C  engine-private literal base             */
};
```

**Captured TDT[2] (FEC TX) values:**
- start = `0xf000825c`
- stop = `0xf00082b4`
- **var = `0xf0008700`** ← TX var-table base
- fdt = `0xf0008e07`
- ...

**Captured TDT[3] (FEC RX) values:**
- start = `0xf00082b8`
- stop = `0xf00082e4`
- **var = `0xf0008780`** ← RX var-table base
- fdt = `0xf0008e07`

## TX var-table (28 bytes — Linux `bcom_fec_tx_var`)

```c
struct bcom_fec_tx_var {
    u32  DRD;          /* +0x00  Self-modified DRD address (SRAM)        */
    u32  fifo;         /* +0x04  FEC TFIFO_DATA = 0xF00031A4             */
    u32  enable;       /* +0x08  TCR[2] address = 0xF0001220             */
    u32  bd_base;      /* +0x0C  BD ring base in SRAM   ★ what we walk  */
    u32  bd_last;      /* +0x10  bd_base + (n-1)*8                       */
    u32  bd_start;     /* +0x14  current cursor (engine writes back)     */
    u32  buffer_size;  /* +0x18  per-packet (set by µcode = 1522 = MTU)  */
};
```

**Captured TX var-table @ `0xF0008700`:**
- DRD = `0xf00082ac`
- fifo = `0xf00031a4` ✓
- enable = `0xf0001220` ✓
- bd_base = `0xf0009400`
- bd_last = `0xf00095f8`
- bd_start = `0xf0009408` (advanced past BD[0] by engine — verifies TX ran)
- buffer_size = `0x000005f2` (1522)

## RX var-table (24 bytes — Linux `bcom_fec_rx_var`)

```c
struct bcom_fec_rx_var {
    u32  enable;       /* +0x00  TCR[3] address = 0xF0001222             */
    u32  fifo;         /* +0x04  FEC RFIFO_DATA = 0xF0003184             */
    u32  bd_base;      /* +0x08  BD ring base in SRAM   ★ what we walk  */
    u32  bd_last;      /* +0x0C                                           */
    u32  bd_start;     /* +0x10  current cursor                           */
    u32  buffer_size;  /* +0x14  RX buffer size (1522 = MTU)             */
};
```

**Captured RX var-table @ `0xF0008780`:**
- enable = `0xf0001222` ✓
- fifo = `0xf0003184` ✓
- bd_base = `0xf0009600`
- bd_last = `0xf00097f8`
- bd_start = `0xf0009600` (no RX yet)
- buffer_size = `0x000005f2`

## BD format (8 bytes — Linux `bcom_fec_bd`)

```c
struct bcom_fec_bd {
    u32 status;        /* offset 0  */
    u32 skb_pa;        /* offset 4  — physical address of buffer in DRAM */
};
```

### Status word bit layout (32-bit BE)

| Bit (MSB=0) | Mask         | Name                  | Direction | Meaning |
|------|--------------|-----------------------|-----------|---------|
| 1    | `0x40000000` | `BCOM_BD_READY`       | both      | 1 = engine owns; 0 = CPU owns |
| 4    | `0x08000000` | `BCOM_FEC_TX_BD_TFD`  | TX        | Transmit Frame Done — last BD in frame |
| 4    | `0x08000000` | `BCOM_FEC_RX_BD_L`    | RX        | Last BD in frame |
| 5    | `0x04000000` | `BCOM_FEC_TX_BD_TC`   | TX        | Append CRC |
| 6    | `0x02000000` | `BCOM_FEC_TX_BD_ABC`  | TX        | Append Bad CRC |
| 21:31| `0x000007FF` | `BCOM_FEC_RX_BD_LEN_MASK` | RX    | 11-bit received length |

### Captured TX BD[0] at first walk

```
0xf0009400  status=0x4c00003c skb_pa=0x07c06680
            |   = BCOM_BD_READY | BCOM_FEC_TX_BD_TFD | BCOM_FEC_TX_BD_TC | len=60
            ↳ 60-byte frame in DRAM at 0x07C06680 (= ARP packet)
```

### Captured RX BDs (all initialised by BSP)

```
0xf0009600..0xf00097f8 (64 BDs)
  status=0x400005f2 skb_pa=0x07c17420   ← READY=1, len-field=1522 (max)
  status=0x400005f2 skb_pa=0x07c17a60   ← spaced 0x640 = 1600 bytes apart
  ...
```

RX path is fully armed waiting for inbound frames.

## Vestas `bcom_task` HANDLE wrapper (~32+ bytes)

The BSS pointers at `0x008CFC00` (TX) and `0x008CFC04` (RX) reach a heap-allocated wrapper that's NOT the var-table. It stores PA pointers into SRAM (post-init). Layout is Vestas-specific — middle fields are pointers into the microcode descriptor block in SRAM, NOT bd_base values.

**Captured TX handle @ `0x009a3bbc` (post-SPR_MBAR fix):**

| offset | value | meaning |
|---|---|---|
| `+0x00` | `0x00000002` | task slot # (TX = 2) |
| `+0x04` | `0xf0008040` | ptr → SRAM cell (descriptor PA) |
| `+0x08` | `0xf0008044` | ptr → SRAM cell |
| `+0x0C` | `0xf0008048` | ptr → SRAM cell |
| `+0x10` | `0xf000804c` | ptr → SRAM cell |
| `+0x14` | `0xf0008058` | ptr → SRAM cell |
| `+0x18` | `0x00000010` | initiator # (TX = 16, RX = 6) |
| `+0x1C` | `0x00000004` | bd_size (or related) |

**Recommended QEMU access pattern:** **don't follow the handle** — go via TaskBAR → TDT directly:

```c
/* Read TaskBAR from our register file (MBAR+0x1200) */
uint32_t taskbar = ldl_be_phys(&address_space_memory, 0xF0001200);

/* TDT[N].var lives at taskbar + N*0x20 + 0x08 */
uint32_t var = ldl_be_phys(&address_space_memory, taskbar + N*0x20 + 0x08);

/* Then var-table fields per Linux MOTbcommlib layout */
uint32_t bd_base = ldl_be_phys(&address_space_memory, var + 0x0C);  /* TX */
/* (For RX: bd_base at var + 0x08) */
```

This is exactly what `mpc5200_bestcomm_walk_tx` and `mpc5200_bestcomm_rx_hook` in `hw/ppc/mac_newworld.c` do.

## QEMU SPR fix required

Without the SPR_MBAR fix, the BSP's `vxMBarGet()` returns 0, and `TaskSetup_TASK_FEC_TX/RX` writes the `sth r4, 4636(r8)` (TCR write) to physical low-RAM `0x1220` instead of MBAR+0x1220. The BestComm engine therefore never receives the enable signal and TX/RX tasks stay dormant.

**Fix** (in `mac_newworld_init`):

```c
mpc5200->cpu->env.spr[SPR_MBAR] = 0xF0000000;
```

QEMU's generic G2 SPR init resets MBAR to `0` (see `target/ppc/cpu_init.c:411`). MPC5200 hardware reset value is `0xF0000000`. Re-applied in the SLT tick handler in case CPU reset clobbers it.

## Sources

- **SRAM write trace** — `mpc5200_mmio_write` for SRAM range (lines ~933-955 of `hw/ppc/mac_newworld.c`)
- **Linux `bcom_tdt` struct** — `/tmp/bestcomm_priv.h`
- **Linux var-table structs** — `/tmp/fec.c` (mirror of `arch/powerpc/sysdev/bestcomm/fec.c`)
- **Vestas BSP** — `/tmp/vxworks_romfs/vxworks.out` (stripped, BE PowerPC, .text @ `0x00100000`)
- **Bootrom Rosetta** — `/tmp/bootrom_extracted.elf` (un-stripped, .text @ `0x01000000`)
- **Earlier (now superseded) docs:** `BSP_motbcommlib_layout.md`, `BSP_var_table_findings.md`, `BSP_fec_bestcomm_findings.md`

## Verification commands

To re-confirm the layout in a fresh QEMU run:

```bash
cd /home/kasper/qemu
ninja -C build qemu-system-ppc

timeout 30 ./build/qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -nic user,id=n0,model=mpc5200-fec,mac=00:1b:f0:00:00:0a,\
net=169.254.254.0/24,host=169.254.254.252 \
  -object filter-dump,id=f0,netdev=n0,file=/tmp/qemu_pcap.bin \
  -display none -serial null 2>/tmp/qemu.log

grep "TaskBAR\|TCR\[\|BestComm TX:" /tmp/qemu.log
tcpdump -r /tmp/qemu_pcap.bin -n
```

Expected output:
- `*** BestComm TaskBAR WRITE: 0xf0008000 ***`
- `*** BestComm TCR[2] WRITE: val=0x00c2 (ENABLE) ***`
- `*** BestComm TCR[3] WRITE: val=0x00c3 (ENABLE) ***`
- `BestComm TX:   BD[0] @0xf0009400 status=0x4c00003c skb_pa=0x... len=60 TFD`
- `BestComm TX:   sending frame, len=60`
- pcap shows ARP request from `169.254.254.254`
