/*
 * QEMU PowerPC CHRP (currently NewWorld PowerMac) hardware System Emulator
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * PCI bus layout on a real G5 (U3 based):
 *
 * 0000:f0:0b.0 Host bridge [0600]: Apple Computer Inc. U3 AGP [106b:004b]
 * 0000:f0:10.0 VGA compatible controller [0300]: ATI Technologies Inc RV350 AP [Radeon 9600] [1002:4150]
 * 0001:00:00.0 Host bridge [0600]: Apple Computer Inc. CPC945 HT Bridge [106b:004a]
 * 0001:00:01.0 PCI bridge [0604]: Advanced Micro Devices [AMD] AMD-8131 PCI-X Bridge [1022:7450] (rev 12)
 * 0001:00:02.0 PCI bridge [0604]: Advanced Micro Devices [AMD] AMD-8131 PCI-X Bridge [1022:7450] (rev 12)
 * 0001:00:03.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0045]
 * 0001:00:04.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0046]
 * 0001:00:05.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0047]
 * 0001:00:06.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0048]
 * 0001:00:07.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0049]
 * 0001:01:07.0 Class [ff00]: Apple Computer Inc. K2 KeyLargo Mac/IO [106b:0041] (rev 20)
 * 0001:01:08.0 USB Controller [0c03]: Apple Computer Inc. K2 KeyLargo USB [106b:0040]
 * 0001:01:09.0 USB Controller [0c03]: Apple Computer Inc. K2 KeyLargo USB [106b:0040]
 * 0001:02:0b.0 USB Controller [0c03]: NEC Corporation USB [1033:0035] (rev 43)
 * 0001:02:0b.1 USB Controller [0c03]: NEC Corporation USB [1033:0035] (rev 43)
 * 0001:02:0b.2 USB Controller [0c03]: NEC Corporation USB 2.0 [1033:00e0] (rev 04)
 * 0001:03:0d.0 Class [ff00]: Apple Computer Inc. K2 ATA/100 [106b:0043]
 * 0001:03:0e.0 FireWire (IEEE 1394) [0c00]: Apple Computer Inc. K2 FireWire [106b:0042]
 * 0001:04:0f.0 Ethernet controller [0200]: Apple Computer Inc. K2 GMAC (Sun GEM) [106b:004c]
 * 0001:05:0c.0 IDE interface [0101]: Broadcom K2 SATA [1166:0240]
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "hw/ppc/ppc.h"
#include "hw/qdev-properties.h"
#include "hw/nvram/mac_nvram.h"
#include "hw/boards.h"
#include "hw/pci-host/uninorth.h"
#include "hw/input/adb.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/pci/pci.h"
#include "hw/irq.h"
#include "net/net.h"
#include "system/system.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/char/escc.h"
#include "hw/misc/macio/macio.h"
#include "hw/misc/unimp.h"
#include "hw/ppc/openpic.h"
#include "hw/loader.h"
#include "hw/fw-path-provider.h"
#include "elf.h"
#include "qemu/error-report.h"
#include "system/kvm.h"
#include "system/reset.h"
#include "kvm_ppc.h"
#include "hw/usb.h"
#include "hw/net/mpc5200_bestcomm.h"
#include "system/dma.h"

/* Forward declarations from hw/net/mpc5200_fec.c */
void mpc5200_fec_raise_eir(DeviceState *dev, uint32_t bits);
void mpc5200_fec_send_packet(DeviceState *dev, const uint8_t *buf, size_t len);

/* EIR.TXF bit — match the FEC device's macro. */
#define MPC5200_FEC_EIR_TXF  (1U << 27)
#define MPC5200_FEC_EIR_RXF  (1U << 25)
#include "hw/sysbus.h"
#include "trace.h"

/*
 * Minimal MPC5200 MMIO stub for VxWorks BSP emulation.
 *
 * I2C2 registers at MBAR+0x3d40:
 *   +0x00 MADR (address)
 *   +0x04 MFDR (frequency divider)
 *   +0x08 MBCR (control) - write START here to begin transfer
 *   +0x0c MBSR (status)  - bit7=MIF(done), bit1=MCF(complete) -> 0x82
 *   +0x10 MDBR (data)    - read returns next EEPROM byte
 *
 * We return a synthetic version-4 EEPROM image (72 bytes):
 *   [0:4]   CRC32 over bytes 4..64
 *   [4:8]   version = 4
 *   [8:12]  size = 0x3c (60-byte data section)
 *   [12:72] 60 bytes data; byte 19 = board type 23 (CT6003_Motherboard_V3)
 */
#define MPC5200_EEPROM_SIZE 512
static const uint8_t mpc5200_eeprom_init[72] = {
    /* checksum */  0xb8, 0xe1, 0xde, 0x02,
    /* version  */  0x00, 0x00, 0x00, 0x04,
    /* size     */  0x00, 0x00, 0x00, 0x3c,
    /* data[0..6] */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* data[7] board type 23 = CT6003_Motherboard_V3 */ 0x17,
    /* data[8..59] */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00,
};

/*
 * Tiny X1226 (RTC + 512 B EEPROM) state machine on I2C2.
 * X1226 wire protocol: 7-bit slave addr (0x6f for RTC regs, 0x57 for EEPROM
 * user space) + 16-bit internal address. Master writes "[slave|W][hi][lo]"
 * to set the address, then either continues with data bytes (write) or
 * issues a repeated-start with [slave|R] and reads bytes back. The kernel's
 * m5200i2c driver pokes MBCR/MBSR/MDR; we satisfy it by tracking the
 * transaction phase and toggling MIF correctly.
 */
typedef enum {
    I2C_IDLE = 0,
    I2C_ADDR,    /* expecting slave-address byte after START */
    I2C_REG_HI,  /* expecting hi byte of internal address (master TX) */
    I2C_REG_LO,  /* expecting lo byte of internal address (master TX) */
    I2C_DATA,    /* data phase, direction given by `reading` */
} MPC5200I2CPhase;

typedef struct {
    MPC5200I2CPhase phase;
    uint8_t  slave_addr;   /* 7-bit */
    uint16_t reg_addr;     /* X1226 16-bit internal cursor */
    bool     reading;      /* R/W bit from slave-address byte */
    bool     mif;           /* interrupt flag, cleared on MBSR read */
    bool     mcf;          /* transfer complete */
    bool     mbb;          /* bus busy */
    bool     rxak;         /* last receive acknowledge (0 = ACK from slave) */
    uint8_t  mbcr;         /* last value written to MBCR (read-back support) */
    uint8_t  madr;         /* last value written to MADR */
    uint8_t  mfdr;         /* last value written to MFDR */
    uint8_t  mdfsrr;       /* last value written to MDFSRR */
    uint8_t  eeprom[MPC5200_EEPROM_SIZE];
} MPC5200I2CState;

typedef struct {
    MemoryRegion     mr;
    QEMUTimer       *timer;
    QEMUTimer       *diag_timer;     /* fast NIP/MSR/DEC sampler */
    PowerPCCPU      *cpu;
    DeviceState     *fec;            /* mpc5200-fec, for executor callbacks */
    bool             ic_pending;       /* legacy: SLT1 timer pending */
    bool             ic_fec_pending;   /* FEC peripheral interrupt pending */
    bool             ic_sdma_pending;  /* SDMA Main IRQ #0 (BestComm task done) */
    MPC5200I2CState  i2c2;
    /*
     * BestComm/SDMA register file (MBAR+0x1200..0x12FF). Modeled as plain
     * register storage: the BSP writes config values (TaskBar, task control
     * bytes, interrupt masks) and reads them back. We don't simulate any
     * actual DMA; tasks are no-ops as far as the BSP can tell.
     */
    uint8_t          bestcomm[0x100];
    /*
     * MPC5200 internal SRAM (MBAR+0x8000..0xBFFF, 16 KiB per manual §13.13).
     * BestComm stores task descriptors here. The BSP reads/writes it as
     * memory. Addresses 0xC000..0xFFFF are reserved on real hardware —
     * we leave them to the fall-through unimplemented stub.
     */
    uint8_t          sram[0x4000];
} MPC5200State;

static void mpc5200_i2c2_init(MPC5200I2CState *i2c)
{
    memset(i2c, 0, sizeof(*i2c));
    memcpy(i2c->eeprom, mpc5200_eeprom_init, sizeof(mpc5200_eeprom_init));
}

/*
 * Word at physical 0x508 in the freshly-loaded vxworks.out image. We treat
 * any value other than this as evidence that VxWorks has installed a real
 * EXT handler at vector 0x500, after which delivering EXT is safe. Until
 * then, EXT delivery bounces into garbage and HV_EMU-loops forever.
 *
 * The literal `0x13e00c08` decodes (incorrectly, on G2) as the AltiVec
 * `vpmsumb` opcode and is what triggered the original loop.
 */
#define MPC5200_VEC_GARBAGE_AT_508 0x13e00c08u

/*
 * Update the CPU EXT line based on the OR of all IC pending sources.
 * Centralised so we don't have to re-derive the right boolean expression
 * at every call site that toggles one of the per-source flags.
 */
static void mpc5200_update_ext(MPC5200State *s)
{
    int lvl = (s->ic_pending || s->ic_fec_pending || s->ic_sdma_pending)
              ? 1 : 0;
    ppc_set_irq(s->cpu, PPC_INTERRUPT_EXT, lvl);
}

/*
 * Read 32-bit big-endian word from the BestComm register file at
 * MBAR+0x1200+offset. The register file is byte-addressable; we treat
 * 4-byte reads as BE.
 */
static inline uint32_t mpc5200_bc_get32(MPC5200State *s, unsigned offset)
{
    return ((uint32_t)s->bestcomm[offset]     << 24)
         | ((uint32_t)s->bestcomm[offset + 1] << 16)
         | ((uint32_t)s->bestcomm[offset + 2] <<  8)
         |  (uint32_t)s->bestcomm[offset + 3];
}

static inline void mpc5200_bc_put32(MPC5200State *s, unsigned offset,
                                    uint32_t v)
{
    s->bestcomm[offset]     = (v >> 24) & 0xff;
    s->bestcomm[offset + 1] = (v >> 16) & 0xff;
    s->bestcomm[offset + 2] = (v >>  8) & 0xff;
    s->bestcomm[offset + 3] =  v        & 0xff;
}

/*
 * Re-evaluate SDMA Main IRQ pending state.
 *
 * IntPending (MBAR+0x1214) bits AND-NOT IntMask (MBAR+0x1218) bits gives
 * the unmasked-pending set. IntMask convention: 1=MASKED, 0=ENABLED
 * (verified via vxworks 0x12e8e0: clearing a bit *enables* the task's
 * IRQ). If any bit is unmasked-pending, we assert the SDMA Main IRQ
 * line (which the IC encodes as PerStat=0x20000000 — main valid bit
 * set, main source = 0).
 */
static void mpc5200_sdma_eval_irq(MPC5200State *s)
{
    uint32_t intp = mpc5200_bc_get32(s, 0x14);
    uint32_t mask = mpc5200_bc_get32(s, 0x18);
    bool was_pending = s->ic_sdma_pending;
    s->ic_sdma_pending = (intp & ~mask) != 0;
    if (s->ic_sdma_pending != was_pending) {
        static unsigned log = 0;
        if (log++ < 32) {
            fprintf(stderr,
                    "SDMA IRQ %s: IntPending=0x%08x IntMask=0x%08x "
                    "unmasked=0x%08x\n",
                    s->ic_sdma_pending ? "RAISE" : "CLEAR",
                    intp, mask, intp & ~mask);
            fflush(stderr);
        }
    }
    mpc5200_update_ext(s);
}

