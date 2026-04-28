# BSP BestComm var-table investigation (2026-04-28)

## TL;DR

The captured 8-word block at `0x009a3bbc`/`0x009a4c68` is **NOT the
Linux MOTbcommlib `bcom_fec_*_var` layout**. It is the **Vestas BSP's
MOTbcommlib `bcom_task` HANDLE wrapper** (the heap-allocated handle
returned by `bcom_task_alloc`-equivalent), not the SRAM-resident
var-table. The five `0x48139fxx` values are **stale heap data** —
PowerPC virtual addresses in a `0x48000000`-based HTAB-mapped window
onto the kernel's `.text` segment, specifically pointing at the
TLB-miss handler instruction stream at physical `0x00139fb0..0x00139fc8`,
left over from a prior owner of the heap slab.

## Recommended action

**Stop polling `0x008CFC00` for the var-table.** The pointer there
reaches a heap wrapper, not the var-table. Snoop the task-arm path
(TCR write to `MBAR+0x121C+2*slot`) and follow the chain via the
**TaskBAR / TDT in BestComm SRAM** (`MBAR+0x8000` region) instead,
where the actual var-table lives.

## Evidence

### Captured 8-word handle layout

| word | TX captured | RX captured | meaning |
|---|---|---|---|
| 0 | `0x00000002` | `0x00000003` | task slot index (TX=2, RX=3) |
| 1 | `0x48139fb2` | `0x48139fxx` | residual heap data (virt ptr) |
| 2 | `0x48139fb6` | `0x48139fxx` | residual |
| 3 | `0x48139fba` | `0x48139fxx` | residual |
| 4 | `0x48139fbe` | `0x48139fxx` | residual |
| 5 | `0x48139fca` | `0x48139fxx` | residual |
| 6 | `0x00000010` | `0x00000006` | initiator # (TX=16, RX=6) |
| 7 | `0x00000004` | `0x00000004` | bd_size? |

### Why the `0x48139fxx` values are residual

```
0x48139fb2 - 0x48000000 = 0x00139fb2
0x48139fb6 - 0x48000000 = 0x00139fb6
0x48139fba - 0x48000000 = 0x00139fba
0x48139fbe - 0x48000000 = 0x00139fbe
0x48139fca - 0x48000000 = 0x00139fca
```

Disassembly of vxworks.out at `0x00139fa8..0x00139fcc` (offset
matching after subtracting `0x48000000` virtual base):

```
139fa8: mfspr 0, SPRG0
139fac: mfsrr1 r3
139fb0: mtcrf 128, r3                     <-- ptr[1]
139fb4: mtspr SRR1, r1
139fb8: tlbwe                              <-- ptr[2]
139fbc: lis r3, 69                         <-- ptr[3]
139fc0: lwz r1, 15428(r3)                  <-- ptr[4]
139fc4: addi r1, r1, 1
139fc8: stw r1, 15428(r3)                  <-- ptr[5]
139fcc: rfi
```

This is the **TLB-miss handler instruction stream**. A var-table does
not point at TLB-handler code — these must be residual data from a
prior owner of the `cacheDmaMalloc`'d slab.

### BAT decode

Decoded BATs from `0x100390..0x100440`:

| BAT | virt range | phys | mode |
|---|---|---|---|
| DBAT0 | `0x00000000..0x0FFFFFFF` (256 MB) | `0x00000000` | cached, RW |
| DBAT1 | `0xF0000000..0xFFFFFFFF` (1 GB) | `0xF0000000` | uncached, RW |
| DBAT2 | `0x80000000..0x80FFFFFF` (16 MB) | `0x80000000` | uncached, RW |
| DBAT3 | (unused at boot) | — | — |

`0x48139fxx` is **outside every DBAT range**, so resolves only via
HTAB (Page Tables). Our `cpu_physical_memory_read` bypasses MMU
translation entirely — it can't follow these pointers.

### Why the BSP populates only [0], [6], [7]

The `bcom_task_alloc`-equivalent function in MOTbcommlib does early
init: stores task slot, initiator and bd_size into the handle. The
descriptor/var/inc/FDT pointers in [1..5] are filled in later, after
`bcom_load_image()` allocates SDMA SRAM space and computes addresses.
Our snoop runs before `bcom_load_image` completes, so we capture
pre-init garbage.

## What we should do instead — Option A (preferred)

The BSP's `bcom_load_image()` writes the SRAM-resident TDT at
`MBAR+0x8000+TDT_OFFSET`. Each `bcom_tdt` entry stores `start`,
`stop`, `var` — and these **are physical** addresses by construction
(BestComm DMA can only see XLB physical).

Hook the SRAM region's write callback in QEMU and:

1. Watch for a 32-byte-aligned write pattern matching `bcom_tdt`
   semantics in the SRAM range.
2. Once `var` is populated, use
   `cpu_physical_memory_read(var, 28)` to read `bcom_fec_tx_var`
   (or 24 bytes for RX). These are real physical addresses into
   the BD ring.
3. Walk `var->bd_base..var->bd_last` with stride 8.

## Option B (fallback)

Software TLB walk via `env->sdr1` and `env->sr[]` to translate
each `0x48139fxx` to physical. More work, more fragile.

## Codebase updates landed

- Renamed `BCOM_FEC_TX_VAR_*` to `BCOM_TASK_HANDLE_*` for fields
  [0..7] of the handle wrapper at `0x008CFC00`. Real var-table
  offsets kept under `BCOM_FEC_TX_VAR_*`/`BCOM_FEC_RX_VAR_*` for
  later use once we locate the var-table via TDT.
- Replaced misleading "var-table dump" in `mac_newworld.c` with a
  clearer "task handle" log showing only the trustworthy fields
  (slot, initiator, bd_size).

## Sources

- Agent investigation 2026-04-28
- `/tmp/vxworks_romfs/vxworks.out` (stripped runtime)
- `/tmp/fec.c` (Linux MOTbcommlib mirror)
- `/tmp/bestcomm_priv.h` (Linux bcom_tdt struct)
- `BSP_motbcommlib_layout.md` (earlier layout doc)
- `BSP_fec_bestcomm_findings.md` (note: function-address mapping
  there appears stale per agent's address validation)
