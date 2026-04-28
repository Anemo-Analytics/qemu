/*
 * MPC5200 BestComm task / BD layout shared definitions
 *
 * Used by hw/net/mpc5200_fec.c (FEC device) and hw/ppc/mac_newworld.c
 * (BestComm executor stub) to coordinate FEC TX/RX BD ring walking.
 */

#ifndef HW_NET_MPC5200_BESTCOMM_H
#define HW_NET_MPC5200_BESTCOMM_H

#include "qemu/osdep.h"

/*
 * BD format — 8 bytes, NOT 16. Per Linux's bcom_fec_bd and
 * BSP_motbcommlib_layout.md.
 *
 *   u32 status;
 *   u32 skb_pa;
 *
 * All multi-byte fields are big-endian (PowerPC native).
 */
#define BCOM_FEC_BD_STRIDE          8

/* Status word bit masks (read after big-endian load into a u32). */
#define BCOM_BD_READY               0x40000000  /* engine owns when 1 */
#define BCOM_FEC_TX_BD_TFD          0x08000000  /* TX: last BD in frame */
#define BCOM_FEC_TX_BD_TC           0x04000000  /* TX: append CRC */
#define BCOM_FEC_TX_BD_ABC          0x02000000  /* TX: append bad CRC */
#define BCOM_FEC_RX_BD_L            0x08000000  /* RX: last BD in frame */
#define BCOM_FEC_RX_BD_LEN_MASK     0x000007FF  /* RX: 11-bit length */

/*
 * BSS pointers populated by the Vestas BSP at runtime via cacheDmaMalloc.
 * Each holds a kernel-heap address pointing to a MOTbcommlib `bcom_task`
 * HANDLE (heap wrapper) — NOT to the var-table itself. The handle is a
 * 32-byte struct; the real var-table lives in BestComm SRAM (MBAR+0x8000
 * range) and must be located via the TDT.
 *
 * See BSP_var_table_findings.md (agent investigation 2026-04-28) for the
 * deep dive — TL;DR: the inner pointers in this struct are virtual
 * addresses (resolved via HTAB) into the kernel image; QEMU's
 * cpu_physical_memory_read cannot follow them. Use SRAM TDT snooping
 * instead.
 */
#define BCOM_BSS_TX_TASK_PTR        0x008CFC00
#define BCOM_BSS_RX_TASK_PTR        0x008CFC04

/*
 * MOTbcommlib bcom_task handle layout (32 bytes). The middle 5 fields
 * are virtual pointers into the kernel image (HTAB-resolved) and not
 * directly usable as physical addresses.
 */
#define BCOM_TASK_HANDLE_SLOT       0x00  /* task slot # (TX=2, RX=3) */
#define BCOM_TASK_HANDLE_RSV1       0x04  /* virt ptr (HTAB-mapped) */
#define BCOM_TASK_HANDLE_RSV2       0x08
#define BCOM_TASK_HANDLE_RSV3       0x0C
#define BCOM_TASK_HANDLE_RSV4       0x10
#define BCOM_TASK_HANDLE_RSV5       0x14
#define BCOM_TASK_HANDLE_INITIATOR  0x18  /* initiator # (TX=16, RX=6) */
#define BCOM_TASK_HANDLE_BD_SIZE    0x1C  /* observed value 4 */

/*
 * Real var-tables live in BestComm SRAM, pointed to by tdt[N].var
 * (offset +0x08 of each 32-byte TDT entry). The TDT itself lives at
 * TaskBAR (MBAR+0x1200, default 0xFC003000 — outside QEMU RAM map; the
 * BSP may write a different value at runtime).
 *
 * MOTbcommlib bcom_fec_tx_var (28 bytes): DRD, fifo, enable, bd_base,
 * bd_last, bd_start, buffer_size.
 * MOTbcommlib bcom_fec_rx_var (24 bytes): enable, fifo, bd_base,
 * bd_last, bd_start, buffer_size.
 */
#define BCOM_FEC_TX_VAR_DRD         0x00
#define BCOM_FEC_TX_VAR_FIFO        0x04
#define BCOM_FEC_TX_VAR_ENABLE      0x08
#define BCOM_FEC_TX_VAR_BD_BASE     0x0C
#define BCOM_FEC_TX_VAR_BD_LAST     0x10
#define BCOM_FEC_TX_VAR_BD_START    0x14
#define BCOM_FEC_TX_VAR_BUF_SIZE    0x18

#define BCOM_FEC_RX_VAR_ENABLE      0x00
#define BCOM_FEC_RX_VAR_FIFO        0x04
#define BCOM_FEC_RX_VAR_BD_BASE     0x08
#define BCOM_FEC_RX_VAR_BD_LAST     0x0C
#define BCOM_FEC_RX_VAR_BD_START    0x10
#define BCOM_FEC_RX_VAR_BUF_SIZE    0x14

/*
 * Per BSP_fec_bestcomm_findings.md, the Vestas BSP's BestComm TCR
 * enable pattern is `sth 0xC0|slot` to MBAR+0x121C+2*slot. For FEC:
 *   - TX: TCR[2] at MBAR+0x1220, written as 0x00C2
 *   - RX: TCR[3] at MBAR+0x1222, written as 0x00C3
 *
 * Per BestComm chapter 13 (Table 13-8), the 16-bit TCR layout (MSB=0)
 * decodes 0x00C2 as: AutoStart(8)=1 | HighEn(9)=1 | AS[3:0]=2.
 * Either the BSP relies on AutoStart auto-arm or uses an earlier EN
 * write — for our executor we treat any non-zero TCR write to the
 * FEC slots as an "arm" trigger.
 */
#define BCOM_TCR_TX_OFFSET          0x1220   /* MBAR-relative */
#define BCOM_TCR_RX_OFFSET          0x1222

/* IntPending / IntMask bit indices for FEC tasks (MBAR+0x1214 / 0x1218).
 * MSB=0 numbering: TASK[N] is at bit 16+N. */
#define BCOM_INTP_FEC_TX            (1U << (31 - (16 + 2)))   /* 0x2000 */
#define BCOM_INTP_FEC_RX            (1U << (31 - (16 + 3)))   /* 0x1000 */

#endif /* HW_NET_MPC5200_BESTCOMM_H */