/*
 * Hook to route the FEC's level-sensitive IRQ output into the EXT path.
 * The IC dispatch (0x524 PerStat/MainStat encoded register) needs to
 * report this as peripheral source 5 (Ethernet) routed via Main
 * source 4 (LO_int) so the BSP's EXT handler dispatches to the FEC ISR
 * (m5200FecInt) and not the SLT1 timer ISR. See mpc5200_mmio_read for
 * the 0x524 path.
 */
static void mpc5200_fec_irq_handler(void *opaque, int n, int level)
{
    MPC5200State *s = opaque;
    static unsigned log_count = 0;
    if (log_count++ < 16) {
        fprintf(stderr, "FEC IRQ -> %d\n", level);
        fflush(stderr);
    }
    s->ic_fec_pending = !!level;
    mpc5200_update_ext(s);
}

/*
 * Patch the BSP's CT296 KeySwitch check so it always passes.
 *
 * Per BSP_post_phy_init_findings.md, m5200FecStart at 0x12d390 calls
 * 0x112630 (CT296DioGetKeySwitch cache reader), compares to 0, and
 * branches to the "simulate PHY init error" path if the cache shows
 * "LOCAL". This BSP build uses FpgaCT296SimKeyGet which reads
 * /fs/fpga_ct296_key.txt via fopen — we have no filesystem yet, so
 * the cache stays in a state the BSP rejects, triggering an infinite
 * m5200FecRestart loop.
 *
 * NOP out the conditional branch so the FEC always starts. Identical
 * patch may be needed inside m5200FecRestart at 0x12ae60 (also a
 * keyswitch check) — applied speculatively.
 *
 * The patches are applied once on the first SLT timer tick, by which
 * time the loader has put the ELF into RAM but the BSP may not yet
 * have hit the patch sites.
 */
static void mpc5200_apply_keyswitch_patches(void)
{
    /* m5200FecStart: beq cr7, 0x12d7e8 → nop */
    static const uint8_t nop[4] = {0x60, 0x00, 0x00, 0x00};
    cpu_physical_memory_write(0x0012d390, nop, 4);
    /* m5200FecRestart: bne- cr7, 0x12aea0 → nop (per agent) */
    cpu_physical_memory_write(0x0012ae60, nop, 4);
    fprintf(stderr,
            "MPC5200: applied CT296 KeySwitch bypass patches at 0x12d390 "
            "and 0x12ae60\n");
    fflush(stderr);
}

/*
 * Fast diagnostic NIP/MSR/DEC sampler. Fires every 100 us of virtual
 * time for the first 100 ms of guest run, then disables itself.
 * Detects when the CPU passes through known boot stations (usrRoot
 * candidates in the bootrom) and logs the first hit on each, plus
 * MSR and DEC SPR snapshots. Independent from the 60 Hz SLT tick.
 */
typedef struct {
    target_ulong addr;
    target_ulong addr_end;
    const char  *name;
    bool         hit;
    target_ulong highest_nip;  /* highest NIP observed within the range */
} BootStation;

static BootStation g_boot_stations[] = {
    /* === outer boot path === */
    { 0x010d8718, 0x010d8807, "usrInit",                                false, 0 },
    { 0x010f2154, 0x010f272f, "excVecInit",                             false, 0 },
    { 0x010f1ff0, 0x010f2153, "excConnectVector helper",                false, 0 },
    { 0x010f2730, 0x010f27f7, "excConnect",                             false, 0 },
    { 0x010f27f8, 0x010f29ff, "excIntConnect",                          false, 0 },
    { 0x010e0b60, 0x010e9e5b, "sysHwInit (large -- includes callees)", false, 0 },
    { 0x010e9e5c, 0x010ea0ff, "sysSdmaInit",                            false, 0 },
    { 0x010ddfec, 0x010de103, "sysSerialHwInit",                        false, 0 },
    { 0x010de104, 0x010de597, "sysSerialHwInit2",                       false, 0 },
    { 0x010dc1a0, 0x010dc7e7, "m5200IntrInit",                          false, 0 },
    { 0x010dc7e8, 0x010dca17, "sysGpioHwInit",                          false, 0 },
    { 0x010e0abc, 0x010e0b5f, "sysCpuCheck",                            false, 0 },

    /* === usrKernelInit and the library-init functions it calls === */
    { 0x010d4e54, 0x010d6047, "usrKernelInit (full body)",              false, 0 },
    { 0x010c8a18, 0x010cbcbf, "taskLibInit",                            false, 0 },
    { 0x010adacc, 0x010b49ff, "taskHookInit",                           false, 0 },
    { 0x010c5480, 0x010c5ce3, "semBLibInit",                            false, 0 },
    { 0x010c5ce4, 0x010c71a3, "semCLibInit",                            false, 0 },
    { 0x010c71a4, 0x010c8a17, "semMLibInit",                            false, 0 },
    { 0x010cbcc0, 0x010ce1f7, "wdLibInit",                              false, 0 },
    { 0x010c358c, 0x010c547f, "msgQLibInit",                            false, 0 },
    { 0x010b4d68, 0x010b523f, "qInit",                                  false, 0 },
    { 0x010ce1f8, 0x010d31e7, "workQInit",                              false, 0 },
    { 0x010a7fc0, 0x010a896b, "memInit (in memPartLib)",                false, 0 },
    { 0x010a896c, 0x010adacb, "memPartLibInit",                         false, 0 },
    { 0x010a1bac, 0x010a1bcb, "cacheLibInit",                           false, 0 },
    { 0x010a1bcc, 0x010a7fbf, "cacheEnable",                            false, 0 },

    /* === kernel scheduling === */
    { 0x010c33c8, 0x010c358b, "kernelInit",                             false, 0 },
    { 0x010d3860, 0x010d3aaf, "windLoadContext",                        false, 0 },
    { 0x010d3640, 0x010d385f, "windExit",                               false, 0 },
    { 0x010d3690, 0x010d370b, "taskCode",                               false, 0 },

    /* === bootrom usrRoot itself === */
    { 0x010d60f8, 0x010d6047 + 0x300, "usrRoot",                        false, 0 },
    { 0x010d6048, 0x010d60f7, "usrMmuInit body",                        false, 0 },
    { 0x010e08a4, 0x010e0abb, "sysClkConnect body",                     false, 0 },
    { 0x010e07fc, 0x010e08a3, "sysHwInit2 body",                        false, 0 },
    { 0x010de5e4, 0x010de7ff, "sysClkRateSet body",                     false, 0 },
    { 0x010de598, 0x010de5e3, "sysClkEnable body",                      false, 0 },
    { 0x01038c1c, 0x01038c2b, "vxDecSet -- DEC IS ARMED",               false, 0 },
    { 0x01038c2c, 0x01038c4b, "vxDecReload (DEC re-arm in ISR)",        false, 0 },
    { 0x010db170, 0x010db26f, "sysClkInt body (DEC ISR)",               false, 0 },

    /* === vxworks.out runtime stations === */
    { 0x0011aae0, 0x0011aaff, "VX: sysClkEnable",                       false, 0 },
    { 0x00207a18, 0x00207a1f, "VX: vxDecSet -- DEC ARMED (runtime)",    false, 0 },
    { 0x00117fd0, 0x001180ff, "VX: sysClkInt",                          false, 0 },

    /* Real PHY init body & probe (per agent investigation) */
    { 0x0012f084, 0x0012f12b, "VX: m5200FecMiiProbe (real)",            false, 0 },
    { 0x0012f12c, 0x0012ffff, "VX: m5200FecPhyInit (real)",             false, 0 },
    { 0x0012ec90, 0x0012edf3, "VX: m5200FecMiiBasicCheck (real)",       false, 0 },
    { 0x0012ef84, 0x0012f083, "VX: m5200FecMiiIsolate (real)",          false, 0 },
    { 0x0012ec3c, 0x0012ec8f, "VX: m5200FecMiiRead",                    false, 0 },
    { 0x0012ebbc, 0x0012ec3b, "VX: m5200FecMiiWrite",                   false, 0 },
    /* === vxworks.out FEC EndLoad gap (gate 3 blocker) === */
    { 0x0012be9c, 0x0012bf6c, "VX: m5200FecMiiProbe",                   false, 0 },
    { 0x0012c184, 0x0012c8bb, "VX: m5200FecEndLoad body",               false, 0 },
    { 0x0012c8bc, 0x0012c8c7, "VX: EndLoad ERROR path entry",           false, 0 },
    { 0x0012c8c8, 0x0012c91f, "VX: EndLoad post-TX-success (RX setup)", false, 0 },
    { 0x0020bbd8, 0x0020bccf, "VX: SDMA TX setup (pre-TCR write)",      false, 0 },
    { 0x0020bcd0, 0x0020bcd3, "VX: TCR[2] WRITE (sth at 4636(r8))",     false, 0 },
    { 0x0020bcd4, 0x0020bdff, "VX: SDMA TX setup post-TCR",             false, 0 },
    { 0x0020b9fc, 0x0020bbd7, "VX: SDMA RX setup body",                 false, 0 },
    { 0x0020b848, 0x0020b9fb, "VX: SDMA common installer",              false, 0 },
    { 0x0020a3b0, 0x0020a5f7, "VX: init_dma_image_TASK_FEC_TX",         false, 0 },
};

/* NIP histogram across full bootrom .text — reveals idle loops. */
#define NIP_HIST_BASE  0x01000000UL
#define NIP_HIST_END   0x011f4000UL
#define NIP_HIST_SIZE  ((NIP_HIST_END - NIP_HIST_BASE) / 4)
static unsigned g_nip_hist[NIP_HIST_SIZE];

