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
 * Each holds a system-RAM address pointing to the per-task var-table.
 */
#define BCOM_BSS_TX_VAR_PTR         0x008CFC00
#define BCOM_BSS_RX_VAR_PTR         0x008CFC04

/*
 * TX var-table (28 bytes). Field indices match the Linux MOTbcommlib
 * layout. The Vestas BSP allocates this via cacheDmaMalloc and stores
 * its address at BCOM_BSS_TX_VAR_PTR.
 */
#define BCOM_FEC_TX_VAR_DRD         0x00  /* phys addr of self-modified DRD */
#define BCOM_FEC_TX_VAR_FIFO        0x04  /* FEC TFIFO_DATA = 0xF00031A4 */
#define BCOM_FEC_TX_VAR_ENABLE      0x08  /* TCR[2] addr (= MBAR+0x1220) */
#define BCOM_FEC_TX_VAR_BD_BASE     0x0C
#define BCOM_FEC_TX_VAR_BD_LAST     0x10
#define BCOM_FEC_TX_VAR_BD_START    0x14
#define BCOM_FEC_TX_VAR_BUF_SIZE    0x18

/* RX var-table (24 bytes). */
#define BCOM_FEC_RX_VAR_ENABLE      0x00  /* TCR[3] addr (= MBAR+0x1222) */
#define BCOM_FEC_RX_VAR_FIFO        0x04  /* FEC RFIFO_DATA = 0xF0003184 */
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