static void mpc5200_diag_sample(void *opaque)
{
    MPC5200State *s = opaque;
    static int  diag_count = 0;
    const int   diag_max   = 30000;  /* 30000 × 100us = 3 s of virtual time */
    int         i;

    target_ulong nip = s->cpu->env.nip;
    target_ulong msr = s->cpu->env.msr;
    target_ulong dec = s->cpu->env.spr[SPR_DECR];

    /* Detect first hit on each boot station */
    for (i = 0; i < (int)(sizeof(g_boot_stations) / sizeof(g_boot_stations[0]));
         i++) {
        BootStation *bs = &g_boot_stations[i];
        if (!bs->hit && nip >= bs->addr && nip <= bs->addr_end) {
            bs->hit = true;
            fprintf(stderr,
                    "STATION HIT [%2d] @ NIP=0x%08x MSR=0x%08x DEC=0x%08x : %s\n",
                    i, (unsigned)nip, (unsigned)msr, (unsigned)dec, bs->name);
            fflush(stderr);
        }
    }

    /* Histogram — track samples in the scheduler/dispatch range */
    if (nip >= NIP_HIST_BASE && nip < NIP_HIST_END) {
        unsigned idx = (nip - NIP_HIST_BASE) / 4;
        if (g_nip_hist[idx] < UINT_MAX) {
            g_nip_hist[idx]++;
        }
    }

    /* Re-apply MBAR SPR if it got cleared by CPU reset. */
    if (s->cpu->env.spr[SPR_MBAR] != 0xF0000000) {
        s->cpu->env.spr[SPR_MBAR] = 0xF0000000;
    }

    /* Debug: dump SPR_MBAR + *(0x908918) + *(0x90851C) once they become non-zero —
     * these are the MBAR-base / SDMA-base BSS variables used by
     * TaskSetup_TASK_FEC_TX (`sth r4, 4636(r8)` at 0x20bcd0 derives the
     * TCR address from these). Per agent investigation 2026-04-28. */
    {
        static uint32_t last_mbar_base, last_sdma_base, last_spr_mbar;
        uint32_t mbar_base = ldl_be_phys(&address_space_memory, 0x908918);
        uint32_t sdma_base = ldl_be_phys(&address_space_memory, 0x90851C);
        uint32_t spr_mbar  = (uint32_t)s->cpu->env.spr[SPR_MBAR];
        if (spr_mbar != last_spr_mbar) {
            fprintf(stderr, "BSP: SPR_MBAR = 0x%08x\n", spr_mbar);
            fflush(stderr);
            last_spr_mbar = spr_mbar;
        }
        if (mbar_base != last_mbar_base) {
            fprintf(stderr,
                    "BSP: *(0x908918) (MBAR base?) = 0x%08x\n", mbar_base);
            fflush(stderr);
            last_mbar_base = mbar_base;
        }
        if (sdma_base != last_sdma_base) {
            fprintf(stderr,
                    "BSP: *(0x90851C) (SDMA base?) = 0x%08x\n", sdma_base);
            fflush(stderr);
            last_sdma_base = sdma_base;
        }
    }

    if (++diag_count < diag_max) {
        timer_mod(s->diag_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000ULL); /* 100us */
    } else {
        unsigned j;
        fprintf(stderr,
                "MPC5200: diag sampler done after %d samples; "
                "summary of unreached stations:\n", diag_count);
        for (i = 0; i < (int)(sizeof(g_boot_stations) / sizeof(g_boot_stations[0]));
             i++) {
            if (!g_boot_stations[i].hit) {
                fprintf(stderr, "  UNREACHED [%2d] 0x%08x : %s\n",
                        i, (unsigned)g_boot_stations[i].addr,
                        g_boot_stations[i].name);
            }
        }
        /* dump top-30 NIP histogram entries by count */
        {
            unsigned top_idx[30] = {0};
            unsigned top_cnt[30] = {0};
            unsigned k;
            for (j = 0; j < NIP_HIST_SIZE; j++) {
                if (g_nip_hist[j] == 0) continue;
                /* insertion sort into top_cnt[] */
                for (k = 0; k < 30; k++) {
                    if (g_nip_hist[j] > top_cnt[k]) {
                        unsigned m;
                        for (m = 29; m > k; m--) {
                            top_cnt[m] = top_cnt[m-1];
                            top_idx[m] = top_idx[m-1];
                        }
                        top_cnt[k] = g_nip_hist[j];
                        top_idx[k] = j;
                        break;
                    }
                }
            }
            fprintf(stderr, "MPC5200: top-30 NIP histogram entries:\n");
            for (k = 0; k < 30 && top_cnt[k] > 0; k++) {
                fprintf(stderr, "  [%2d] 0x%08x : %u samples\n",
                        k, (unsigned)(NIP_HIST_BASE + top_idx[k] * 4),
                        top_cnt[k]);
            }
        }
        fflush(stderr);
    }
}

static void mpc5200_tick(void *opaque)
{
    MPC5200State *s = opaque;
    static int  tick_count = 0;
    static bool ext_armed  = false;
    static bool patches_applied = false;

    if (!patches_applied) {
        mpc5200_apply_keyswitch_patches();
        patches_applied = true;
    }

    if (!ext_armed) {
        uint32_t w = ldl_be_phys(&address_space_memory, 0x508);
        if (w != MPC5200_VEC_GARBAGE_AT_508) {
            ext_armed = true;
            fprintf(stderr,
                    "MPC5200: EXT vector populated (word@0x508=0x%08x), "
                    "arming EXT delivery\n", w);
            fflush(stderr);
        }
    }

    /*
     * Per BSP investigation, the BSP populates per-task config struct
     * pointers in BSS at these absolute addresses once cacheDmaMalloc'd:
     *   0x008CFC00 -> TASK_FEC_TX config struct
     *   0x008CFC04 -> TASK_FEC_RX config struct
     * Polling these from the timer is a simple way to surface the
     * pointers as soon as the BSP writes them, without instrumenting
     * the entire RAM write path.
     */
    /*
     * The pointers stored at BSS[0x008CFC00]/[0x008CFC04] are NOT
     * var-tables — they are MOTbcommlib `bcom_task` heap-handles
     * (32-byte wrappers). Only fields [0]=slot, [6]=initiator,
     * [7]=bd_size are reliably populated at this point. The middle
     * 5 fields look like residual heap data (virt pointers into
     * TLB-handler code from a prior slab user). The real var-table
     * lives in BestComm SRAM, pointed to by tdt[N].var. See
     * BSP_var_table_findings.md (agent investigation 2026-04-28).
     */
    {
        static uint32_t last_tx_handle, last_rx_handle;
        uint32_t tx_handle = ldl_be_phys(&address_space_memory,
                                         BCOM_BSS_TX_TASK_PTR);
        uint32_t rx_handle = ldl_be_phys(&address_space_memory,
                                         BCOM_BSS_RX_TASK_PTR);
        if (tx_handle != last_tx_handle) {
            fprintf(stderr,
                    "BestComm: BSS[0x008CFC00] (FEC TX task handle) = 0x%08x\n",
                    tx_handle);
            if (tx_handle) {
                uint32_t w[8];
                for (int i = 0; i < 8; i++) {
                    w[i] = ldl_be_phys(&address_space_memory,
                                       tx_handle + i * 4);
                }
                fprintf(stderr,
                        "  TX handle dump @0x%08x:\n"
                        "    [0x00]=0x%08x [0x04]=0x%08x [0x08]=0x%08x [0x0C]=0x%08x\n"
                        "    [0x10]=0x%08x [0x14]=0x%08x [0x18]=0x%08x [0x1C]=0x%08x\n",
                        tx_handle,
                        w[0], w[1], w[2], w[3],
                        w[4], w[5], w[6], w[7]);
                /* Deref the SRAM pointers (handle[1..5] and [7]) to see
                 * what var-table values they actually point at. */
                for (int i = 1; i <= 5; i++) {
                    if (w[i] >= 0xF0008000 && w[i] < 0xF000C000) {
                        uint32_t v = ldl_be_phys(&address_space_memory, w[i]);
                        fprintf(stderr,
                                "    *handle[%d](0x%08x) = 0x%08x\n",
                                i, w[i], v);
                    }
                }
                if (w[7] >= 0xF0008000 && w[7] < 0xF000C000) {
                    uint32_t v = ldl_be_phys(&address_space_memory, w[7]);
                    fprintf(stderr,
                            "    *handle[7](0x%08x) = 0x%08x\n", w[7], v);
                }
                /* Also dump the contiguous 8-word region at SRAM
                 * pointed by handle[1] — this is likely the actual
                 * MOTbcommlib var-table for this task. */
                if (w[1] >= 0xF0008000 && w[1] < 0xF000C000) {
                    fprintf(stderr, "    SRAM[%08x..+0x20] var-table:\n      ",
                            w[1]);
                    for (int i = 0; i < 8; i++) {
                        uint32_t v = ldl_be_phys(&address_space_memory,
                                                 w[1] + i * 4);
                        fprintf(stderr, "%08x ", v);
                    }
                    fprintf(stderr, "\n");
                }
                /* TX: var_base = *handle[3] (Linux MOTbcommlib layout:
                 *   +0x00 DRD, +0x04 fifo, +0x08 enable, +0x0C bd_base,
                 *   +0x10 bd_last, +0x14 bd_start, +0x18 buffer_size).
                 * Read bd_base from var-table and dump first 8 BDs. */
                if (w[3] >= 0xF0008000 && w[3] < 0xF000C000) {
                    uint32_t var_base = ldl_be_phys(&address_space_memory,
                                                     w[3]);
                    fprintf(stderr,
                            "    TX var_base=*handle[3]=0x%08x; var-table:\n      ",
                            var_base);
                    for (int i = 0; i < 7; i++) {
                        uint32_t v = ldl_be_phys(&address_space_memory,
                                                 var_base + i * 4);
                        fprintf(stderr, "%08x ", v);
                    }
                    fprintf(stderr, "\n");
                    uint32_t bd_base = ldl_be_phys(&address_space_memory,
                                                    var_base + 0x0C);
                    uint32_t bd_last = ldl_be_phys(&address_space_memory,
                                                    var_base + 0x10);
                    if (bd_base >= 0xF0008000 && bd_base < 0xF000C000 &&
                        bd_last >= bd_base && bd_last < 0xF000C000) {
                        unsigned n = (bd_last - bd_base) / 8 + 1;
                        fprintf(stderr,
                                "    TX BD ring @0x%08x..0x%08x (%u BDs); first 8 BDs:\n      ",
                                bd_base, bd_last, n);
                        for (int i = 0; i < 16; i++) {
                            uint32_t v = ldl_be_phys(
                                &address_space_memory, bd_base + i * 4);
                            fprintf(stderr, "%08x ", v);
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
            fflush(stderr);
            last_tx_handle = tx_handle;
        }
        if (rx_handle != last_rx_handle) {
            fprintf(stderr,
                    "BestComm: BSS[0x008CFC04] (FEC RX task handle) = 0x%08x\n",
                    rx_handle);
            if (rx_handle) {
                uint32_t w[8];
                for (int i = 0; i < 8; i++) {
                    w[i] = ldl_be_phys(&address_space_memory,
                                       rx_handle + i * 4);
                }
                fprintf(stderr,
                        "  RX handle dump @0x%08x:\n"
                        "    [0x00]=0x%08x [0x04]=0x%08x [0x08]=0x%08x [0x0C]=0x%08x\n"
                        "    [0x10]=0x%08x [0x14]=0x%08x [0x18]=0x%08x [0x1C]=0x%08x\n",
                        rx_handle,
                        w[0], w[1], w[2], w[3],
                        w[4], w[5], w[6], w[7]);
                for (int i = 1; i <= 5; i++) {
                    if (w[i] >= 0xF0008000 && w[i] < 0xF000C000) {
                        uint32_t v = ldl_be_phys(&address_space_memory, w[i]);
                        fprintf(stderr,
                                "    *handle[%d](0x%08x) = 0x%08x\n",
                                i, w[i], v);
                    }
                }
                if (w[7] >= 0xF0008000 && w[7] < 0xF000C000) {
                    uint32_t v = ldl_be_phys(&address_space_memory, w[7]);
                    fprintf(stderr,
                            "    *handle[7](0x%08x) = 0x%08x\n", w[7], v);
                }
                /* RX: var_base = *handle[3] (Linux MOTbcommlib RX layout:
                 *   +0x00 enable, +0x04 fifo, +0x08 bd_base, +0x0C bd_last,
                 *   +0x10 bd_start, +0x14 buffer_size). */
                if (w[3] >= 0xF0008000 && w[3] < 0xF000C000) {
                    uint32_t var_base = ldl_be_phys(&address_space_memory,
                                                     w[3]);
                    fprintf(stderr,
                            "    RX var_base=*handle[3]=0x%08x; var-table:\n      ",
                            var_base);
                    for (int i = 0; i < 6; i++) {
                        uint32_t v = ldl_be_phys(&address_space_memory,
                                                 var_base + i * 4);
                        fprintf(stderr, "%08x ", v);
                    }
                    fprintf(stderr, "\n");
                    uint32_t bd_base = ldl_be_phys(&address_space_memory,
                                                    var_base + 0x08);
                    uint32_t bd_last = ldl_be_phys(&address_space_memory,
                                                    var_base + 0x0C);
                    if (bd_base >= 0xF0008000 && bd_base < 0xF000C000 &&
                        bd_last >= bd_base && bd_last < 0xF000C000) {
                        unsigned n = (bd_last - bd_base) / 8 + 1;
                        fprintf(stderr,
                                "    RX BD ring @0x%08x..0x%08x (%u BDs); first 8 BDs:\n      ",
                                bd_base, bd_last, n);
                        for (int i = 0; i < 16; i++) {
                            uint32_t v = ldl_be_phys(
                                &address_space_memory, bd_base + i * 4);
                            fprintf(stderr, "%08x ", v);
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
            fflush(stderr);
            last_rx_handle = rx_handle;
        }
    }

    if (tick_count++ < 3) {
        fprintf(stderr, "MPC5200: tick #%d, ic_pending=1, ext_armed=%d\n",
                tick_count, ext_armed);
        fflush(stderr);
    }

    /*
     * Diagnostic: sample NIP every tick for the first few seconds.
     * Log when NIP is NOT in the idle loop (intUnlock @ 0x0100107c
     * for bootrom, or 0x207fxx for vxworks.out runtime). This
     * captures any code path the boot task touches.
     */
    {
        target_ulong nip = s->cpu->env.nip;
        target_ulong lr  = s->cpu->env.lr;
        bool in_idle =
            (nip >= 0x00207f00 && nip < 0x00208000) ||  /* runtime idle */
            (nip >= 0x01001000 && nip < 0x01001200);    /* bootrom idle */
        if (!in_idle && tick_count < 600) {
            fprintf(stderr, "TRACE: NIP=0x%08x LR=0x%08x\n",
                    (unsigned)nip, (unsigned)lr);
            fflush(stderr);
        }
    }
    s->ic_pending = true;
    if (ext_armed) {
        mpc5200_update_ext(s);
    }

    /*
     * Once-per-second TCB dump: identifies which VxWorks task is
     * currently running and what it's blocked on. Per agent
     * investigation 2026-04-29, taskIdCurrent lives at ramBase+0x9084a8;
     * TCB layout: +0x34 name (char*), +0x3C status (0=READY, 2=PEND,
     * 4=DELAY, 6=PEND+TIMEOUT), +0x5C pSemId, +0x7C saved SP.
     * Status decoding helps tell us whether the BSP is blocked on an
     * IRQ that never fires (PEND on sem) vs a TCP timeout (DELAY on
     * tsleep) vs idle (running tShell).
     */
    /*
     * Once-per-second full task-list dump. Walks the DLL anchored at
     * activeQHead (vxworks.out BSS @ 0x008d94e4) — head pointer is
     * *(0x008d94e4); each node is at TCB+0x20; next-link = *(node+0).
     * Per agent investigation 2026-04-29.
     *
     * For each task we report: name, priority, status, pSemId, saved
     * PC/LR (REG_SET starts at TCB+0x130; PC=+0x1BC, LR=+0x1B4). Status
     * 0x2=PEND, 0x4=DELAY, 0x6=PEND+TIMEOUT. The saved PC tells us
     * exactly which kernel routine each task is blocked in.
     */
    if ((tick_count % 60) == 0 && tick_count <= 60 * 90) {
        AddressSpace *as = &address_space_memory;
        const uint32_t ACTIVEQ_HEAD = 0x008d94e4;
        uint32_t cur = ldl_be_phys(as, ACTIVEQ_HEAD);
        fprintf(stderr, "=== task list t=%ds (activeQHead=0x%08x) ===\n",
                tick_count / 60, cur);
        for (int n = 0; n < 64 && cur && cur != ACTIVEQ_HEAD; n++) {
            uint32_t tcb     = cur - 0x20;
            uint32_t name_pa = ldl_be_phys(as, tcb + 0x34);
            uint32_t status  = ldl_be_phys(as, tcb + 0x3C);
            uint32_t prio    = ldl_be_phys(as, tcb + 0x40);
            uint32_t pSemId  = ldl_be_phys(as, tcb + 0x5C);
            uint32_t errnov  = ldl_be_phys(as, tcb + 0x84);
            uint32_t pc      = ldl_be_phys(as, tcb + 0x130 + 0x8C);
            uint32_t lr      = ldl_be_phys(as, tcb + 0x130 + 0x84);
            uint32_t sp      = ldl_be_phys(as, tcb + 0x130 + 0x04);
            char nm[20] = {0};
            if (name_pa && name_pa < 0x10000000) {
                cpu_physical_memory_read(name_pa, (uint8_t *)nm, 19);
            }
            const char *st = "?";
            switch (status) {
            case 0x0:     st = "READY"; break;
            case 0x2:     st = "PEND"; break;
            case 0x4:     st = "DELAY"; break;
            case 0x6:     st = "PEND+TO"; break;
            case 0x10000: st = "SUSP"; break;
            }
            fprintf(stderr,
                    "  [%2d] tcb=0x%08x %-16s prio=%3u %-8s "
                    "sem=0x%08x errno=0x%08x PC=0x%08x LR=0x%08x SP=0x%08x\n",
                    n, tcb, nm, prio, st, pSemId, errnov, pc, lr, sp);
            cur = ldl_be_phys(as, cur);  /* DLL_NODE.next */
        }
        fflush(stderr);
    }

    timer_mod(s->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667ULL); /* ~60 Hz */
}

/* Log MMIO accesses outside well-known ranges to help trace kernel behavior */
static void mpc5200_log_access(const char *rw, hwaddr offset, uint64_t val, unsigned size)
{
    static uint64_t log_count = 0;
    if (log_count < 2000 && 0) { /* disabled — re-enable for diagnosis */
        fprintf(stderr, "MPC5200 %s off=0x%05x sz=%u val=0x%08x\n",
                rw, (unsigned)offset, size, (unsigned)val);
        log_count++;
        if (log_count == 2000) {
            fprintf(stderr, "MPC5200: log limit reached, suppressing further\n");
        }
        fflush(stderr);
    }
}

static uint64_t mpc5200_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    MPC5200State *s = opaque;

    /*
     * IC: 0x508 — Peripheral Priority and HI/LO Select 2 (per manual).
     * Existing stub returned 0x00000001 when ic_pending and the SLT1
     * dispatch path worked. Preserving for SLT; FEC dispatch goes via
     * 0x524 only.
     */
    if (offset == 0x0508 && s->ic_pending) {
        return 0x00000001;
    }
    /*
     * IC: 0x524 — PerStat/MainStat/CritStat Encoded (manual §7.2.4.9).
     *   bits  2:7 = PSe (Peripheral Status Encoded, 6 bits = flag+5-bit src)
     *   bits 10:15 = MSe (Main Status Encoded, 6 bits = flag+5-bit src)
     *   bits 21:23 = CSe (Critical, 3 bits)
     * For an active FEC interrupt:
     *   PSe = 0x25  (flag=1, peripheral source 5 = Ethernet)
     *   MSe = 0x24  (flag=1, main source 4 = LO_int — peripheral group)
     *   value = (0x25 << 24) | (0x24 << 16) = 0x25240000
     * For SLT1 (legacy stub behaviour) we keep the working value
     * 0x40000000 — this is in a reserved bit per manual but it's what
     * the BSP's EXT handler expects given how it was developed.
     * Read-to-clear: deassert EXT after the handler reads us. The level
     * will re-assert if a source is still active.
     */
    /*
     * SDMA Main IRQ #0 (BestComm task done). Per agent investigation
     * 2026-04-29 of the EXT dispatch decoder at vxworks 0x1178f0:
     *   - Main lane test: r31 & 0x3F000000 (PPC bits 2..7 = valid+5-bit src)
     *   - Source ID extract: (r31 >> 24) & 0x1F → vector 0..31
     * SDMA Main ISR (0x132854) is registered on Main IRQ #0 via
     * intConnect(0, 0x132854, ...) at 0x1329a0. So PerStat must encode
     * source=0 with the valid bit set. The minimum valid encoding is
     * 0x20000000 (just bit 2 PPC; source-ID = 0).
     *
     * We do NOT auto-clear ic_sdma_pending here — the BSP's per-task
     * callback W1C-clears IntPending at 0x1214, and our W1C handler
     * re-evaluates the SDMA IRQ. The IC dispatch decoder reads PerStat
     * each time the EXT vector enters; if the source is still pending,
     * we re-report it.
     */
    if (offset == 0x0524) {
        if (s->ic_fec_pending) {
            return 0x25240000;
        }
        if (s->ic_sdma_pending) {
            return 0x20000000;
        }
        if (s->ic_pending) {
            /* SLT1 timer — legacy path; read-to-clear. */
            s->ic_pending = false;
            mpc5200_update_ext(s);
            return 0x40000000;
        }
        return 0;
    }

    /* BestComm/SDMA register file: byte-addressable RAM. Big-endian. */
    if (offset >= 0x1200 && offset < 0x1300) {
        unsigned i = offset - 0x1200;
        uint64_t v = 0;
        for (unsigned k = 0; k < size && (i + k) < 0x100; k++) {
            v = (v << 8) | s->bestcomm[i + k];
        }
        return v << (8 * (4 - size)); /* MSB-align the result for BE */
    }
    /* MPC5200 internal SRAM (MBAR+0x8000..0xBFFF, 16 KiB per manual §13.13) */
    if (offset >= 0x8000 && offset < 0xC000) {
        unsigned i = offset - 0x8000;
        uint64_t v = 0;
        for (unsigned k = 0; k < size && (i + k) < sizeof(s->sram); k++) {
            v = (v << 8) | s->sram[i + k];
        }
        return v << (8 * (4 - size));
    }
    /*
     * I2C1 (MBAR+0x3D00..0x3D14): no real chip behind it. Provide
     * NACK semantics so any probe sees "no slave" and moves on.
     */
    if (offset >= 0x3d00 && offset < 0x3d20) {
        static unsigned i2c1_r_log = 0;
        if (i2c1_r_log++ < 64) {
            fprintf(stderr, "I2C1 R +0x%02x sz=%u  NIP=0x%08x\n",
                    (unsigned)(offset - 0x3d00), size,
                    (unsigned)s->cpu->env.nip);
            fflush(stderr);
        }
        if (offset == 0x3d0c) {
            return 0x83000000;     /* MCF+MIF+RXAK = transfer done, no ack */
        }
        if (offset == 0x3d10) {
            return 0xff000000;     /* open bus */
        }
        return 0;
    }
    /* I2C2 (0x3d40..0x3d57): X1226 state machine + register read-back */
    if (offset == 0x3d40) { return (uint64_t)s->i2c2.madr   << 24; }
    if (offset == 0x3d44) { return (uint64_t)s->i2c2.mfdr   << 24; }
    if (offset == 0x3d48) { return (uint64_t)s->i2c2.mbcr   << 24; }
    if (offset == 0x3d54) { return (uint64_t)s->i2c2.mdfsrr << 24; }
    if (offset == 0x3d4c) { /* MBSR */
        MPC5200I2CState *i2c = &s->i2c2;
        uint8_t sr = ((i2c->mcf  ? 1 : 0) << 7)
                   | ((i2c->mbb  ? 1 : 0) << 5)
                   | ((i2c->mif  ? 1 : 0) << 1)
                   | ((i2c->rxak ? 1 : 0) << 0);
        i2c->mif = false; /* read clears interrupt flag */
        return (uint64_t)sr << 24;
    }
    if (offset == 0x3d50) { /* MDR — data read */
        MPC5200I2CState *i2c = &s->i2c2;
        uint8_t b = 0xff;
        if (i2c->phase == I2C_DATA && i2c->reading) {
            if (i2c->slave_addr == 0x50 ||
                i2c->slave_addr == 0x57 ||
                i2c->slave_addr == 0x6f) {
                b = i2c->eeprom[i2c->reg_addr & (MPC5200_EEPROM_SIZE - 1)];
                fprintf(stderr, "I2C2 RD slave=0x%02x reg=0x%03x -> 0x%02x\n",
                        i2c->slave_addr, i2c->reg_addr, b);
                fflush(stderr);
                i2c->reg_addr++;
            }
            i2c->mcf = true;
            i2c->mif = true;
            i2c->rxak = false;
        }
        return (uint64_t)b << 24;
    }
    /*
     * PSC1-6: log reads + return plausible "TX always ready" status.
     */
    if (offset >= 0x2000 && offset < 0x2c00) {
        static unsigned psc_r_log = 0;
        if (psc_r_log++ < 200) {
            unsigned psc_idx, reg;
            if (offset < 0x2200)      { psc_idx = 1; reg = offset - 0x2000; }
            else if (offset < 0x2400) { psc_idx = 2; reg = offset - 0x2200; }
            else if (offset < 0x2600) { psc_idx = 3; reg = offset - 0x2400; }
            else if (offset < 0x2800) { psc_idx = 4; reg = offset - 0x2600; }
            else if (offset < 0x2A00) { psc_idx = 5; reg = offset - 0x2800; }
            else                      { psc_idx = 6; reg = offset - 0x2A00; }
            fprintf(stderr, "PSC%u R +0x%02x sz=%u\n", psc_idx, reg, size);
            fflush(stderr);
        }
        return 0x30303030; /* all bytes = TxRDY+TxEMP set */
    }
    mpc5200_log_access("R", offset, 0, size);
    return 0;
}

/*
 * BestComm TX BD-walker (executor for FEC slot 2).
 *
 * Vestas BSP uses the standard Linux MOTbcommlib layout — verified via
 * SRAM-write trace 2026-04-28:
 *   TaskBAR (MBAR+0x1200) = 0xF0008000 (start of internal SRAM)
 *   TDT[2] @ 0xF0008040 (TaskBAR + 2*0x20)
 *   TDT[2].var @ 0xF0008048 = 0xF0008700 (TX var-table base)
 *   TX var-table:
 *     +0x00 DRD ptr
 *     +0x04 fifo  = 0xF00031A4 (FEC TFIFO_DATA)
 *     +0x08 enable = 0xF0001220 (TCR[2])
 *     +0x0C bd_base  (e.g. 0xF0009400)
 *     +0x10 bd_last  (e.g. 0xF00095F8)
 *     +0x14 bd_start
 *     +0x18 buffer_size (1522 = MTU)
 *   BD: u32 status; u32 skb_pa;  (8 bytes BE)
 *     status: BCOM_BD_READY (0x40000000), BCOM_FEC_TX_BD_TFD (0x08000000),
 *             BCOM_FEC_TX_BD_TC (0x04000000), 11-bit length in low bits.
 */
static void mpc5200_bestcomm_walk_tx(MPC5200State *s)
{
    uint32_t taskbar = ((uint32_t)s->bestcomm[0] << 24)
                     | ((uint32_t)s->bestcomm[1] << 16)
                     | ((uint32_t)s->bestcomm[2] <<  8)
                     |  (uint32_t)s->bestcomm[3];
    if (taskbar < 0xF0008000 || taskbar >= 0xF000C000) {
        fprintf(stderr,
                "BestComm TX: TaskBAR=0x%08x not in SRAM range\n", taskbar);
        fflush(stderr);
        return;
    }

    uint32_t var = ldl_be_phys(&address_space_memory, taskbar + 0x40 + 0x08);
    if (var < 0xF0008000 || var >= 0xF000C000) {
        fprintf(stderr,
                "BestComm TX: TDT[2].var=0x%08x out of SRAM\n", var);
        fflush(stderr);
        return;
    }

    uint32_t bd_base  = ldl_be_phys(&address_space_memory,
                                    var + BCOM_FEC_TX_VAR_BD_BASE);
    uint32_t bd_last  = ldl_be_phys(&address_space_memory,
                                    var + BCOM_FEC_TX_VAR_BD_LAST);
    uint32_t bd_start = ldl_be_phys(&address_space_memory,
                                    var + BCOM_FEC_TX_VAR_BD_START);

    fprintf(stderr,
            "BestComm TX: TaskBAR=0x%08x var=0x%08x bd_base=0x%08x "
            "bd_last=0x%08x bd_start=0x%08x\n",
            taskbar, var, bd_base, bd_last, bd_start);
    fflush(stderr);

    if (!bd_base || !bd_last || bd_start < bd_base || bd_start > bd_last) {
        fprintf(stderr, "BestComm TX: BD ring fields look bogus, abort walk\n");
        fflush(stderr);
        return;
    }

    /*
     * Defensive cap: stop after ringsize iterations even if we keep
     * finding READY BDs, in case our READY-clear write doesn't take
     * (e.g. if dst is read-only because BSP zeroed BSS).
     */
    unsigned ringsize = (bd_last - bd_base) / BCOM_FEC_BD_STRIDE + 1;
    unsigned cursor   = (bd_start - bd_base) / BCOM_FEC_BD_STRIDE;
    unsigned walked   = 0;
    unsigned sent     = 0;

    /*
     * For multi-fragment frames the BSP may chain BDs, signalling the
     * last fragment with BCOM_FEC_TX_BD_TFD. We accumulate fragments
     * into a frame buffer and emit on TFD.
     */
    uint8_t frame[2048];
    unsigned frame_len = 0;

    while (walked++ < ringsize) {
        uint32_t bd_addr = bd_base + cursor * BCOM_FEC_BD_STRIDE;
        uint32_t status  = ldl_be_phys(&address_space_memory, bd_addr);
        uint32_t skb_pa  = ldl_be_phys(&address_space_memory, bd_addr + 4);

        if (!(status & BCOM_BD_READY)) {
            break;
        }

        unsigned len = status & 0x000007FF;  /* 11-bit length field */

        fprintf(stderr,
                "BestComm TX:   BD[%u] @0x%08x status=0x%08x skb_pa=0x%08x len=%u%s\n",
                cursor, bd_addr, status, skb_pa, len,
                (status & BCOM_FEC_TX_BD_TFD) ? " TFD" : "");
        fflush(stderr);

        if (len && skb_pa && (frame_len + len) <= sizeof(frame)) {
            cpu_physical_memory_read(skb_pa, frame + frame_len, len);
            frame_len += len;
        }

        if (status & BCOM_FEC_TX_BD_TFD) {
            if (frame_len >= 14 && s->fec) {
                fprintf(stderr,
                        "BestComm TX:   sending frame, len=%u\n", frame_len);
                fflush(stderr);
                mpc5200_fec_send_packet(s->fec, frame, frame_len);
                sent++;
            }
            frame_len = 0;
        }

        /* Clear READY (CPU re-owns this BD). Preserve other status bits. */
        stl_be_phys(&address_space_memory, bd_addr, status & ~BCOM_BD_READY);

        if (bd_addr == bd_last) {
            cursor = 0;
        } else {
            cursor++;
        }
    }

    /* Update bd_start cursor in var-table so next walk picks up where we
     * left off. */
    {
        uint32_t new_start = bd_base + cursor * BCOM_FEC_BD_STRIDE;
        if (new_start > bd_last) {
            new_start = bd_base;
        }
        stl_be_phys(&address_space_memory,
                    var + BCOM_FEC_TX_VAR_BD_START, new_start);
    }

    if (sent && s->fec) {
        mpc5200_fec_raise_eir(s->fec, MPC5200_FEC_EIR_TXF);

        /*
         * BSP's SDMA Main ISR (vxworks 0x132854, registered on Main IRQ
         * #0 by intConnect at 0x1329a0) reads MBAR+0x1214 IntPending,
         * ANDs against ~MBAR+0x1218 IntMask, and dispatches per-task
         * callbacks. For FEC TX (task 2) we set bit 2 (LSB) and let the
         * SDMA-IRQ evaluator decide whether to assert EXT (it checks
         * IntMask first). Without this the BSP never sees TX completion
         * and stalls.
         */
        uint32_t intp = mpc5200_bc_get32(s, 0x14);
        mpc5200_bc_put32(s, 0x14, intp | BCOM_INTP_FEC_TX);
        mpc5200_sdma_eval_irq(s);
    }

    fprintf(stderr, "BestComm TX: walk done, frames sent=%u\n", sent);
    fflush(stderr);
}

/* Pointer to the live MPC5200State for the static RX hook. */
static MPC5200State *g_mpc5200_for_rx;

/*
 * BestComm RX hook (executor for FEC slot 3).
 *
 * Called from the FEC's .receive callback when the host network stack
 * delivers an inbound Ethernet frame. We walk the RX BD ring (in SRAM
 * via TDT[3].var), find a BD with READY=1, copy the frame to its
 * skb_pa in DRAM, mark BCOM_FEC_RX_BD_L + length, clear READY, advance
 * cursor, fire EIR.RXF.
 *
 * RX var-table layout (Linux MOTbcommlib bcom_fec_rx_var):
 *   +0x00 enable, +0x04 fifo, +0x08 bd_base, +0x0C bd_last,
 *   +0x10 bd_start, +0x14 buffer_size
 */
static void mpc5200_bestcomm_rx_hook(const uint8_t *buf, size_t len)
{
    MPC5200State *s = g_mpc5200_for_rx;
    if (!s || len < 14 || len > 2048) {
        return;
    }

    uint32_t taskbar = ((uint32_t)s->bestcomm[0] << 24)
                     | ((uint32_t)s->bestcomm[1] << 16)
                     | ((uint32_t)s->bestcomm[2] <<  8)
                     |  (uint32_t)s->bestcomm[3];
    if (taskbar < 0xF0008000 || taskbar >= 0xF000C000) {
        return; /* TaskBAR not set yet — drop */
    }

    /* TDT[3].var = taskbar + 3*0x20 + 0x08 */
    uint32_t var = ldl_be_phys(&address_space_memory, taskbar + 0x60 + 0x08);
    if (var < 0xF0008000 || var >= 0xF000C000) {
        return;
    }

    uint32_t bd_base  = ldl_be_phys(&address_space_memory,
                                    var + BCOM_FEC_RX_VAR_BD_BASE);
    uint32_t bd_last  = ldl_be_phys(&address_space_memory,
                                    var + BCOM_FEC_RX_VAR_BD_LAST);
    uint32_t bd_start = ldl_be_phys(&address_space_memory,
                                    var + BCOM_FEC_RX_VAR_BD_START);

    if (!bd_base || !bd_last || bd_start < bd_base || bd_start > bd_last) {
        return;
    }

    /* Look for the next READY BD starting at bd_start. */
    uint32_t bd_addr = bd_start;
    uint32_t status  = ldl_be_phys(&address_space_memory, bd_addr);
    uint32_t skb_pa  = ldl_be_phys(&address_space_memory, bd_addr + 4);

    if (!(status & BCOM_BD_READY)) {
        fprintf(stderr,
                "BestComm RX: BD[start=0x%08x] not READY, dropping %zu B\n",
                bd_addr, len);
        fflush(stderr);
        return;
    }

    fprintf(stderr,
            "BestComm RX: TaskBAR=0x%08x var=0x%08x bd=0x%08x skb_pa=0x%08x len=%zu\n",
            taskbar, var, bd_addr, skb_pa, len);
    fflush(stderr);

    /* Copy the frame into the BD's skb_pa buffer in DRAM. */
    cpu_physical_memory_write(skb_pa, buf, len);

    /* Status: length in low 11 bits + BCOM_FEC_RX_BD_L (last in frame). */
    uint32_t new_status = (uint32_t)(len & 0x7FF) | BCOM_FEC_RX_BD_L;
    stl_be_phys(&address_space_memory, bd_addr, new_status);

    /* Advance bd_start cursor in var-table. */
    uint32_t next = (bd_addr == bd_last) ? bd_base
                                         : bd_addr + BCOM_FEC_BD_STRIDE;
    stl_be_phys(&address_space_memory,
                var + BCOM_FEC_RX_VAR_BD_START, next);

    if (s->fec) {
        mpc5200_fec_raise_eir(s->fec, MPC5200_FEC_EIR_RXF);
    }

    /*
     * Set SDMA IntPending bit 3 (FEC RX = task 3) and re-evaluate.
     * BSP unmasks bit 3 in IntMask when the RX task is enabled; this
     * fires the SDMA Main ISR which dispatches to the FEC RX callback.
     */
    uint32_t intp = mpc5200_bc_get32(s, 0x14);
    mpc5200_bc_put32(s, 0x14, intp | BCOM_INTP_FEC_RX);
    mpc5200_sdma_eval_irq(s);
}

/* Forward decl for the FEC -> BestComm RX hook installer. */
void mpc5200_fec_set_rx_hook(void (*hook)(const uint8_t *, size_t));

static void mpc5200_mmio_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    MPC5200State *s = opaque;

    /*
     * IC register write: ack legacy SLT path. We deliberately do NOT
     * clear ic_fec_pending or ic_sdma_pending here — those are level-
     * sensitive sources that clear when their underlying IRQ register
     * (FEC EIR / SDMA IntPending) is acked by the BSP. Spurious clears
     * would silently drop FEC TX/RX completion IRQs.
     */
    if (offset >= 0x0500 && offset <= 0x052c) {
        s->ic_pending = false;
        mpc5200_update_ext(s);
        return;
    }
    /* BestComm/SDMA register file: store as bytes (BE), match access size. */
    if (offset >= 0x1200 && offset < 0x1300) {
        unsigned i = offset - 0x1200;
        uint64_t v = value;
        if (size < 4) {
            v &= ((uint64_t)1 << (8 * size)) - 1;
        }
        /*
         * IntPending (MBAR+0x1214) is W1C — writing 1 to a bit clears it.
         * The BSP's per-task ACK helper (vxworks 0x12e888) does
         * `stw (1<<taskID), MBAR+0x1214` to acknowledge a task IRQ.
         */
        if (offset == 0x1214 && size == 4) {
            uint32_t cur = mpc5200_bc_get32(s, 0x14);
            uint32_t bits = (uint32_t)v;
            mpc5200_bc_put32(s, 0x14, cur & ~bits);
            mpc5200_sdma_eval_irq(s);
            return;
        }
        for (int k = (int)size - 1; k >= 0 && (i + k) < 0x100; k--) {
            s->bestcomm[i + k] = v & 0xff;
            v >>= 8;
        }
        /*
         * IntMask (MBAR+0x1218) writes can change which bits are
         * unmasked-pending — re-evaluate the SDMA IRQ line.
         */
        if (offset >= 0x1218 && offset < 0x121C) {
            mpc5200_sdma_eval_irq(s);
        }
        /* Diagnostic: log all BestComm config writes (first ~64) with caller NIP. */
        {
            static unsigned bc_log = 0;
            if (bc_log++ < 64) {
                target_ulong nip = s->cpu->env.nip;
                target_ulong lr  = s->cpu->env.lr;
                fprintf(stderr,
                        "BestComm W +0x%03x sz=%u val=0x%08x  NIP=0x%08x LR=0x%08x\n",
                        (unsigned)offset, size, (unsigned)value,
                        (unsigned)nip, (unsigned)lr);
                fflush(stderr);
            }
        }
        if (offset == 0x1200 && size == 4) {
            /* TaskBAR write — points at TDT in SRAM. Hardware default
             * 0xFC003000 (chip-internal-SRAM offset); BSP may rewrite
             * to point inside MBAR+0x8000 SRAM after init. */
            fprintf(stderr,
                    "*** BestComm TaskBAR WRITE: 0x%08x  NIP=0x%08x LR=0x%08x ***\n",
                    (unsigned)value, (unsigned)s->cpu->env.nip,
                    (unsigned)s->cpu->env.lr);
            fflush(stderr);
        }
        if (offset >= 0x121C && offset < 0x123C && size <= 2) {
            unsigned slot = (offset - 0x121C) / 2;
            uint16_t tcr = (uint16_t)(value & 0xFFFF);
            fprintf(stderr,
                    "*** BestComm TCR[%u] WRITE: val=0x%04x %s ***\n",
                    slot, tcr, (tcr & 0xC0) ? "(ENABLE)" : "");
            fflush(stderr);
            /* Trigger the BD-ring walker for the FEC TX (slot 2)
             * task whenever a non-zero value is written. The Vestas
             * BSP enables the task with `0x00C2` (AutoStart|HighEn|
             * AS=2) — kicking on any non-zero write covers both the
             * Linux-style EN bit and the BSP's AutoStart/AS pattern. */
            if (slot == 2 && tcr != 0) {
                mpc5200_bestcomm_walk_tx(s);
            }
        }
        return;
    }
    /* MPC5200 internal SRAM (MBAR+0x8000..0xBFFF, 16 KiB per manual §13.13) */
    if (offset >= 0x8000 && offset < 0xC000) {
        unsigned i = offset - 0x8000;
        uint64_t v = value;
        if (size < 4) {
            v &= ((uint64_t)1 << (8 * size)) - 1;
        }
        for (int k = (int)size - 1; k >= 0 && (i + k) < (int)sizeof(s->sram); k--) {
            s->sram[i + k] = v & 0xff;
            v >>= 8;
        }
        /* Diagnostic: skip the early bzero pass (NIP 0x002053b8 area
         * doing reverse bzero of all SRAM) so we see the actually-
         * interesting TDT/var-table writes that come after. */
        {
            static unsigned sram_log = 0;
            target_ulong nip = s->cpu->env.nip;
            bool is_bzero = (nip >= 0x002053b0 && nip <= 0x002053c4);
            if (!is_bzero && sram_log++ < 600) {
                target_ulong lr = s->cpu->env.lr;
                fprintf(stderr,
                        "SRAM W +0x%03x sz=%u val=0x%08x  NIP=0x%08x LR=0x%08x\n",
                        (unsigned)(offset - 0x8000), size, (unsigned)value,
                        (unsigned)nip, (unsigned)lr);
                fflush(stderr);
            }
        }
        return;
    }
    /* I2C2 control / data writes (X1226 state machine) */
    if (offset >= 0x3d40 && offset < 0x3d58) {
        MPC5200I2CState *i2c = &s->i2c2;
        /*
         * MBCR is byte-accessed via 32-bit BE writes from VxWorks; bits we
         * care about live in the high byte: MEN(7) MIEN(6) MSTA(5,START)
         * MTX(4) TXAK(3) RSTA(2). Both 32-bit writes (value MSB) and 1-byte
         * writes (value low byte) need handling.
         */
        uint8_t b = (size == 1) ? (value & 0xff) : ((value >> 24) & 0xff);
        switch (offset) {
        case 0x3d48: { /* MBCR */
            i2c->mbcr = b;                   /* store for read-back */
            bool start = (b >> 5) & 1;       /* MSTA */
            bool restart = (b >> 2) & 1;     /* RSTA */
            if (start && (!i2c->mbb || restart)) {
                /* START or repeated-START: next MDR write is slave-addr byte */
                fprintf(stderr, "I2C2 START%s\n", restart ? " (RSTA)" : "");
                fflush(stderr);
                i2c->phase = I2C_ADDR;
                i2c->mbb = true;
                i2c->mcf = false;
                i2c->mif = false;
                i2c->rxak = false;
            } else if (!start && i2c->mbb) {
                /* STOP */
                fprintf(stderr, "I2C2 STOP\n"); fflush(stderr);
                i2c->phase = I2C_IDLE;
                i2c->mbb = false;
                i2c->mcf = true;
            }
            return;
        }
        case 0x3d50: { /* MDR — master TX data byte */
            switch (i2c->phase) {
            case I2C_ADDR:
                i2c->slave_addr = (b >> 1) & 0x7f;
                i2c->reading = b & 1;
                fprintf(stderr, "I2C2 ADDR slave=0x%02x %s\n",
                        i2c->slave_addr, i2c->reading ? "RD" : "WR");
                fflush(stderr);
                /*
                 * 0x50 = AT24Cxx-style board-id EEPROM (1-byte internal addr).
                 * 0x57/0x6f = X1226 EEPROM/RTC (2-byte internal addr).
                 * Everything else NACKs.
                 */
                if (i2c->slave_addr == 0x50) {
                    i2c->rxak = false;
                    if (i2c->reading) {
                        i2c->phase = I2C_DATA;     /* repeated-start, addr already set */
                    } else {
                        i2c->phase = I2C_REG_LO;   /* 1-byte internal address */
                    }
                } else if (i2c->slave_addr == 0x57 || i2c->slave_addr == 0x6f) {
                    i2c->rxak = false;
                    if (i2c->reading) {
                        i2c->phase = I2C_DATA;
                    } else {
                        i2c->phase = I2C_REG_HI;   /* 2-byte internal address */
                    }
                } else {
                    i2c->rxak = true; /* NACK */
                    i2c->phase = I2C_IDLE;
                }
                break;
            case I2C_REG_HI:
                i2c->reg_addr = (uint16_t)b << 8;
                i2c->phase = I2C_REG_LO;
                i2c->rxak = false;
                break;
            case I2C_REG_LO:
                i2c->reg_addr |= b;
                i2c->phase = I2C_DATA;
                i2c->rxak = false;
                break;
            case I2C_DATA:
                if (!i2c->reading &&
                    (i2c->slave_addr == 0x57 || i2c->slave_addr == 0x6f)) {
                    i2c->eeprom[i2c->reg_addr & (MPC5200_EEPROM_SIZE - 1)] = b;
                    i2c->reg_addr++;
                }
                i2c->rxak = false;
                break;
            default:
                i2c->rxak = true;
                break;
            }
            i2c->mcf = true;
            i2c->mif = true;
            return;
        }
        case 0x3d40: i2c->madr   = b; return;
        case 0x3d44: i2c->mfdr   = b; return;
        case 0x3d54: i2c->mdfsrr = b; return;
        default:
            return;
        }
    }
    /* PSC1-6 at MBAR+0x2000..0x2C00 */
    if (offset >= 0x2000 && offset < 0x2c00) {
        unsigned psc_idx;
        unsigned reg;
        /* PSC1=0x2000, PSC2=0x2200, PSC3=0x2400, PSC4=0x2600, PSC5=0x2800, PSC6=0x2C00 */
        if (offset < 0x2200)      { psc_idx = 1; reg = offset - 0x2000; }
        else if (offset < 0x2400) { psc_idx = 2; reg = offset - 0x2200; }
        else if (offset < 0x2600) { psc_idx = 3; reg = offset - 0x2400; }
        else if (offset < 0x2800) { psc_idx = 4; reg = offset - 0x2600; }
        else if (offset < 0x2A00) { psc_idx = 5; reg = offset - 0x2800; }
        else                      { psc_idx = 6; reg = offset - 0x2A00; }

        /*
         * Console-output routing: any write to a TX data path on PSC1
         * (the standard VxWorks console). Both legacy non-FIFO TX
         * buffer (offset 0x0c) and FIFO-mode TX data (offset 0x40)
         * are checked. Bytes are dumped as ASCII to stderr — this
         * gives us BSP log messages.
         */
        if (psc_idx == 1 && (reg == 0x0c || reg == 0x40 || reg == 0x10)) {
            uint8_t ch = value & 0xff;
            fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) || ch == '\n' || ch == '\r'
                                   ? ch : '.');
            fflush(stderr);
            return;
        }
        /* Diagnostic: log first 200 PSC writes outside the TX path. */
        {
            static unsigned psc_log = 0;
            if (psc_log++ < 200) {
                fprintf(stderr, "PSC%u W +0x%02x sz=%u val=0x%08x\n",
                        psc_idx, reg, size, (unsigned)value);
                fflush(stderr);
            }
        }
        return;
    }
    mpc5200_log_access("W", offset, value, size);
}

static const MemoryRegionOps mpc5200_mmio_ops = {
    .read  = mpc5200_mmio_read,
    .write = mpc5200_mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

#define MAX_IDE_BUS 2
#define CFG_ADDR 0xf0000510
#define TBFREQ (25UL * 1000UL * 1000UL)
#define CLOCKFREQ (900UL * 1000UL * 1000UL)
#define BUSFREQ (100UL * 1000UL * 1000UL)

#define NDRV_VGA_FILENAME "qemu_vga.ndrv"

#define PROM_FILENAME "openbios-ppc"
#define PROM_BASE 0xfff00000
#define PROM_SIZE (1 * MiB)

#define KERNEL_LOAD_ADDR 0x01000000
#define KERNEL_GAP       0x00100000

#define TYPE_CORE99_MACHINE MACHINE_TYPE_NAME("mac99")
typedef struct Core99MachineState Core99MachineState;
DECLARE_INSTANCE_CHECKER(Core99MachineState, CORE99_MACHINE,
                         TYPE_CORE99_MACHINE)

typedef enum {
    CORE99_VIA_CONFIG_CUDA = 0,
    CORE99_VIA_CONFIG_PMU,
    CORE99_VIA_CONFIG_PMU_ADB
} Core99ViaConfig;

struct Core99MachineState {
    /*< private >*/
    MachineState parent;

    Core99ViaConfig via_config;
};

static void fw_cfg_boot_set(void *opaque, const char *boot_device,
                            Error **errp)
{
    fw_cfg_modify_i16(opaque, FW_CFG_BOOT_DEVICE, boot_device[0]);
}

static uint64_t translate_kernel_address(void *opaque, uint64_t addr)
{
    return (addr & 0x0fffffff) + KERNEL_LOAD_ADDR;
}

static void ppc_core99_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /* 970 CPUs want to get their initial IP as part of their boot protocol */
    cpu->env.nip = PROM_BASE + 0x100;
}

/* PowerPC Mac99 hardware initialisation */
static void ppc_core99_init(MachineState *machine)
{
    Core99MachineState *core99_machine = CORE99_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    PowerPCCPU *cpu = NULL;
    CPUPPCState *env = NULL;
    char *filename;
    IrqLines *openpic_irqs;
    int i, j, k, ppc_boot_device, machine_arch, bios_size = -1;
    const char *bios_name = machine->firmware ?: PROM_FILENAME;
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    MPC5200State *mpc5200 = g_new0(MPC5200State, 1);
    hwaddr kernel_base = 0, initrd_base = 0, cmdline_base = 0;
    long kernel_size = 0, initrd_size = 0;
    PCIBus *pci_bus;
    bool has_pmu, has_adb;
    Object *macio;
    MACIOIDEState *macio_ide;
    BusState *adb_bus;
    MacIONVRAMState *nvr;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    void *fw_cfg;
    SysBusDevice *s;
    DeviceState *dev, *pic_dev, *uninorth_pci_dev;
    DeviceState *uninorth_internal_dev = NULL, *uninorth_agp_dev = NULL;
    hwaddr nvram_addr = 0xFFF04000;
    uint64_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq() : TBFREQ;

    /* init CPUs */
    for (i = 0; i < machine->smp.cpus; i++) {
        cpu = POWERPC_CPU(cpu_create(machine->cpu_type));
        env = &cpu->env;

        /* Set time-base frequency to 100 Mhz */
        cpu_ppc_tb_init(env, TBFREQ);
        qemu_register_reset(ppc_core99_reset, cpu);
    }

    /* allocate RAM */
    if (machine->ram_size > 2 * GiB) {
        error_report("RAM size more than 2 GiB is not supported");
        exit(1);
    }
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /* allocate and load firmware ROM */
    memory_region_init_rom(bios, NULL, "ppc_core99.bios", PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), PROM_BASE, bios);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        /* Load OpenBIOS (ELF) */
        bios_size = load_elf(filename, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);

        if (bios_size <= 0) {
            /* or load binary ROM image */
            bios_size = load_image_targphys(filename, PROM_BASE, PROM_SIZE);
        }
        g_free(filename);
    }
    if (bios_size < 0 || bios_size > PROM_SIZE) {
        error_report("could not load PowerPC bios '%s'", bios_name);
        exit(1);
    }

    if (machine->kernel_filename) {
        kernel_base = KERNEL_LOAD_ADDR;
        kernel_size = load_elf(machine->kernel_filename, NULL,
                               translate_kernel_address, NULL, NULL, NULL,
                               NULL, NULL, ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);
        if (kernel_size < 0) {
            kernel_size = load_aout(machine->kernel_filename, kernel_base,
                                    machine->ram_size - kernel_base,
                                    true, TARGET_PAGE_SIZE);
        }
        if (kernel_size < 0) {
            kernel_size = load_image_targphys(machine->kernel_filename,
                                              kernel_base,
                                              machine->ram_size - kernel_base);
        }
        if (kernel_size < 0) {
            error_report("could not load kernel '%s'",
                         machine->kernel_filename);
            exit(1);
        }
        /* load initrd */
        if (machine->initrd_filename) {
            initrd_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
            initrd_size = load_image_targphys(machine->initrd_filename,
                                              initrd_base,
                                              machine->ram_size - initrd_base);
            if (initrd_size < 0) {
                error_report("could not load initial ram disk '%s'",
                             machine->initrd_filename);
                exit(1);
            }
            cmdline_base = TARGET_PAGE_ALIGN(initrd_base + initrd_size);
        } else {
            cmdline_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
        }
        ppc_boot_device = 'm';
    } else {
        ppc_boot_device = '\0';
        /* We consider that NewWorld PowerMac never have any floppy drive
         * For now, OHW cannot boot from the network.
         */
        for (i = 0; machine->boot_config.order[i] != '\0'; i++) {
            if (machine->boot_config.order[i] >= 'c' &&
                machine->boot_config.order[i] <= 'f') {
                ppc_boot_device = machine->boot_config.order[i];
                break;
            }
        }
        if (ppc_boot_device == '\0') {
            error_report("No valid boot device for Mac99 machine");
            exit(1);
        }
    }

    openpic_irqs = g_new0(IrqLines, machine->smp.cpus);
    dev = DEVICE(cpu);
    for (i = 0; i < machine->smp.cpus; i++) {
        /* Mac99 IRQ connection between OpenPIC outputs pins
         * and PowerPC input pins
         */
        switch (PPC_INPUT(env)) {
        case PPC_FLAGS_INPUT_6xx:
            openpic_irqs[i].irq[OPENPIC_OUTPUT_INT] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_CINT] =
                 qdev_get_gpio_in(dev, PPC6xx_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_MCK] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_MCP);
            /* Not connected ? */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_DEBUG] = NULL;
            /* Check this */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_RESET] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_HRESET);
            break;
#if defined(TARGET_PPC64)
        case PPC_FLAGS_INPUT_970:
            openpic_irqs[i].irq[OPENPIC_OUTPUT_INT] =
                qdev_get_gpio_in(dev, PPC970_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_CINT] =
                qdev_get_gpio_in(dev, PPC970_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_MCK] =
                qdev_get_gpio_in(dev, PPC970_INPUT_MCP);
            /* Not connected ? */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_DEBUG] = NULL;
            /* Check this */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_RESET] =
                qdev_get_gpio_in(dev, PPC970_INPUT_HRESET);
            break;
#endif /* defined(TARGET_PPC64) */
        default:
            error_report("Bus model not supported on mac99 machine");
            exit(1);
        }
    }

    /* UniN init */
    s = SYS_BUS_DEVICE(qdev_new(TYPE_UNI_NORTH));
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xf8000000,
                                sysbus_mmio_get_region(s, 0));

    if (PPC_INPUT(env) == PPC_FLAGS_INPUT_970) {
        machine_arch = ARCH_MAC99_U3;
        /* 970 gets a U3 bus */
        /* Uninorth AGP bus */
        uninorth_pci_dev = qdev_new(TYPE_U3_AGP_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_pci_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf0800000);
        sysbus_mmio_map(s, 1, 0xf0c00000);
        /* PCI hole */
        memory_region_add_subregion(get_system_memory(), 0x80000000,
                                    sysbus_mmio_get_region(s, 2));
        /* Register 8 MB of ISA IO space */
        memory_region_add_subregion(get_system_memory(), 0xf2000000,
                                    sysbus_mmio_get_region(s, 3));
    } else {
        machine_arch = ARCH_MAC99;
        /* Use values found on a real PowerMac */
        /* Uninorth AGP bus */
        uninorth_agp_dev = qdev_new(TYPE_UNI_NORTH_AGP_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_agp_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf0800000);
        sysbus_mmio_map(s, 1, 0xf0c00000);

        /* Uninorth internal bus */
        uninorth_internal_dev = qdev_new(
                                TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_internal_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf4800000);
        sysbus_mmio_map(s, 1, 0xf4c00000);

        /* Uninorth main bus - this must be last to make it the default */
        uninorth_pci_dev = qdev_new(TYPE_UNI_NORTH_PCI_HOST_BRIDGE);
        qdev_prop_set_uint32(uninorth_pci_dev, "ofw-addr", 0xf2000000);
        s = SYS_BUS_DEVICE(uninorth_pci_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf2800000);
        sysbus_mmio_map(s, 1, 0xf2c00000);
        /* PCI hole */
        memory_region_add_subregion(get_system_memory(), 0x80000000,
                                    sysbus_mmio_get_region(s, 2));
        /* Register 8 MB of ISA IO space */
        memory_region_add_subregion(get_system_memory(), 0xf2000000,
                                    sysbus_mmio_get_region(s, 3));
    }

    machine->usb |= defaults_enabled() && !machine->usb_disabled;
    has_pmu = (core99_machine->via_config != CORE99_VIA_CONFIG_CUDA);
    has_adb = (core99_machine->via_config == CORE99_VIA_CONFIG_CUDA ||
               core99_machine->via_config == CORE99_VIA_CONFIG_PMU_ADB);

    /* init basic PC hardware */
    pci_bus = PCI_HOST_BRIDGE(uninorth_pci_dev)->bus;

    /* MacIO */
    macio = OBJECT(pci_new(-1, TYPE_NEWWORLD_MACIO));
    dev = DEVICE(macio);
    qdev_prop_set_uint64(dev, "frequency", tbfreq);
    qdev_prop_set_bit(dev, "has-pmu", has_pmu);
    qdev_prop_set_bit(dev, "has-adb", has_adb);

    dev = DEVICE(object_resolve_path_component(macio, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));

    pci_realize_and_unref(PCI_DEVICE(macio), pci_bus, &error_fatal);

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    for (i = 0; i < 4; i++) {
        qdev_connect_gpio_out(uninorth_pci_dev, i,
                              qdev_get_gpio_in(pic_dev, 0x1b + i));
    }

    /* TODO: additional PCI buses only wired up for 32-bit machines */
    if (PPC_INPUT(env) != PPC_FLAGS_INPUT_970) {
        /* Uninorth AGP bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_agp_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }

        /* Uninorth internal bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_internal_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }
    }

    /* OpenPIC */
    s = SYS_BUS_DEVICE(pic_dev);
    k = 0;
    for (i = 0; i < machine->smp.cpus; i++) {
        for (j = 0; j < OPENPIC_OUTPUT_NB; j++) {
            sysbus_connect_irq(s, k++, openpic_irqs[i].irq[j]);
        }
    }
    g_free(openpic_irqs);

    /* We only emulate 2 out of 3 IDE controllers for now */
    ide_drive_get(hd, ARRAY_SIZE(hd));

    macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[0]"));
    macio_ide_init_drives(macio_ide, hd);

    macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[1]"));
    macio_ide_init_drives(macio_ide, &hd[MAX_IDE_DEVS]);

    if (has_adb) {
        if (has_pmu) {
            dev = DEVICE(object_resolve_path_component(macio, "pmu"));
        } else {
            dev = DEVICE(object_resolve_path_component(macio, "cuda"));
        }

        adb_bus = qdev_get_child_bus(dev, "adb.0");
        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);

        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    }

    if (machine->usb) {
        pci_create_simple(pci_bus, -1, "pci-ohci");

        /* U3 needs to use USB for input because Linux doesn't support via-cuda
        on PPC64 */
        if (!has_adb || machine_arch == ARCH_MAC99_U3) {
            USBBus *usb_bus;

            usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                              &error_abort));
            usb_create_simple(usb_bus, "usb-kbd");
            usb_create_simple(usb_bus, "usb-mouse");
        }
    }

    pci_vga_init(pci_bus);

    if (graphic_depth != 15 && graphic_depth != 32 && graphic_depth != 8) {
        graphic_depth = 15;
    }

    pci_init_nic_devices(pci_bus, mc->default_nic);

    /* The NewWorld NVRAM is not located in the MacIO device */
    if (kvm_enabled() && qemu_real_host_page_size() > 4096) {
        /* We can't combine read-write and read-only in a single page, so
           move the NVRAM out of ROM again for KVM */
        nvram_addr = 0xFFE00000;
    }
    dev = qdev_new(TYPE_MACIO_NVRAM);
    qdev_prop_set_uint32(dev, "size", MACIO_NVRAM_SIZE);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, nvram_addr);
    nvr = MACIO_NVRAM(dev);
    pmac_format_nvram_partition(nvr, MACIO_NVRAM_SIZE);
    /* No PCI init: the BIOS will do it */

    dev = qdev_new(TYPE_FW_CFG_MEM);
    fw_cfg = FW_CFG(dev);
    qdev_prop_set_uint32(dev, "data_width", 1);
    qdev_prop_set_bit(dev, "dma_enabled", false);
    object_property_add_child(OBJECT(machine), TYPE_FW_CFG, OBJECT(fw_cfg));
    s = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(s, &error_fatal);
    sysbus_mmio_map(s, 0, CFG_ADDR);
    sysbus_mmio_map(s, 1, CFG_ADDR + 2);

    /* MPC5200 MBAR region: custom stub for IC, I2C2 (X1226), PSC, EEPROM */
    mpc5200->cpu = POWERPC_CPU(first_cpu);

    /*
     * MPC5200 internal-peripheral base register (SPR 638 / MBAR) hardware
     * reset value is 0xF0000000. QEMU's generic G2 init resets it to 0,
     * which makes the BSP's vxMBarGet() return 0; downstream code (notably
     * TaskSetup_TASK_FEC_TX) computes TCR addresses as MBAR + slot*2 +
     * 0x121C and writes them to physical RAM at 0x1220 instead of MBAR.
     * Force the proper reset value here. Also re-applied later in the
     * SLT tick handler in case CPU reset clobbers it.
     */
    mpc5200->cpu->env.spr[SPR_MBAR] = 0xF0000000;

    mpc5200_i2c2_init(&mpc5200->i2c2);

    /*
     * BestComm TaskBar Pointer (MBAR+0x1200) hardware reset value is
     * 0xFC003000 — points at SRAM where SDMA task descriptors live.
     * Per BSP investigation (agent post-PHY-init trace):
     * `m5200FecEndLoad` reads MBAR+0x1200 early; if it sees 0 it
     * silently exits before reaching `m5200FecSdmaTaskInit`. Initialise
     * the bestcomm[] backing buffer with the reset value at offset 0
     * so the BSP's first read returns sane data.
     */
    mpc5200->bestcomm[0x00] = 0xFC;
    mpc5200->bestcomm[0x01] = 0x00;
    mpc5200->bestcomm[0x02] = 0x30;
    mpc5200->bestcomm[0x03] = 0x00;
    mpc5200->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mpc5200_tick, mpc5200);
    mpc5200->diag_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, mpc5200_diag_sample, mpc5200);
    /* delay first tick by 1 s of guest time to let BSP run early init */
    {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        fprintf(stderr, "MPC5200: init timer, now=%"PRId64"\n", now);
        fflush(stderr);
        timer_mod(mpc5200->timer, now + 1000000000LL);
        /* Diagnostic sampler runs from boot — no startup delay so we
         * catch usrRoot in flight before it blocks. */
        timer_mod(mpc5200->diag_timer, now + 1000ULL);
    }
    memory_region_init_io(&mpc5200->mr, NULL, &mpc5200_mmio_ops, mpc5200,
                          "mpc5200-mmio", 1 * MiB);
    memory_region_add_subregion(get_system_memory(), 0xf0000000, &mpc5200->mr);

    /*
     * MPC5200 Fast Ethernet Controller (FEC) at MBAR+0x3000.
     * Overlaps the broader MMIO stub with higher priority so its
     * 0x3000..0x33FF range is handled here instead of the fall-through
     * stub. IRQ output drives the existing EXT path via ic_pending.
     */
    {
        DeviceState *fec = qdev_new("mpc5200-fec");
        qemu_configure_nic_device(fec, true, "mpc5200-fec");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(fec), &error_fatal);
        memory_region_add_subregion_overlap(
            get_system_memory(),
            0xf0000000 + 0x3000,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(fec), 0),
            1);
        sysbus_connect_irq(SYS_BUS_DEVICE(fec), 0,
                           qemu_allocate_irq(mpc5200_fec_irq_handler,
                                             mpc5200, 0));
        mpc5200->fec = fec;
        /* Install RX hook so incoming Ethernet frames are delivered to
         * our BestComm RX walker. */
        g_mpc5200_for_rx = mpc5200;
        mpc5200_fec_set_rx_hook(mpc5200_bestcomm_rx_hook);
    }

    /* MPC5200 internal SRAM at 0x601f8000 (64 KiB) */
    memory_region_init_ram(sram, NULL, "mpc5200-sram", 64 * KiB, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x601f8000, sram);

    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, (uint16_t)machine->smp.cpus);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, (uint16_t)machine->smp.max_cpus);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)machine->ram_size);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MACHINE_ID, machine_arch);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, kernel_base);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    if (machine->kernel_cmdline) {
        fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_CMDLINE, cmdline_base);
        pstrcpy_targphys("cmdline", cmdline_base, TARGET_PAGE_SIZE,
                         machine->kernel_cmdline);
    } else {
        fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_CMDLINE, 0);
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_base);
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
    fw_cfg_add_i16(fw_cfg, FW_CFG_BOOT_DEVICE, ppc_boot_device);

    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_WIDTH, graphic_width);
    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_HEIGHT, graphic_height);
    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_DEPTH, graphic_depth);

    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_VIACONFIG, core99_machine->via_config);

    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_IS_KVM, kvm_enabled());
    if (kvm_enabled()) {
        uint8_t *hypercall;

        hypercall = g_malloc(16);
        kvmppc_get_hypercall(env, hypercall, 16);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_PPC_KVM_HC, hypercall, 16);
        fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_KVM_PID, getpid());
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_TBFREQ, tbfreq);
    /* Mac OS X requires a "known good" clock-frequency value; pass it one. */
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_CLOCKFREQ, CLOCKFREQ);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_BUSFREQ, BUSFREQ);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_NVRAM_ADDR, nvram_addr);

    /* MacOS NDRV VGA driver */
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, NDRV_VGA_FILENAME);
    if (filename) {
        gchar *ndrv_file;
        gsize ndrv_size;

        if (g_file_get_contents(filename, &ndrv_file, &ndrv_size, NULL)) {
            fw_cfg_add_file(fw_cfg, "ndrv/qemu_vga.ndrv", ndrv_file, ndrv_size);
        }
        g_free(filename);
    }

    qemu_register_boot_set(fw_cfg_boot_set, fw_cfg);
}

/*
 * Implementation of an interface to adjust firmware path
 * for the bootindex property handling.
 */
static char *core99_fw_dev_path(FWPathProvider *p, BusState *bus,
                                DeviceState *dev)
{
    PCIDevice *pci;
    MACIOIDEState *macio_ide;

    if (!strcmp(object_get_typename(OBJECT(dev)), "macio-newworld")) {
        pci = PCI_DEVICE(dev);
        return g_strdup_printf("mac-io@%x", PCI_SLOT(pci->devfn));
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "macio-ide")) {
        macio_ide = MACIO_IDE(dev);
        return g_strdup_printf("ata-3@%x", macio_ide->addr);
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "ide-hd")) {
        return g_strdup("disk");
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "ide-cd")) {
        return g_strdup("cdrom");
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "virtio-blk-device")) {
        return g_strdup("disk");
    }

    return NULL;
}
static int core99_kvm_type(MachineState *machine, const char *arg)
{
    /* Always force PR KVM */
    return 2;
}

static void core99_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    FWPathProviderClass *fwc = FW_PATH_PROVIDER_CLASS(oc);

    mc->desc = "Mac99 based PowerMac";
    mc->init = ppc_core99_init;
    mc->block_default_type = IF_IDE;
    /* SMP is not supported currently */
    mc->max_cpus = 1;
    mc->default_boot_order = "cd";
    mc->default_display = "std";
    mc->default_nic = "sungem";
    mc->kvm_type = core99_kvm_type;
#ifdef TARGET_PPC64
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
#else
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("7400_v2.9");
#endif
    mc->default_ram_id = "ppc_core99.ram";
    mc->ignore_boot_device_suffixes = true;
    fwc->get_dev_path = core99_fw_dev_path;
}

static char *core99_get_via_config(Object *obj, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    switch (cms->via_config) {
    default:
    case CORE99_VIA_CONFIG_CUDA:
        return g_strdup("cuda");

    case CORE99_VIA_CONFIG_PMU:
        return g_strdup("pmu");

    case CORE99_VIA_CONFIG_PMU_ADB:
        return g_strdup("pmu-adb");
    }
}

static void core99_set_via_config(Object *obj, const char *value, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    if (!strcmp(value, "cuda")) {
        cms->via_config = CORE99_VIA_CONFIG_CUDA;
    } else if (!strcmp(value, "pmu")) {
        cms->via_config = CORE99_VIA_CONFIG_PMU;
    } else if (!strcmp(value, "pmu-adb")) {
        cms->via_config = CORE99_VIA_CONFIG_PMU_ADB;
    } else {
        error_setg(errp, "Invalid via value");
        error_append_hint(errp, "Valid values are cuda, pmu, pmu-adb.\n");
    }
}

static void core99_instance_init(Object *obj)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    /* Default via_config is CORE99_VIA_CONFIG_CUDA */
    cms->via_config = CORE99_VIA_CONFIG_CUDA;
    object_property_add_str(obj, "via", core99_get_via_config,
                            core99_set_via_config);
    object_property_set_description(obj, "via",
                                    "Set VIA configuration. "
                                    "Valid values are cuda, pmu and pmu-adb");

    return;
}

static const TypeInfo core99_machine_info = {
    .name          = MACHINE_TYPE_NAME("mac99"),
    .parent        = TYPE_MACHINE,
    .class_init    = core99_machine_class_init,
    .instance_init = core99_instance_init,
    .instance_size = sizeof(Core99MachineState),
    .interfaces = (InterfaceInfo[]) {
        { TYPE_FW_PATH_PROVIDER },
        { }
    },
};

static void mac_machine_register_types(void)
{
    type_register_static(&core99_machine_info);
}

type_init(mac_machine_register_types)
