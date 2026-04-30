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
 * FS-hook hypercall MMIO doorbell at MBAR+0x4000 (Phase 4b, 2026-05-02).
 *
 * The BSP's open/read/close wrappers (vxworks 0x2b13c8 / 0x2b18fc / 0x2b17d4)
 * are patched into 9-instruction stubs that write the args into our doorbell
 * and trigger a host-syscall dispatcher here. Real `/fs/<tail>` traffic is
 * proxied to a host directory; everything else is rejected.
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define FSHOOK_OFFSET        0x4000   /* MBAR+0x4000..0x403F doorbell */
#define FSHOOK_REG_SIZE      0x40

/* Doorbell layout: cmd@0, arg0@4, arg1@8, arg2@C, result@10, errno@14. */
#define FSHOOK_REG_CMD       0x00
#define FSHOOK_REG_ARG0      0x04
#define FSHOOK_REG_ARG1      0x08
#define FSHOOK_REG_ARG2      0x0C
#define FSHOOK_REG_RESULT    0x10
#define FSHOOK_REG_ERRNO     0x14
/* Diagnostic: write a guest path-pointer here to log "TRACE: <path>" without
 * triggering any host syscall. Used to verify that an instrumented BSP site
 * is actually being executed (e.g. the 0x13ffe4 existence-check stub). */
#define FSHOOK_REG_TRACE     0x18
/* Host->guest doorbell: QEMU writes a sem_id to wake; BSP reads it in the
 * sysClkInt tail patch and calls semGive(sem_id), then writes 0 back to
 * clear. Bypasses the MSR.EE=0 EXT-dispatch wall (2026-05-05 finding) by
 * letting the BSP's own ~60 Hz clock ISR do the wake. */
#define FSHOOK_REG_SEM_QUEUE 0x1C
/* netJobAdd-call doorbell (plan 2026-05-11 step 3): QEMU sets NETJOB_FUNC
 * last (acts as doorbell — non-zero = active). The sysClkInt tail-patch
 * picks it up next tick, loads args from NETJOB_ARG[1..5], and tail-calls
 * netJobAdd at runtime VA 0x0022c288. After netJobAdd returns, control
 * flows back to sysClkInt's continuation via the saved LR.
 * NETJOB_PROBE is host-readable: a probe stub at 0x002acfc0 stores
 * 0xCAFEBABE here when invoked, so QEMU can confirm netTask actually
 * deferred-called the function. */
#define FSHOOK_REG_NETJOB_FUNC      0x20
#define FSHOOK_REG_NETJOB_ARG1      0x24
#define FSHOOK_REG_NETJOB_ARG2      0x28
#define FSHOOK_REG_NETJOB_ARG3      0x2C
#define FSHOOK_REG_NETJOB_ARG4      0x30
#define FSHOOK_REG_NETJOB_ARG5      0x34
#define FSHOOK_REG_NETJOB_PROBE     0x38

/* Command IDs — match the BSP-side stubs in mpc5200_apply_keyswitch_patches. */
#define FS_OPEN   1
#define FS_READ   2
#define FS_CLOSE  3
#define FS_LSEEK  4
#define FS_FSTAT  5
#define FS_EXISTS 6

#define FSHOOK_NUM_FDS       16
#define FSHOOK_FAKE_FD_BASE  1000  /* fake fds returned to BSP: 1000..1015 */

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
    /*
     * FS-hook hypercall state (Phase 4b). Doorbell registers + a small
     * proxy table mapping fake VxWorks fds (1000+i) back to host fds.
     */
    struct {
        uint32_t arg0, arg1, arg2;
        uint32_t result;
        uint32_t err;
        uint32_t sem_queue;          /* host->guest doorbell: sem_id to give */
        uint32_t netjob_func;        /* host->guest doorbell: netJobAdd func ptr */
        uint32_t netjob_args[5];     /* args 1..5 for netJobAdd-deferred call */
        uint32_t netjob_probe;       /* guest writes 0xCAFEBABE on probe-stub fire */
        struct {
            int  host_fd;
            bool in_use;
        } fd_proxy[FSHOOK_NUM_FDS];
    } fshook;
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
    /*
     * 2026-05-05 dispatch experiment: drop SLT1 (s->ic_pending) from
     * the OR. Hypothesis (per agent 1 review): SLT1 fires every 1s
     * and holds ic_pending=true between BSP reads of 0x524, keeping
     * the EXT line at 1 continuously. When SDMA RAISES,
     * `pending_interrupts` already has EXT bit set, so
     * `ppc_set_irq(EXT, 1)` is a no-op (edge-tracked at line 60 of
     * hw/ppc/ppc.c) — `ppc_maybe_interrupt` is never re-invoked,
     * CPU never re-takes the exception. sysClkInt is on the DEC
     * vector (per BSP_static_a1_vxworks.md and runtime station
     * 0x117fd0), not EXT — so removing SLT1 from EXT doesn't break
     * the clock. The legacy 0x524 SLT1 dispatch path is left in
     * place as a fallback.
     */
    int lvl = (s->ic_fec_pending || s->ic_sdma_pending) ? 1 : 0;
    static int prev = -1;
    static unsigned ext_log = 0;
    /* Log first 32, then every 100th, then any with sdma=1. */
    bool log_this = (ext_log < 32) ||
                    (s->ic_sdma_pending && lvl == 1) ||
                    (ext_log % 100 == 0);
    if (lvl != prev && log_this && ext_log < 200) {
        fprintf(stderr,
                "EXT line %d->%d (slt=%d fec=%d sdma=%d) #%u\n",
                prev, lvl, s->ic_pending, s->ic_fec_pending,
                s->ic_sdma_pending, ext_log);
        fflush(stderr);
    }
    if (lvl != prev) {
        ext_log++;
        prev = lvl;
    }
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
    static unsigned log = 0;
    bool transitioned = (s->ic_sdma_pending != was_pending);
    /* Always log transitions; rate-limit no-change to first 64. */
    if (transitioned || log++ < 64) {
        uint32_t pi  = (uint32_t)s->cpu->env.pending_interrupts;
        uint32_t msr = (uint32_t)s->cpu->env.msr;
        fprintf(stderr,
                "SDMA eval: IntPending=0x%08x IntMask=0x%08x unmasked=0x%08x "
                "%s  [pi.EXT=%d MSR.EE=%d slt=%d fec=%d sdma=%d nip=0x%08x]\n",
                intp, mask, intp & ~mask,
                transitioned
                    ? (s->ic_sdma_pending ? "RAISE" : "CLEAR")
                    : "(no change)",
                (pi & PPC_INTERRUPT_EXT) ? 1 : 0,
                (msr & (1u << 15)) ? 1 : 0,    /* MSR_EE bit (15 PPC) */
                s->ic_pending, s->ic_fec_pending, s->ic_sdma_pending,
                (unsigned)s->cpu->env.nip);
        fflush(stderr);
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
/* Forward decl: fshook_root() body lives near the MMIO read/write callbacks. */
static const char *fshook_root(void);

static void mpc5200_apply_keyswitch_patches(void)
{
    /* m5200FecStart: beq cr7, 0x12d7e8 → nop */
    static const uint8_t nop[4] = {0x60, 0x00, 0x00, 0x00};
    cpu_physical_memory_write(0x0012d390, nop, 4);
    /* m5200FecRestart: bne- cr7, 0x12aea0 → nop (per agent) */
    cpu_physical_memory_write(0x0012ae60, nop, 4);

    /*
     * Vestas-app spawn gate. At NIP 0x001009d8 the BSP function 0x00100920
     * reads *(0x00962e2c); if non-zero, all downstream taskSpawns
     * (tApMain, tFirecrest, tFiredrake, tNeon, ...) are skipped. Phase 2a
     * naive force-zero: clobber the BSS slot once at first SLT tick, then
     * Phase 1a's per-100us watcher will tell us whether BSP code rewrites
     * it. If it does, we'll escalate to nop'ing 0x001009e0 (Phase 2b).
     */
    stl_be_phys(&address_space_memory, 0x00962e2c, 0);

    /*
     * Filesystem-gate bypass (plan 2026-05-01).
     *
     * The BSP boots cleanly, then in usrAppInit-equivalent code it calls
     * a file-existence helper at 0x0013ffe4 with arg "/fs/etc/startup.app"
     * to decide whether the runmode startup script exists. Helper body:
     *
     *   13ffe4: stwu  r1,-16(r1)
     *   13ffe8: lis   r4, 62               ; r4 = 0x3DBAC0 = "r" (mode)
     *   13ffec: mflr  r0
     *   13fff0: addi  r4, r4, -17728
     *   13fff4: stw   r0, 20(r1)
     *   13fff8: bl    0x2a5508             ; fopen(path, "r") -> r3 = FILE*
     *   13fffc: cmpwi cr7, r3, 0
     *   140000: bt    eq, 0x14001c          ; null -> return 0
     *   140004: bl    0x2a4cac              ; fclose(FILE*)
     *   140008: li    r3, 1                 ; success
     *   ... epilogue, return r3
     *
     * Because we have no /fs/ filesystem mounted (no MPC5200 ATA model),
     * the real fopen returns NULL and the BSP loops on
     *   "Cannot find startup.app in runmode !"
     * for 8 retries before giving up to bootmode idle.
     *
     * Stub the helper to always return 1. This makes the BSP take the
     * success path that prints "Executing startup script /fs/etc/startup.app"
     * and calls the script-runner at 0x001061c0. The runner will still
     * fail (real open() also has no FS), but flipping this single check
     * proves the gate-7 mechanism end-to-end and unblocks Phase 4b
     * (full host-fs hook subsystem with open/read/close proxy).
     *
     *   13ffe4: 38 60 00 01    li r3, 1
     *   13ffe8: 4e 80 00 20    blr
     *
     * The function never set up its stack frame yet, so a bare blr is safe.
     */
    /* fs_exists hypercall (plan 2026-05-08, Track 2). Replaced the
     * earlier "always return 1" trace stub with a real FS_EXISTS
     * hypercall: the BSP now gets an honest -1/+1 from access(F_OK).
     * Body at 0x13ffe4 is 9 instructions (36 bytes) — same size as
     * our open/read/close hypercall stubs — installed below in the
     * shared template block alongside FS_OPEN/READ/CLOSE. */

    /*
     * Phase 4b (2026-05-02): hypercall stubs at the BSP's open/read/close
     * entry points. Each replaces the native VxWorks wrapper with 9 PPC
     * instructions (36 bytes) that write args to the FS-hook MMIO doorbell
     * at 0xF0004000 and read the result back into r3.
     *
     *   3D 80 F0 00   lis  r12, 0xF000
     *   61 8C 40 00   ori  r12, r12, 0x4000      ; r12 = 0xF0004000
     *   90 6C 00 04   stw  r3, 4(r12)              ; arg0
     *   90 8C 00 08   stw  r4, 8(r12)              ; arg1
     *   90 AC 00 0C   stw  r5, 12(r12)             ; arg2
     *   38 00 00 0X   li   r0, <FS_*>
     *   90 0C 00 00   stw  r0, 0(r12)              ; cmd: trigger handler
     *   80 6C 00 10   lwz  r3, 16(r12)             ; result -> r3
     *   4E 80 00 20   blr
     *
     * r0/r12 are PPC ABI volatile (caller-saved) — safe scratch. LR is
     * unchanged so blr returns to the original caller.
     *
     * Entry points (verified Phase A from monodis + symtab @ file 0x7f60xx):
     *   open  = 0x002b13c8  (wraps 0x2b13ec -> iosOpen 0x2b2d9c)
     *   read  = 0x002b18fc  (wraps iosRead  0x2b2f00)
     *   close = 0x002b17d4  (wraps iosClose 0x2b2df0)
     */
    {
        uint8_t stub[36] = {
            0x3d, 0x80, 0xf0, 0x00,   /* lis  r12, 0xF000              */
            0x61, 0x8c, 0x40, 0x00,   /* ori  r12, r12, 0x4000         */
            0x90, 0x6c, 0x00, 0x04,   /* stw  r3, 4(r12)               */
            0x90, 0x8c, 0x00, 0x08,   /* stw  r4, 8(r12)               */
            0x90, 0xac, 0x00, 0x0c,   /* stw  r5, 12(r12)              */
            0x38, 0x00, 0x00, 0x00,   /* li   r0, cmd  (patched)       */
            0x90, 0x0c, 0x00, 0x00,   /* stw  r0, 0(r12)  -> trigger   */
            0x80, 0x6c, 0x00, 0x10,   /* lwz  r3, 16(r12)              */
            0x4e, 0x80, 0x00, 0x20,   /* blr                            */
        };
        stub[23] = FS_OPEN;
        cpu_physical_memory_write(0x002b13c8, stub, sizeof(stub));
        stub[23] = FS_READ;
        cpu_physical_memory_write(0x002b18fc, stub, sizeof(stub));
        stub[23] = FS_CLOSE;
        cpu_physical_memory_write(0x002b17d4, stub, sizeof(stub));
        /* fs_exists at 0x13ffe4 — same template, FS_EXISTS cmd. The
         * BSP-level body uses fopen("/fs/<path>", "r") to test for
         * existence; our hypercall delegates to host access(F_OK)
         * via fshook_handle_exists. */
        stub[23] = FS_EXISTS;
        cpu_physical_memory_write(0x0013ffe4, stub, sizeof(stub));
    }

    /*
     * printf no-op (plan 2026-05-03, Phase C).
     *
     * Phase A (stack-walk on tRootTask) showed usrToolsInit @ 0x107a4c
     * → 0x1076b0 → 0x106594 (banner printer) blocking on the third call
     * to printf at 0x2acee8. The deep call chain into iosWrite ends at
     * a kernel sem block (PC=0x2fe918) that's only posted by a PSC1
     * TX-empty IRQ — and our PSC1 emulation never raises one (no IRQ
     * wiring). Two earlier printfs went through (probably small enough
     * to fit a buffer); the third blocks waiting for drain.
     *
     * The fully correct fix is to wire PSC1 IRQs to fire on TX writes;
     * scope creep for this session. Pragmatic alternative: turn printf
     * itself into a no-op so the BSP's printf-heavy code can complete.
     * We lose log output, but PSC1 W +0x0c|+0x40 echo path stays for
     * any direct-to-UART writes (sysSerialHwInit etc.).
     *
     *   2acee8: 38 60 00 00    li  r3, 0
     *   2acee8: 4e 80 00 20    blr
     *
     * Function never set up its frame yet, so a bare li/blr is safe.
     */
    static const uint8_t printf_stub[8] = {
        0x38, 0x60, 0x00, 0x00,   /* li  r3, 0                      */
        0x4e, 0x80, 0x00, 0x20,   /* blr                            */
    };
    cpu_physical_memory_write(0x002acee8, printf_stub, sizeof(printf_stub));

    /*
     * sysClkInt tail-patch (semGive shim, plan 2026-05-06).
     *
     * 2026-05-05 confirmed MSR.EE=0 holds across the SDMA-pending window —
     * EXT dispatch never reaches the SDMA Main ISR, so tFecEndRx never
     * wakes via the normal IRQ path. Sidestep: have the BSP's own clock
     * ISR call semGive on a doorbell flag we set from the QEMU IO thread
     * after the RX walker delivers a frame.
     *
     * Patch site: replace the `bl 0x00138a7c` (intUnlock) at sysClkInt
     * 0x001180b8 with a `bl` to a stub we install in the freed body of
     * the no-op'd printf at 0x002acef0. The stub does the original
     * intUnlock (so EE=1 again), then reads FSHOOK_REG_SEM_QUEUE; if
     * non-zero, clears the flag and tail-calls semGive(sem_id).
     *
     * Public semGive @ 0x002ff5c4 — verified: byte-identical prologue to
     * bootrom semGive, 686 direct callers in vxworks.out, name string at
     * 0x003e67b0 referenced from runtime symtab record at 0x008fe080.
     *
     * Update 2026-05-07 (gate 9 follow-up): swap tail-call from semGive
     * @ 0x002ff5c4 to semFlush @ 0x002ff884. semFlush still goes through
     * the workQ deferred path that never drains in our run — wake never
     * reaches the scheduler.
     *
     * Update 2026-04-29 (plan-pivot follow-up): swap tail-call from semFlush
     * to a direct qPriBMapPut call on the readyQ at 0x0099AF58. The previous
     * windPendQGet @ 0x00178550 attempt didn't fire — qGet on sem.pendQ
     * came back without affecting state (sem.qHead unchanged at vt=10s
     * post-shim), suggesting the runtime windPendQGet checks something
     * we haven't fully traced. Side-stepping by emulating its tail-end
     * directly: clear TCB.status (PEND -> READY) + clear TCB.pSemId, then
     * call qPriBMapPut(readyQ, TCB, priority).
     *
     * qPriBMapPut @ 0x002cd8e0 — runtime address found by sig-matching
     * bootrom symbol qPriBMapPut (0x010b533c). Identical instruction stream.
     * It does:
     *   - if priority is lower than current head, become new head
     *   - bMapAtomicSet(bitmap, priority)
     *   - qListPut(bucket[priority], node)
     *
     * readyQ struct @ 0x0099AF58 — runtime BSS address, found by tracing
     * the wake-tail of windPendQGet at 0x00178550 (`lis 3, 154; addi 3, 3,
     * -20648; bctr` with vtable[0x10] = qPriBMapPut).
     *
     * tFecEndRx TCB @ 0x07BEDE38, priority 29.
     *
     * intUnlock @ 0x00138a7c — verified: mfmsr; rlwinm clear EE; mtmsr;
     * isync; blr (matches bootrom 0x010fXXXX intUnlock primitive).
     *
     * Stub layout (21 instructions, 84 bytes, at 0x002acef0):
     *
     *   +0x00  mflr  r12              ; save sysClkInt continuation
     *   +0x04  bl    0x00138a7c       ; intUnlock (EE=1 again)
     *   +0x08  lis   r10, 0xF000      ; r10 = SEM_QUEUE doorbell addr
     *   +0x0C  ori   r10, r10, 0x401C
     *   +0x10  lwz   r3,  0(r10)      ; r3 = pending sem_id (used as flag)
     *   +0x14  mtlr  r12              ; pre-set LR for tail-call/blr
     *   +0x18  cmpwi r3, 0
     *   +0x1C  beq+  +0x34            ; skip if no sem (->+0x50 blr)
     *   +0x20  li    r9, 0
     *   +0x24  stw   r9,  0(r10)      ; clear doorbell BEFORE call
     *   +0x28  lis   r9, 0x07BF       ; r9 = TCB high
     *   +0x2C  addi  r9, r9, -0x21C8  ; r9 = 0x07BEDE38 (tFecEndRx TCB)
     *   +0x30  li    r0, 0
     *   +0x34  stw   r0, 0x3C(r9)     ; TCB.status = 0 (clear PEND)
     *   +0x38  stw   r0, 0x5C(r9)     ; TCB.pSemId = 0
     *   +0x3C  lis   r3, 0x009A       ; r3 = readyQ high
     *   +0x40  addi  r3, r3, -0x50A8  ; r3 = 0x0099AF58 (readyQ struct)
     *   +0x44  mr    r4, r9            ; r4 = TCB (= node)
     *   +0x48  li    r5, 29            ; r5 = priority
     *   +0x4C  b     0x002cd8e0       ; tail-call qPriBMapPut (no link)
     *   +0x50  blr                    ; reached via beq when no sem
     *
     * r3, r4, r5, r9, r10, r12 are PPC ABI volatile (caller-saved).
     * sysClkInt's code after the patch site reads only r26, r28, r30
     * (non-volatile) + reloads its scratch regs, so our clobbers are safe.
     * r3 is overwritten at 0x001180f8 (`li r3, 0xf0`) so the saved-MSR
     * returned by intUnlock is unused — same as the original `bl 0x138a7c`.
     */
    {
        const uint32_t stub_addr     = 0x002acef0;
        const uint32_t intunlock_addr = 0x00138a7c;
        const uint32_t qpribmap_addr = 0x002cd8e0; /* qPriBMapPut — readyQ enqueue */
        const uint32_t netjobadd_addr = 0x0022c288; /* netJobAdd — runtime sig-match (plan 2026-05-11) */
        const uint32_t patch_site    = 0x001180b8;

        /* Encode bl/b: insn = 0x48000000 | (offset & 0x03FFFFFC) | LK.
         *
         * Plan 2026-04-30 (FEC+FTP boot): per-sem dispatch in sem-queue
         * path. Drops work-flag write (4 insns) and trailing blr (1 insn,
         * replaced by `beqlr`). Adds 4-insn dispatch + tRoot setup +
         * common-path tail. Net offset changes from prior 35-insn layout:
         *   - netJobAdd tail-call stays at +0x34
         *   - qPriBMapPut tail-call moves from +0x84 to +0x88 (last insn).
         */
        uint32_t bl_intunlock =
            0x48000000u | ((intunlock_addr  - (stub_addr + 0x04)) & 0x03FFFFFC) | 1u;
        uint32_t b_netjobadd =
            0x48000000u | ((netjobadd_addr  - (stub_addr + 0x34)) & 0x03FFFFFC);
        uint32_t b_qpribmap =
            0x48000000u | ((qpribmap_addr   - (stub_addr + 0x88)) & 0x03FFFFFC);
        uint32_t bl_to_stub =
            0x48000000u | ((stub_addr       - patch_site)         & 0x03FFFFFC) | 1u;

        /*
         * Extended sysClkInt tail-patch shim (35 instructions, 140 bytes).
         * Two doorbells, each one-shot per tick:
         *   1. NETJOB_FUNC (0xF0004020) — if non-zero, tail-call netJobAdd
         *      with args from NETJOB_ARG[1..3].
         *   2. SEM_QUEUE   (0xF000401C) — wake one of two PEND'd tasks based
         *      on bit 9 of the sem ID:
         *        sem & 0x200 == 0  -> wake tFecEndRx (TCB 0x07BEDE38, prio 29)
         *        sem & 0x200 != 0  -> wake tRootTask (TCB 0x07FEFE00, prio 0)
         *      Both wake paths flip TCB+0x3C=0, clear pSemId, tail-call
         *      qPriBMapPut on readyQ 0x0099AF58.
         *
         * Plan 2026-04-30 (FEC+FTP single-node boot): per-sem dispatch
         * fallback (Plan risk #1). The dual-slot extension didn't fit:
         * shim already ends at 0x002acf7c with only 4 B clearance to the
         * probe stub at 0x002acf80. Per-sem dispatch in the existing slot
         * is the bounded alternative — same instruction count.
         *
         * To make room for the dispatch (4 insns), this version drops:
         *   - work-flag write *(0x07BEDD04)=8 (4 insns) — per Plan "What
         *     we are explicitly NOT doing": tFecEndRx body progression no
         *     longer relevant; usrToolsInit's FEC respawn supersedes.
         *   - trailing blr at +0x88 (1 insn) — replaced by `beqlr` at the
         *     cmpwi for empty-doorbell case.
         *
         * Both paths preserve sysClkInt's continuation in r12 and use mtlr
         * before tail-call. Each doorbell is cleared by the shim BEFORE the
         * tail-call so the shim doesn't re-fire on the same arg.
         *
         * Free-space check: shim end = stub_addr + 0x8C = 0x002acf7c. The
         * probe stub starts at 0x002acf80 — 4 B clearance preserved.
         */
        const uint32_t stub_words[35] = {
            0x7d8802a6,    /* +0x00  mflr  r12                              */
            bl_intunlock,  /* +0x04  bl    0x00138a7c (intUnlock)            */
            0x3d40f000,    /* +0x08  lis   r10, 0xF000                      */
            0x614a4000,    /* +0x0C  ori   r10, r10, 0x4000  (FSHOOK base)  */
            0x806a0020,    /* +0x10  lwz   r3,  0x20(r10)  (netjob_func)    */
            0x2c030000,    /* +0x14  cmpwi r3, 0                            */
            0x41820020,    /* +0x18  beq   +0x20  (-> +0x38 sem-queue path) */
            0x808a0024,    /* +0x1C  lwz   r4,  0x24(r10)  (arg1)           */
            0x80aa0028,    /* +0x20  lwz   r5,  0x28(r10)  (arg2)           */
            0x80ca002c,    /* +0x24  lwz   r6,  0x2c(r10)  (arg3)           */
            0x38000000,    /* +0x28  li    r0, 0                            */
            0x900a0020,    /* +0x2C  stw   r0,  0x20(r10) (clr netjob)      */
            0x7d8803a6,    /* +0x30  mtlr  r12                               */
            b_netjobadd,   /* +0x34  b     netJobAdd (tail-call)             */
            0x806a001c,    /* +0x38  lwz   r3,  0x1c(r10) (sem_queue)       */
            0x7d8803a6,    /* +0x3C  mtlr  r12                               */
            0x2c030000,    /* +0x40  cmpwi r3, 0                            */
            0x4d820020,    /* +0x44  beqlr (no doorbell - return via mtlr'd LR)*/
            0x39200000,    /* +0x48  li    r9, 0                            */
            0x912a001c,    /* +0x4C  stw   r9,  0x1c(r10) (clr sem_queue)   */
            /* Per-sem dispatch on bit 9 of the sem ID: 0x07bee080 (tFec)
             * has bit 9 = 0, 0x07bee378 (tRoot) has bit 9 = 1.            */
            0x70690200,    /* +0x50  andi. r9, r3, 0x0200 (sets CR0)        */
            0x38000000,    /* +0x54  li    r0, 0                            */
            0x41820014,    /* +0x58  beq   +0x14  (-> +0x6C tFec setup)     */
            /* tRoot setup: TCB 0x07FEFE00, prio 0 — fall through.          */
            0x3c8007ff,    /* +0x5C  lis   r4, 0x07FF                       */
            0x3884fe00,    /* +0x60  addi  r4, r4, -0x0200 (= 0x07FEFE00)   */
            0x38a00000,    /* +0x64  li    r5, 0                            */
            0x48000010,    /* +0x68  b     +0x10  (-> +0x78 common)         */
            /* tFec setup: TCB 0x07BEDE38, prio 29.                         */
            0x3c8007bf,    /* +0x6C  lis   r4, 0x07BF                       */
            0x3884de38,    /* +0x70  addi  r4, r4, -0x21C8 (= 0x07BEDE38)   */
            0x38a0001d,    /* +0x74  li    r5, 29                           */
            /* Common path: clear TCB.status, TCB.pSemId; enqueue on readyQ.*/
            0x9004003c,    /* +0x78  stw   r0, 0x3C(r4) (TCB.status = 0)   */
            0x9004005c,    /* +0x7C  stw   r0, 0x5C(r4) (TCB.pSemId = 0)   */
            0x3c60009a,    /* +0x80  lis   r3, 0x009A                       */
            0x3863af58,    /* +0x84  addi  r3, r3, -0x50A8 (= 0x0099AF58)   */
            b_qpribmap,    /* +0x88  b     0x002cd8e0 (tail-call qPriBMapPut)*/
        };
        /* Convert to BE byte array for cpu_physical_memory_write. */
        uint8_t stub_bytes[sizeof(stub_words)];
        for (unsigned i = 0; i < ARRAY_SIZE(stub_words); i++) {
            stub_bytes[i*4 + 0] = (stub_words[i] >> 24) & 0xFF;
            stub_bytes[i*4 + 1] = (stub_words[i] >> 16) & 0xFF;
            stub_bytes[i*4 + 2] = (stub_words[i] >>  8) & 0xFF;
            stub_bytes[i*4 + 3] = (stub_words[i] >>  0) & 0xFF;
        }
        cpu_physical_memory_write(stub_addr, stub_bytes, sizeof(stub_bytes));

        /*
         * NULL-fn-ptr guard for excExcHandle (plan 2026-05-13, simplified).
         *
         * The BSP's exception-hook dispatcher at runtime ~0x00179000 calls
         * registered handlers via `mtctr r10; bctrl` at 0x001791d8/dc. The
         * guard at 0x001791d0 only checks struct[+22] (16-bit "installed"
         * flag), not struct[+24] (the fn-ptr in r10). At vt~13s an entry
         * has +22 set as enabled (so bf falls through) and +24 = NULL →
         * bctrl jumps to NIP=0 → Program excp → kernel doesn't recover,
         * sysClkInt stops.
         *
         * Original plan tried installing a 6-insn null-guard stub at
         * 0x002acfb4 reached via `b 0x002acfb4` patched into 0x001791d4.
         * The plan assumed 0x002acfb4+ was dead printf-body. WRONG: the
         * function at 0x002acee8 ends at 0x002acfac with blr, and a
         * SEPARATE function (vfprintf-style varargs handler) starts at
         * 0x002acfb0 and extends to 0x002ad074. Corrupting that function's
         * body with our stub crashed the kernel within the first sysClkInt
         * tick because callers of 0x002acfb0 fell into our `mtctr r10;
         * bctrl` and jumped to whatever r10 happened to contain.
         *
         * Simpler fix per the plan's "Or simpler" alternative: replace the
         * bctrl at 0x001791dc with a nop. ALL handler calls at this site
         * are skipped — even valid ones. Tradeoff per session log: this
         * site is one of two indirect-call sites in the dispatcher.
         *
         * Plan 2026-05-13 (Phase A): the *other* indirect-call site at
         * 0x001791a8 (`mtctr r11; crclr 6; bctrl`) hits the same NULL
         * fn-ptr scenario — it's the source of the residual vt=9s HV_EMU
         * (NIP=0, MSR=0, LR=0). Apply identical fix: nop the bctrl. We
         * lose any debug/log hook handlers registered through *either*
         * slot, but the kernel itself is robust to missing hook calls.
         */
        const uint32_t guard_call_site_dc = 0x001791dc;
        const uint32_t guard_call_site_a8 = 0x001791a8;

        /* Pre-flight: confirm bctrl bytes at both call sites match. */
        {
            uint8_t pre_call_dc[4], pre_call_a8[4];
            cpu_physical_memory_read(guard_call_site_dc, pre_call_dc,
                                     sizeof(pre_call_dc));
            cpu_physical_memory_read(guard_call_site_a8, pre_call_a8,
                                     sizeof(pre_call_a8));
            static const uint8_t exp_call[4] = { 0x4e, 0x80, 0x04, 0x21 };
            if (memcmp(pre_call_dc, exp_call, sizeof(exp_call)) != 0) {
                fprintf(stderr,
                        "MPC5200: PRE-PATCH BYTES MISMATCH at 0x001791dc: "
                        "%02x%02x%02x%02x (expect bctrl 4e800421). "
                        "BSP image may have changed — refusing to patch.\n",
                        pre_call_dc[0], pre_call_dc[1],
                        pre_call_dc[2], pre_call_dc[3]);
                fflush(stderr);
                abort();
            }
            if (memcmp(pre_call_a8, exp_call, sizeof(exp_call)) != 0) {
                fprintf(stderr,
                        "MPC5200: PRE-PATCH BYTES MISMATCH at 0x001791a8: "
                        "%02x%02x%02x%02x (expect bctrl 4e800421). "
                        "BSP image may have changed — refusing to patch.\n",
                        pre_call_a8[0], pre_call_a8[1],
                        pre_call_a8[2], pre_call_a8[3]);
                fflush(stderr);
                abort();
            }
        }

        /* Patch bctrl -> nop (0x60000000) at both 0x001791a8 and 0x001791dc. */
        static const uint8_t nop_bytes[4] = { 0x60, 0x00, 0x00, 0x00 };
        cpu_physical_memory_write(guard_call_site_a8, nop_bytes,
                                  sizeof(nop_bytes));
        cpu_physical_memory_write(guard_call_site_dc, nop_bytes,
                                  sizeof(nop_bytes));

        /*
         * NETJOB-PROBE / RX-wake stub at 0x002acf80 (UNCHANGED). Run in
         * netTask context (via netJobAdd deferred-call). Does:
         *   1. Write flag=8 at 0x07BEDD04 (= struct+848 for tFecEndRx). This
         *      makes tFecEndRx's main loop take the work-path on wake instead
         *      of falling through to cleanup-and-exit.
         *   2. Write 0xCAFEBABE to NETJOB_PROBE MMIO so QEMU sees the call hit.
         *   3. Call semGive(0x07bee080) — proper kernel wake of tFecEndRx, in
         *      netTask (EE=1) context, so semGive can dispatch the scheduler.
         *   4. Return.
         * 13 insns / 52 bytes. semGive @ 0x002ff5c4.
         *
         * Note: probe stub has a latent LR-loop bug (no mflr/mtlr around `bl
         * semGive`) — when semGive returns, blr at probe_addr+0x30 returns
         * to itself = infinite self-loop. Currently masked because DECR
         * preempts every ~13ms. NOT fixed here (relocating to 0x002acfd0
         * collides with the live function at 0x002acfb0).
         *
         * The flag location 0x07BEDD04 = struct_ptr 0x07BED9B4 + 848.
         * struct_ptr back-computed from sem ID 0x07bee080 stored at
         * struct+1248 (per disasm of tFecEndRx entry @ 0x12e294).
         *
         * Side-effect: probe stub's trailing blr at offset 0x30 lands at
         * 0x002acfb0 — overwriting the FIRST instruction of the live
         * function at 0x002acfb0. Calls to that function early-return
         * harmlessly (no frame allocated). This is a known/lucky aliasing
         * we depend on.
         */
        const uint32_t probe_addr = 0x002acf80;
        const uint32_t semgive_addr = 0x002ff5c4;
        uint32_t bl_semgive =
            0x48000000u | ((semgive_addr - (probe_addr + 0x2C)) & 0x03FFFFFC) | 1u;
        const uint32_t probe_words[13] = {
            0x3d2007be,    /* +0x00  lis   r9,  0x07BE                      */
            0x6129dd04,    /* +0x04  ori   r9,  r9, 0xDD04 (= 0x07BEDD04)   */
            0x39400008,    /* +0x08  li    r10, 8                            */
            0x91490000,    /* +0x0C  stw   r10, 0(r9) (flag = 8)             */
            0x3d20f000,    /* +0x10  lis   r9,  0xF000                      */
            0x61294038,    /* +0x14  ori   r9,  r9, 0x4038 (NETJOB_PROBE)   */
            0x3d40cafe,    /* +0x18  lis   r10, 0xCAFE                      */
            0x614ababe,    /* +0x1C  ori   r10, r10, 0xBABE                 */
            0x91490000,    /* +0x20  stw   r10, 0(r9) (probe = 0xCAFEBABE)  */
            0x3c6007be,    /* +0x24  lis   r3,  0x07BE                      */
            0x6063e080,    /* +0x28  ori   r3,  r3, 0xE080 (sem ID)         */
            bl_semgive,    /* +0x2C  bl    0x002ff5c4 (semGive)              */
            0x4e800020,    /* +0x30  blr  (lands at 0x002acfb0)              */
        };
        uint8_t probe_bytes[sizeof(probe_words)];
        for (unsigned i = 0; i < ARRAY_SIZE(probe_words); i++) {
            probe_bytes[i*4 + 0] = (probe_words[i] >> 24) & 0xFF;
            probe_bytes[i*4 + 1] = (probe_words[i] >> 16) & 0xFF;
            probe_bytes[i*4 + 2] = (probe_words[i] >>  8) & 0xFF;
            probe_bytes[i*4 + 3] = (probe_words[i] >>  0) & 0xFF;
        }
        cpu_physical_memory_write(probe_addr, probe_bytes, sizeof(probe_bytes));

        /* Patch sysClkInt: replace `bl 0x00138a7c` at 0x001180b8 with
         * `bl <stub_addr>`. The original orig-bl encoding is 0x480209c5;
         * the new encoding lands in stub_bytes layout above. */
        uint8_t patch[4] = {
            (bl_to_stub >> 24) & 0xFF,
            (bl_to_stub >> 16) & 0xFF,
            (bl_to_stub >>  8) & 0xFF,
            (bl_to_stub >>  0) & 0xFF,
        };
        cpu_physical_memory_write(patch_site, patch, sizeof(patch));
    }

    /* Read-back verification: dump the patched bytes so we can confirm
     * the writes landed (catches DRAM-mapping / read-only-region issues
     * that would otherwise be silent). */
    {
        uint8_t v_site[4], v_stub[16], v_probe[16];
        uint8_t v_guard_dc[4], v_guard_a8[4];
        cpu_physical_memory_read(0x001180b8, v_site, sizeof(v_site));
        cpu_physical_memory_read(0x002acef0, v_stub, sizeof(v_stub));
        cpu_physical_memory_read(0x002acf80, v_probe, sizeof(v_probe));
        cpu_physical_memory_read(0x001791dc, v_guard_dc, sizeof(v_guard_dc));
        cpu_physical_memory_read(0x001791a8, v_guard_a8, sizeof(v_guard_a8));
        fprintf(stderr,
                "MPC5200: post-patch verify: 0x001180b8 = %02x%02x%02x%02x "
                "(expect bl 0x002acef0 = 48194e39); "
                "0x002acef0 = %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                "(expect mflr r12 = 7d8802a6, bl intUnlock = 4be8bb89, "
                "lis r10,0xF000 = 3d40f000, ori = 614a4000); "
                "probe @ 0x002acf80 = %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                "(expect lis r9,0x07BE = 3d2007be, ori = 6129dd04, "
                "li r10,8 = 39400008, stw r10,0(r9) = 91490000); "
                "0x001791a8 = %02x%02x%02x%02x (expect nop = 60000000, "
                "was bctrl = 4e800421); "
                "0x001791dc = %02x%02x%02x%02x (expect nop = 60000000, "
                "was bctrl = 4e800421)\n",
                v_site[0], v_site[1], v_site[2], v_site[3],
                v_stub[0], v_stub[1], v_stub[2], v_stub[3],
                v_stub[4], v_stub[5], v_stub[6], v_stub[7],
                v_stub[8], v_stub[9], v_stub[10], v_stub[11],
                v_stub[12], v_stub[13], v_stub[14], v_stub[15],
                v_probe[0], v_probe[1], v_probe[2], v_probe[3],
                v_probe[4], v_probe[5], v_probe[6], v_probe[7],
                v_probe[8], v_probe[9], v_probe[10], v_probe[11],
                v_probe[12], v_probe[13], v_probe[14], v_probe[15],
                v_guard_a8[0], v_guard_a8[1], v_guard_a8[2], v_guard_a8[3],
                v_guard_dc[0], v_guard_dc[1], v_guard_dc[2], v_guard_dc[3]);
    }

    fprintf(stderr,
            "MPC5200: applied CT296 KeySwitch bypass patches at 0x12d390, "
            "0x12ae60; force-zeroed app-spawn gate at 0x00962e2c; "
            "installed FS-hook hypercall stubs at open=0x2b13c8 "
            "read=0x2b18fc close=0x2b17d4 exists=0x13ffe4 "
            "(doorbell @ 0xF0004000, root=%s); "
            "no-op'd printf at 0x2acee8 (PSC1 TX IRQ workaround); "
            "installed sysClkInt tail-patch @ 0x001180b8 -> stub @ 0x002acef0 "
            "(35-insn shim: netjob path -> netJobAdd @ 0x0022c288 + "
            "sem-queue path per-sem dispatch on bit 9 of sem ID -> "
            "wake tFecEndRx (TCB 0x07BEDE38, prio 29) or tRootTask "
            "(TCB 0x07FEFE00, prio 0) via qPriBMapPut @ 0x002cd8e0); "
            "nopped excExcHandle bctrl @ 0x001791a8 + 0x001791dc (skips all "
            "hook-handler calls at both indirect-call sites to avoid "
            "NULL-fn-ptr crashes); "
            "probe stub @ 0x002acf80 (writes 0xCAFEBABE to MMIO 0xF0004038)\n",
            fshook_root());
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

    /* === tFecEndRx body progression (plan 2026-05-13 Phase C) ===
     * Body verified by disasm @ 0x0012e294. Flow:
     *   0x12e294: lwz r3, 0x4e0(r31) ; load sem-id from struct+0x4e0
     *   0x12e29c: bl semTake (sig)
     *   0x12e2a0: cmpwi r3, 0
     *   0x12e2a4: bne -> 0x12e2b4 (semTake error -> early-exit)
     *   0x12e2a8: lwz r9, 0x350(r31) ; load work-flag from struct+848
     *   0x12e2ac: cmpwi cr7, r9, 8
     *   0x12e2b0: beq cr7, -> 0x12e2c8 (flag == 8 -> work-path)
     *   0x12e2b4: ; early-exit: lwz r3, 0x4e4 + bl + br to 0x1a4ee4
     *   0x12e2c8: ; work-continue: stwu, etc.
     */
    { 0x0012e294, 0x0012e297, "VX: tFecEndRx entry (pre-semTake)",     false, 0 },
    { 0x0012e2a8, 0x0012e2ab, "VX: tFecEndRx struct+848 check",        false, 0 },
    { 0x0012e2b4, 0x0012e2b7, "VX: tFecEndRx early-exit (flag != 8)",  false, 0 },
    { 0x0012e2c8, 0x0012e2cb, "VX: tFecEndRx work-continue path",      false, 0 },

    /* === Vestas-app spawn gate (plan 2026-04-30) === */
    { 0x00100920, 0x00100923, "VX: gate-fn entry (0x100920)",            false, 0 },
    { 0x001009d8, 0x001009db, "VX: gate-check load *(0x962e2c)",         false, 0 },
    { 0x001009e0, 0x001009e3, "VX: gate-check branch (skip if !=0)",     false, 0 },
    { 0x001009e4, 0x001009ef, "VX: gate-pass (sets *(0x95a5a0)=1)",      false, 0 },
    { 0x0014adc0, 0x0014adc3, "VX: error-print xref to 0x962e2c",        false, 0 },

    /* === FS gate (plan 2026-05-01) === */
    { 0x0013ffe4, 0x0013ffe7, "VX: fs-exists check entry (stubbed)",     false, 0 },
    { 0x0014c7d4, 0x0014c7d7, "VX: startup-loop call to fs-exists",      false, 0 },
    { 0x0014c7ec, 0x0014c7ef, "VX: startup-loop SUCCESS branch",         false, 0 },
    { 0x0014c828, 0x0014c82b, "VX: startup-loop FAILURE branch (Cannot find)", false, 0 },
    { 0x001061c0, 0x001061c3, "VX: script-runner entry (post-banner)",   false, 0 },
    { 0x002b13c8, 0x002b13cb, "VX: open() entry",                        false, 0 },
    { 0x002a5508, 0x002a550b, "VX: fopen() entry",                       false, 0 },

    /* === SDMA Main ISR (plan 2026-05-05, gate 9 dispatch hunt) ===
     * intConnect(0, 0x132854, ...) at 0x1329a0 registers the SDMA Main
     * ISR. If this station hits, EXT dispatch reaches the ISR — problem
     * is sem-post (Phase 2). If it never hits despite SDMA RAISE in the
     * eval log, the problem is dispatch (Phase 1). */
    { 0x00132854, 0x00132857, "VX: SDMA Main ISR entry (0x132854)",      false, 0 },

    /* === sysClkInt tail-patch shim (plan 2026-05-06; updated 2026-05-07) ===
     * Stub @ 0x002acef0 fires once per ~17 ms tick — first hit confirms
     * patch is wired correctly. semFlush @ 0x002ff884 fires only when
     * the RX hook (or synthetic doorbell) has set sem_queue — first hit
     * there confirms the shim actually delivered a wake.
     *
     * Both semGive (0x002ff5c4) and semFlush (0x002ff884) are sampled
     * here — semGive is unused as the tail-call target after the 2026-05-07
     * swap, but it still gets called by ordinary BSP code and lights up
     * during boot, so its station tells us nothing about the shim. */
    { 0x002acef0, 0x002acef3, "VX: sysClkInt tail-patch stub entry",     false, 0 },
    { 0x002acf80, 0x002acf83, "VX: NETJOB-PROBE stub entry (deferred)",   false, 0 },
    { 0x0022c288, 0x0022c28b, "VX: netJobAdd entry (sig-matched)",        false, 0 },
    { 0x0022c0fc, 0x0022c0ff, "VX: netTask entry (sig-matched)",          false, 0 },
    { 0x002ff5c4, 0x002ff5c7, "VX: semGive entry (BSP-direct, not shim)", false, 0 },
    { 0x002ff884, 0x002ff887, "VX: semFlush entry (via shim)",           false, 0 },

    /* === usrRoot stall hunt (plan 2026-05-03) === */
    { 0x00107a08, 0x00107a0b, "VX: bl usrKernelCoreInit",                false, 0 },
    { 0x00107a14, 0x00107a17, "VX: bl memInit",                          false, 0 },
    { 0x00107a18, 0x00107a1b, "VX: bl wncan_core_init",                  false, 0 },
    { 0x00107a24, 0x00107a27, "VX: bl memPartLibInit",                   false, 0 },
    { 0x00107a28, 0x00107a2b, "VX: bl usrMmuInit",                       false, 0 },
    { 0x00107a2c, 0x00107a2f, "VX: bl sysClkInit",                       false, 0 },
    { 0x00107a34, 0x00107a37, "VX: bl selectInit",                       false, 0 },
    { 0x00107a38, 0x00107a3b, "VX: bl usrIosCoreInit",                   false, 0 },
    { 0x00107a3c, 0x00107a3f, "VX: bl usrKernelExtraInit",               false, 0 },
    { 0x00107a40, 0x00107a43, "VX: bl usrIosExtraInit",                  false, 0 },
    { 0x00107a44, 0x00107a47, "VX: bl usrNetworkInit",                   false, 0 },
    { 0x00107a48, 0x00107a4b, "VX: bl selTaskDeleteHookAdd",             false, 0 },
    { 0x00107a4c, 0x00107a4f, "VX: bl usrToolsInit",                     false, 0 },
    { 0x00107a50, 0x00107a53, "VX: bl cplusCtorsLink",                   false, 0 },
    { 0x00107a54, 0x00107a57, "VX: bl wn_mpc5200Can_init",               false, 0 },
    { 0x00107a58, 0x00107a5b, "VX: bl wncan_attach",                     false, 0 },
    { 0x00107a5c, 0x00107a5f, "VX: bl usrAppInit",                       false, 0 },
    { 0x00107a68, 0x00107a6b, "VX: bl usrStartupScript",                 false, 0 },

    /* === FTP path stations (plan 2026-04-30 Phase B) ===
     * tRootTask post-wake: 0x00177b34 is the loop-test after the bl at
     * 0x00177b2c per plan. Hit means tRootTask resumed past the wedge.
     * ftpdInit body: 0x0017de2c is the addi forming "(%d) ftpdTask
     * created" string addr (0x00399530); the bl at 0x0017de34 is the
     * printf call. If 0x0017de2c hits, ftpdTask was successfully
     * created — stronger signal than just usrToolsInit. */
    { 0x00177b34, 0x00177b37, "VX: tRootTask post-wake (after wedge)",   false, 0 },
    { 0x0017de2c, 0x0017de2f, "VX: ftpdTask-created printf (in ftpdInit)", false, 0 },
};

/* NIP histogram across full bootrom .text — reveals idle loops. */
#define NIP_HIST_BASE  0x01000000UL
#define NIP_HIST_END   0x011f4000UL
#define NIP_HIST_SIZE  ((NIP_HIST_END - NIP_HIST_BASE) / 4)
static unsigned g_nip_hist[NIP_HIST_SIZE];

/*
 * Step-1 (gate-9 follow-up, plan 2026-05-07): parallel histogram covering
 * vxworks.out semGive (0x002ff5c4) + semFlush (0x002ff884) bodies. Tells
 * us which path the wake actually ran:
 *   0x002ff5c4..0x002ff60c — semGive fast-path dispatch wrapper
 *   0x002ff610..0x002ff6dc — semGive slow-path kernel hooks
 *   0x002ff700+           — semGive class-byte dispatch indirect call
 *   0x002ff714+           — semGive post-dispatch
 *   0x002ff884..          — semFlush body (the new tail-call target)
 */
#define SEMGIVE_HIST_BASE 0x002ff5c0UL
#define SEMGIVE_HIST_END  0x002ffa00UL
#define SEMGIVE_HIST_SIZE ((SEMGIVE_HIST_END - SEMGIVE_HIST_BASE) / 4)
static unsigned g_semgive_hist[SEMGIVE_HIST_SIZE];

static void mpc5200_diag_sample(void *opaque)
{
    MPC5200State *s = opaque;
    static int  diag_count = 0;
    const int   diag_max   = 600000; /* 600000 × 100us = 60 s of virtual time */
    static bool patches_applied = false;
    int         i;

    /*
     * Apply BSP patches at the very first diag sample (~1us virtual time),
     * BEFORE the BSP has a chance to enter any of the patched functions.
     * Was previously in mpc5200_tick (60Hz, first fire delayed 1s) — by
     * which time usrToolsInit had already entered printf and PEND'd on
     * the unposted PSC1 TX sem.
     */
    if (!patches_applied) {
        mpc5200_apply_keyswitch_patches();
        patches_applied = true;
    }

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

    /* Step-1 histogram — semGive + semFlush body coverage. */
    if (nip >= SEMGIVE_HIST_BASE && nip < SEMGIVE_HIST_END) {
        unsigned idx = (nip - SEMGIVE_HIST_BASE) / 4;
        if (g_semgive_hist[idx] < UINT_MAX) {
            g_semgive_hist[idx]++;
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

    /*
     * Vestas-app spawn-gate watcher (plan 2026-04-30, Phase 1a Option A).
     * Logs every change to *(0x00962e2c) (gate input) and *(0x0095a5a0)
     * (Vestas-mode flag set when gate passes). Sentinel 0xDEADBEEF means
     * "not observed yet".
     */
    {
        static uint32_t last_gate    = 0xDEADBEEF;
        static uint32_t last_apmode  = 0xDEADBEEF;
        uint32_t gate_val   = ldl_be_phys(&address_space_memory, 0x00962e2c);
        uint32_t apmode_val = ldl_be_phys(&address_space_memory, 0x0095a5a0);
        if (gate_val != last_gate) {
            fprintf(stderr,
                    "GATE: *(0x00962e2c) %s 0x%08x at NIP=0x%08x\n",
                    last_gate == 0xDEADBEEF ? "init" : "->",
                    gate_val, (unsigned)nip);
            fflush(stderr);
            last_gate = gate_val;
        }
        if (apmode_val != last_apmode) {
            fprintf(stderr,
                    "GATE: *(0x0095a5a0) %s 0x%08x at NIP=0x%08x  (Vestas-mode flag)\n",
                    last_apmode == 0xDEADBEEF ? "init" : "->",
                    apmode_val, (unsigned)nip);
            fflush(stderr);
            last_apmode = apmode_val;
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
        /* SEM-HIST: every non-zero entry in the semGive+semFlush body
         * histogram, in ascending-address order (small range — ~272
         * slots — so listing all hits is more useful than top-K for
         * tracing which path actually ran). */
        {
            unsigned hits = 0;
            fprintf(stderr,
                    "SEM-HIST: semGive+semFlush body samples "
                    "(0x%08x..0x%08x):\n",
                    (unsigned)SEMGIVE_HIST_BASE,
                    (unsigned)SEMGIVE_HIST_END);
            for (j = 0; j < SEMGIVE_HIST_SIZE; j++) {
                if (g_semgive_hist[j] == 0) continue;
                fprintf(stderr, "SEM-HIST:   0x%08x : %u\n",
                        (unsigned)(SEMGIVE_HIST_BASE + j * 4),
                        g_semgive_hist[j]);
                hits++;
            }
            fprintf(stderr, "SEM-HIST: %u distinct PCs sampled\n", hits);
        }
        fflush(stderr);
    }
}

static void mpc5200_tick(void *opaque)
{
    MPC5200State *s = opaque;
    static int  tick_count = 0;
    static bool ext_armed  = false;
    /* Patches now applied from mpc5200_diag_sample (fires at 1us, much
     * earlier than this 60Hz tick which is delayed 1s by design). */

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
    /*
     * Synthetic doorbell test (plan 2026-05-06, Step 5 verification).
     *
     * Fire BEFORE tFecEndRecover wakes at t≈14s (it runs m5200FecRestart
     * which destroys tFecEndRx, per 2026-05-05 finding). At t=8s the BSP
     * is at steady state with tFecEndRx PEND on 0x07bee080.
     *
     * Bypass the RX-hook path entirely: the sysClkInt tail-patch picks
     * up the doorbell on its next tick (~17ms), tail-calls semGive on
     * sem 0x07bee080, and tFecEndRx should leave PEND.
     *
     * Also: dump s->fshook.sem_queue once per second after the write —
     * if the shim ran, it would have cleared the slot back to 0. A
     * persistent non-zero value across multiple ticks means the shim
     * is NOT running.
     */
    /*
     * SYNTH-DOORBELL (kept). The shim's qPriBMapPut path force-readies
     * tFecEndRx. Combined with netjob arming below, both paths run on
     * sequential sysClkInt ticks (netjob first, sem-queue second).
     */
    if (tick_count == 60 * 8) {
        fprintf(stderr,
                "SYNTH-DOORBELL: writing s->fshook.sem_queue = 0x07bee080 at "
                "t=8s — expect tFecEndRx wake by t=9s\n");
        fflush(stderr);
        s->fshook.sem_queue = 0x07bee080;
    }

    /*
     * ROOTTASK-DOORBELL (plan 2026-04-30 pivot): tRootTask PEND'd on sem
     * 0x07bee378 (WAIT_FOREVER, errno=0x00030065) inside usrAppInit's
     * service-init chain. Stack chain (frame 20 lr=0x00107a60) confirms
     * task is past `bl usrAppInit` at 0x00107a5c.
     *
     * Saved PC=0x00177b30 is post-`bl 0x00207efc` (windExit) inside the
     * sem-class take-loop fn at 0x001779e0. The wait was reached via
     * usrAppInit → 0x0014ce34 → ... → fn @ 0x0012b658 doing
     * `lwz r3, 136(r3); bl 0x002ff730` (semTake on object+136 = sem
     * 0x07bee378). Owning object @ 0x07bee2F0.
     *
     * Sem is dynamically allocated (not in .data/BSS); no semGive call
     * site found in trace, and all 11 downstream PEND'd services are
     * gated behind tRootTask. Classic orphan-sem: the would-be giver
     * never runs because its parent init is blocked here.
     *
     * Strategy: reuse the SYNTH-DOORBELL infra to semGive sem 0x07bee378
     * via the BSP shim's sysClkInt tail-patch. Post at multiple vt's so
     * (1) if tRootTask hasn't reached the semTake yet, the sem count is
     * pre-incremented; (2) if it's already PEND, the wake fires; (3) if
     * the wait-loop iterates and re-blocks, subsequent posts unblock.
     *
     * Guarded with `sem_queue == 0` so we don't clobber the FEC RX post
     * at vt=8s. Each post is consumed within ~17ms (one sysClkInt tick).
     */
    if ((tick_count == 60 * 5 || tick_count == 60 * 10 ||
         tick_count == 60 * 12 || tick_count == 60 * 15 ||
         tick_count == 60 * 20 || tick_count == 60 * 25 ||
         tick_count == 60 * 30 || tick_count == 60 * 35) &&
        s->fshook.sem_queue == 0) {
        fprintf(stderr,
                "ROOTTASK-DOORBELL: writing s->fshook.sem_queue = 0x07bee378 "
                "at vt=%ds — wake tRootTask from PEND inside usrAppInit chain\n",
                tick_count / 60);
        fflush(stderr);
        s->fshook.sem_queue = 0x07bee378;
    }

    /*
     * ENTRY-PROBE (plan 2026-05-11): one-shot dump at vt=2s of TCB+0x80..+0xC0
     * for tFecEndRx (0x07BEDE38). Goal: identify the entry-PC offset (standard
     * VxWorks puts the task entry function pointer somewhere near here).
     * Cross-check against a code-address range (0x001xxxxx) to find the field
     * — the right offset will look like a function in vxworks.out's .text.
     */
    /*
     * ENTRY-PROBE: confirmed once, kept for documentation. tFecEndRx entry
     * function = TCB+0x74 = 0x0012e268. First blocking call: semTake at
     * 0x002ff730 with sem from r3+1248 (= 0x07bee080). After wake, task
     * checks flag at r3+848 == 8, else falls through to cleanup/exit.
     * Struct ptr (= taskInit r3 arg) computed: 0x07bee080 - 1248 = 0x07BEDBA0.
     */
    /* ENTRY-PROBE: doc-only summary at vt=2s.
     * tFecEndRx entry=0x0012e268 (TCB+0x74). First block: semTake @ 0x002ff730
     * via r3+1248. Struct ptr = 0x07BED9B4 (back-comp from sem 0x07bee080
     * stored at struct+1248). Flag at struct+848=0x07BEDD04 must be 8 for
     * tFecEndRx to enter work-path on wake. */
    if (tick_count == 60 * 2) {
        AddressSpace *as = &address_space_memory;
        const uint32_t TCB = 0x07bede38;
        uint32_t entry = ldl_be_phys(as, TCB + 0x74);
        fprintf(stderr, "ENTRY-PROBE: tFecEndRx entry=0x%08x (TCB+0x74), "
                "struct_ptr=0x07bed9b4, sem=0x07bee080, flag@0x07bedd04\n", entry);
        fflush(stderr);
    }

    /*
     * ROOTTASK-COORDS probe (plan 2026-04-30, Phase A). One-shot at vt=3s.
     * Walks activeQ DLL anchored at *(0x008d94e4), finds the TCB whose name
     * (TCB+0x34 → ASCIIZ) starts with "tRootTask", and prints:
     *   - TCB address
     *   - priority (TCB+0x40)
     *   - readyQ head/bmap/vtable (assumes same global readyQ at 0x0099AF58)
     *   - TCB.status, TCB.pSemId, saved PC
     * These are the constants needed for the Phase B shim wake-stub.
     * Read-only; no state changes.
     */
    if (tick_count == 60 * 3 || tick_count == 60 * 8) {
        AddressSpace *as = &address_space_memory;
        const uint32_t ACTIVEQ_HEAD = 0x008d94e4;
        const uint32_t READYQ       = 0x0099af58;
        uint32_t cur = ldl_be_phys(as, ACTIVEQ_HEAD);
        uint32_t found_tcb = 0;
        uint32_t found_prio = 0;
        uint32_t found_status = 0;
        uint32_t found_pSemId = 0;
        uint32_t found_pc = 0;
        for (int n = 0; n < 64 && cur && cur != ACTIVEQ_HEAD; n++) {
            uint32_t tcb     = cur - 0x20;
            uint32_t name_pa = ldl_be_phys(as, tcb + 0x34);
            char nm[16] = {0};
            if (name_pa && name_pa < 0x10000000) {
                cpu_physical_memory_read(name_pa, (uint8_t *)nm, 15);
            }
            if (strncmp(nm, "tRootTask", 9) == 0) {
                found_tcb    = tcb;
                found_prio   = ldl_be_phys(as, tcb + 0x40);
                found_status = ldl_be_phys(as, tcb + 0x3C);
                found_pSemId = ldl_be_phys(as, tcb + 0x5C);
                found_pc     = ldl_be_phys(as, tcb + 0x130 + 0x8C);
                break;
            }
            cur = ldl_be_phys(as, cur);
        }
        if (found_tcb) {
            uint32_t rqhead = ldl_be_phys(as, READYQ + 0x00);
            uint32_t rqbmap = ldl_be_phys(as, READYQ + 0x04);
            uint32_t rqvtbl = ldl_be_phys(as, READYQ + 0x0C);
            fprintf(stderr,
                    "ROOTTASK-COORDS: tcb=0x%08x pri=%u status=0x%x "
                    "pSemId=0x%08x PC=0x%08x\n"
                    "ROOTTASK-COORDS:   readyQ=0x%08x head=0x%08x "
                    "bmap=0x%08x vtbl=0x%08x\n",
                    found_tcb, found_prio, found_status,
                    found_pSemId, found_pc,
                    READYQ, rqhead, rqbmap, rqvtbl);
        } else {
            fprintf(stderr,
                    "ROOTTASK-COORDS: NOT FOUND in activeQ at vt=3s "
                    "(activeQHead=0x%08x first=0x%08x)\n",
                    ACTIVEQ_HEAD, ldl_be_phys(as, ACTIVEQ_HEAD));
        }
        fflush(stderr);
    }

    /*
     * NETJOB-DOORBELL test (plan 2026-05-11 step 3): at vt=12s, arm the
     * netjob doorbell with the probe stub. The next sysClkInt picks it up,
     * tail-calls netJobAdd(probe, 0,0,0,0,0). netJobAdd enqueues + semGives
     * netTaskSem. netTask wakes, dequeues, calls probe(0,0,0,0,0). Probe
     * writes 0xCAFEBABE to NETJOB_PROBE MMIO. QEMU logs "NETJOB-PROBE: flag
     * flipped" — confirms the full netJobAdd→netTask deferred path works.
     *
     * Pre-conditions: tNetTask must be PEND on netTaskSem (= netJobInfo+12).
     * If tNetTask isn't running yet (ENT init not done) the semGive queues
     * but no consumer drains; we'd see no probe flip.
     */
    /*
     * Arm at multiple times. sysClkInt may stop firing past vt~11.5s when
     * tFecEndRx becomes READY-but-stuck (kernel scheduler degenerates).
     * Arm at vt=4s (pre-sem-wake), vt=6s, vt=10s — one of these should hit
     * a live sysClkInt firing before the scheduler stalls.
     */
    /*
     * Arm netjob doorbell at multiple times. By vt=8s tFecEndRx is PEND on
     * sem 0x07bee080. Our wake-stub semGives that sem in netTask context
     * (proper kernel path), waking tFecEndRx with correct register state.
     * Re-arm at later times in case sysClkInt isn't picking up earlier arms.
     */
    if ((tick_count == 60 * 8 || tick_count == 60 * 9 ||
         tick_count == 60 * 10) && s->fshook.netjob_func == 0) {
        s->fshook.netjob_args[0] = 0;
        s->fshook.netjob_args[1] = 0;
        s->fshook.netjob_args[2] = 0;
        s->fshook.netjob_args[3] = 0;
        s->fshook.netjob_args[4] = 0;
        s->fshook.netjob_func    = 0x002acf80;  /* probe stub (wake tFecEndRx) */
        fprintf(stderr,
                "NETJOB-DOORBELL: armed at vt=%ds, func=0x002acf80 "
                "(wake-stub); expecting NETJOB-PROBE within ~2s\n",
                tick_count / 60);
        fflush(stderr);
    }
    if (tick_count >= 60 * 4 && tick_count <= 60 * 20 &&
        (tick_count % 30) == 0) {
        fprintf(stderr,
                "NETJOB-WATCH: t=%d.%02ds netjob_func=0x%08x "
                "netjob_probe=0x%08x\n",
                tick_count / 60, (tick_count % 60) * 100 / 60,
                s->fshook.netjob_func, s->fshook.netjob_probe);
        fflush(stderr);
    }
    if (tick_count >= 60 * 8 && tick_count <= 60 * 14 && (tick_count % 60) == 0) {
        fprintf(stderr, "SYNTH-DOORBELL: t=%ds, s->fshook.sem_queue = 0x%08x "
                "(0 means BSP shim cleared it)\n",
                tick_count / 60, s->fshook.sem_queue);
        fflush(stderr);
    }

    /*
     * READYQ-PROBE (plan 2026-04-29 follow-up).
     *
     * The 2026-05-09 session pivot tried Track A — manual TCB.status flip
     * + sem-queue clear from QEMU. Result: TCB.status went READY but the
     * scheduler still wouldn't dispatch the task. Confirmed the scheduler
     * reads a separate readyQ, NOT TCB.status, when picking the next task
     * to run. Track A's writes are now REVERTED (they corrupted the sem
     * state that windPendQGet needs to find the waiter).
     *
     * Replacement strategy: redirect the existing semGive shim's tail-call
     * from semFlush (workQ-deferred — never drains) to windPendQGet @
     * 0x00178550, which inlines the full qGet+windReadyQPut wake primitive
     * (qGet from sem.pendQ → clear PEND in TCB.status → tail-call qPut on
     * readyQ at 0x0099AF58). Same one-shot doorbell at vt=8s; the BSP shim
     * picks it up next sysClkInt (~17 ms later) and does the proper kernel
     * wake instead of the previous workQ-deferred path.
     *
     * READYQ-PROBE confirms the wake actually landed. Reads:
     *   *(0x07bee080+0x00) — sem.qHead. After windPendQGet, should be the
     *     empty sentinel (0x0099af68) since we had only one waiter.
     *   *(0x07bede38+0x3C) — TCB.status. After wake, expect 0 (READY).
     *   *(0x0099AF58+0x00) — readyQ head. After put, this should be our TCB
     *     (0x07bede38) IF its priority (29) is the lowest currently READY.
     *
     * One-shot at vt=10s (giving the shim ~2s of sysClkInt firings to run).
     */
    if (tick_count == 60 * 10) {
        AddressSpace *as = &address_space_memory;
        const uint32_t SEM     = 0x07bee080;
        const uint32_t TCB     = 0x07bede38;
        const uint32_t READYQ  = 0x0099af58;
        uint32_t qhead   = ldl_be_phys(as, SEM + 0x00);
        uint32_t qtail   = ldl_be_phys(as, SEM + 0x04);
        uint32_t status  = ldl_be_phys(as, TCB + 0x3C);
        uint32_t pSemId  = ldl_be_phys(as, TCB + 0x5C);
        uint32_t our_key = ldl_be_phys(as, TCB + 0x08);
        uint32_t rqhead  = ldl_be_phys(as, READYQ + 0x00);
        uint32_t rqbmap  = ldl_be_phys(as, READYQ + 0x04);
        uint32_t rqvtbl  = ldl_be_phys(as, READYQ + 0x0C);
        uint32_t head_key = rqhead ? ldl_be_phys(as, rqhead + 0x08) : 0;
        uint32_t bmap_w0 = rqbmap ? ldl_be_phys(as, rqbmap + 0x00) : 0;
        uint32_t bmap_w1 = rqbmap ? ldl_be_phys(as, rqbmap + 0x04) : 0;
        fprintf(stderr,
                "READYQ-PROBE: t=10s\n"
                "READYQ-PROBE:   sem 0x%08x qHead=0x%08x qTail=0x%08x\n"
                "READYQ-PROBE:   tcb 0x%08x status=0x%x pSemId=0x%08x our_key(TCB+8)=0x%08x\n"
                "READYQ-PROBE:   readyQ@0x%08x first=0x%08x bmap=0x%08x vtbl=0x%08x\n"
                "READYQ-PROBE:     readyQ.first.key(=first+8)=0x%08x\n"
                "READYQ-PROBE:     bmap[0]=0x%08x bmap[1]=0x%08x\n",
                SEM, qhead, qtail,
                TCB, status, pSemId, our_key,
                READYQ, rqhead, rqbmap, rqvtbl,
                head_key, bmap_w0, bmap_w1);

        /*
         * READYQ-FORCE (plan 2026-04-29 escalation): the BSP-side qPriBMapPut
         * call from the shim correctly writes TCB+0x08 = key (= 0x1D = 29),
         * but readyQ.first stays at CpuloadLow's TCB and bmap[0] stays at
         * 0x01 (only CpuloadLow's bit). Either qPriBMapPut got called with
         * wrong args, or its writes got rolled back by a context switch
         * before our probe. Force the issue by directly writing readyQ.first
         * = our TCB and setting bit 28 in bmap[0] (= prio 29 group).
         *
         * This is option (b) of the plan ("splice manually as a one-shot").
         */
        uint32_t pre_bmap0 = bmap_w0;
        stl_be_phys(as, READYQ + 0x00, TCB);
        if (rqbmap) {
            stl_be_phys(as, rqbmap + 0x00, pre_bmap0 | (1u << 28));
            /* Also set the within-group byte. For prio 29: byte index =
             * (255-29)/8 = 28; bit within byte = (255-29)&7 = 2. */
            uint32_t byte_off = rqbmap + 4 + 28;
            uint8_t b;
            cpu_physical_memory_read(byte_off, &b, 1);
            b |= (1u << 2);
            cpu_physical_memory_write(byte_off, &b, 1);
        }
        fprintf(stderr,
                "READYQ-FORCE: wrote readyQ.first = 0x%08x, set bmap[0] |= bit 28 "
                "(was 0x%08x, now should be 0x%08x), set byte[28] |= 0x04\n",
                TCB, pre_bmap0, pre_bmap0 | (1u << 28));
        fflush(stderr);
    }

    /*
     * Step 2 (gate-9 follow-up, plan 2026-05-07): one-shot sem-layout dump
     * at vt=10s. Compares the structure at three known PEND'd sems —
     * tFecEndRx (0x07bee080, the failing one), tNetTask (0x00980a48,
     * presumed-OK BSP-internal), tWdbTask (0x07ba6828, debug agent).
     * If layouts agree on offsets, our reading is right and the issue
     * is in semGive's give logic, not sem-layout. Particular check:
     * byte at +4 — for 0x07bee080 it's 0x07 (top byte of the 0x07bede38
     * queue-prev pointer); semGive's slow path does `lbz r0, 4(r31)`
     * to derive the class-table index.
     */
    if (tick_count == 60 * 10) {
        AddressSpace *as = &address_space_memory;
        static const struct { uint32_t addr; const char *name; } sems[] = {
            { 0x07bee080, "tFecEndRx (failing)" },
            { 0x00980a48, "tNetTask (BSP-internal)" },
            { 0x07ba6828, "tWdbTask (debug agent)" },
        };
        for (int i = 0; i < (int)ARRAY_SIZE(sems); i++) {
            uint32_t w[8];
            for (int j = 0; j < 8; j++) {
                w[j] = ldl_be_phys(as, sems[i].addr + j * 4);
            }
            uint8_t b0 = (w[1] >> 24) & 0xff;
            fprintf(stderr,
                    "SEM-LAYOUT: 0x%08x %s\n"
                    "SEM-LAYOUT:   [+0x00]=0x%08x [+0x04]=0x%08x [+0x08]=0x%08x [+0x0C]=0x%08x\n"
                    "SEM-LAYOUT:   [+0x10]=0x%08x [+0x14]=0x%08x [+0x18]=0x%08x [+0x1C]=0x%08x\n"
                    "SEM-LAYOUT:   class-byte (top of word @ +4) = 0x%02x\n",
                    sems[i].addr, sems[i].name,
                    w[0], w[1], w[2], w[3],
                    w[4], w[5], w[6], w[7],
                    b0);
        }
        fflush(stderr);

        /*
         * Step 1.A (gate-9 follow-up, plan 2026-05-08): dump the class
         * dispatch table semFlush walks at 0x002ff9a0. Disasm:
         *   lbz   r0, 4(r31)                ; class byte from sem_id+4
         *   rlwinm r0, r0, 2, 0x1b, 0x1d    ; (class << 2) & 0x1c
         *   lwzx  r0, r9, r0                ; r0 = *(table_base + idx*4)
         * with r9 = 0x008d9564 (FIRST table base, used by class-table
         * dispatch when *(0x0090879c) == 0). The hook fn at 0x002ffdd8
         * (called when *(0x0090879c) != 0) uses a SECOND table at
         * 0x008d95a4 — dumped below as SEM-CLASS2.
         */
        const uint32_t class_base  = 0x008d9564;
        const uint32_t class2_base = 0x008d95a4;
        fprintf(stderr,
                "SEM-CLASS: 1st-table base = 0x%08x  (used by semFlush "
                "class-table dispatch at 0x002ff9b0..d0)\n", class_base);
        for (int i = 0; i < 8; i++) {
            uint32_t addr = class_base + i * 4;
            uint32_t fn   = ldl_be_phys(as, addr);
            fprintf(stderr,
                    "SEM-CLASS:   class[%d] @ 0x%08x = 0x%08x\n",
                    i, addr, fn);
        }
        fprintf(stderr,
                "SEM-CLASS2: 2nd-table base = 0x%08x  (used by hook fn "
                "0x002ffdd8 dispatch at 0x002ffe2c after validation)\n",
                class2_base);
        for (int i = 0; i < 8; i++) {
            uint32_t addr = class2_base + i * 4;
            uint32_t fn   = ldl_be_phys(as, addr);
            fprintf(stderr,
                    "SEM-CLASS2:  class[%d] @ 0x%08x = 0x%08x\n",
                    i, addr, fn);
        }
        /* Sentinel the hook fn compares qHead against. *(0x008d951c) is
         * loaded as r9, then r9+0x24 is also compared. Two values
         * sufficient to reproduce the validation logic in next session. */
        uint32_t sent      = ldl_be_phys(as, 0x008d951c);
        uint32_t sent_p24  = (sent && sent < 0x10000000)
                                ? ldl_be_phys(as, sent + 0x24) : 0;
        fprintf(stderr,
                "SEM-SENTINEL: *(0x008d951c) = 0x%08x ; "
                "*(sentinel+0x24) = 0x%08x  (qHead must equal one of "
                "these or the hook fn errors out)\n", sent, sent_p24);

        /*
         * Step 1.C (gate-9 follow-up): dump the BSS-resident sem
         * controls semFlush reads on its slow path. Sign-extension on
         *   lis r9, 0x91 ; lwz rN, -<imm>(r9)
         * gives us r9 = 0x00910000 and the addresses below by adding
         * (signed) the lwz offset to r9.
         *   0x008d5c60  kernelState (0 = scheduler dispatch direct)
         *   0x0090879c  sem global #1 (0x910000-0x7864)
         *   0x009083e8  sem global #2 (0x910000-0x7c18)
         *   0x008f8388  workQueue control byte (one we already read)
         */
        struct { uint32_t addr; const char *name; } globals[] = {
            { 0x008d5c60, "kernelState" },
            { 0x0091879c, "sem-glob-91879c"   },
            { 0x0090879c, "sem-glob-90879c"   },
            { 0x009083e8, "sem-glob-9083e8"   },
            { 0x008f8388, "workQ-ctl-8f8388"  },
            { 0x0091877c, "sem-glob-91877c"   },
        };
        for (int i = 0; i < (int)ARRAY_SIZE(globals); i++) {
            uint32_t v = ldl_be_phys(as, globals[i].addr);
            fprintf(stderr,
                    "SEM-GLOBALS: 0x%08x %-22s = 0x%08x\n",
                    globals[i].addr, globals[i].name, v);
        }
        fflush(stderr);
    }

    /*
     * Step 3 (gate-9 follow-up, plan 2026-05-07): tFecEndRx TCB watch
     * across the synth-doorbell window. Every tick from vt=8s to vt=14s,
     * dump TCB at 0x07bede38 — status (TCB+0x3C), pSemId (TCB+0x5C),
     * errno (TCB+0x84), saved PC (REG_SET at TCB+0x130, PC at +0x8C).
     * Tells us whether the wake reached the scheduler:
     *   status PEND throughout       — wake never made it (workQ?)
     *   status flickers PEND→READY   — wake reached scheduler then re-blocked
     *   errno changes                — semGive returned an error path
     */
    /*
     * Step 1 dump (gate-9 follow-up, plan 2026-05-07): one-shot SEM-HIST
     * at vt=15s. Diag-sampler's diag_max equals the QEMU run timeout, so
     * its end-of-run dump rarely fires; emit the histogram from here so
     * we always see it. Same content as the diag-end dump (see below).
     */
    if (tick_count == 60 * 15) {
        unsigned hits = 0;
        fprintf(stderr,
                "SEM-HIST: semGive+semFlush body samples "
                "(0x%08x..0x%08x) at vt=15s:\n",
                (unsigned)SEMGIVE_HIST_BASE, (unsigned)SEMGIVE_HIST_END);
        for (unsigned j = 0; j < SEMGIVE_HIST_SIZE; j++) {
            if (g_semgive_hist[j] == 0) continue;
            fprintf(stderr, "SEM-HIST:   0x%08x : %u\n",
                    (unsigned)(SEMGIVE_HIST_BASE + j * 4),
                    g_semgive_hist[j]);
            hits++;
        }
        fprintf(stderr, "SEM-HIST: %u distinct PCs sampled\n", hits);
        fflush(stderr);
    }

    if (tick_count >= 60 * 8 && tick_count <= 60 * 90 && (tick_count % 30) == 0) {
        AddressSpace *as = &address_space_memory;
        static const struct { uint32_t tcb; const char *name; } watchlist[] = {
            { 0x07bede38, "tFecEndRx" },
            { 0x07fefe00, "tRootTask" },  /* Phase A coords */
        };
        for (int w = 0; w < (int)ARRAY_SIZE(watchlist); w++) {
            const uint32_t tcb = watchlist[w].tcb;
            uint32_t status = ldl_be_phys(as, tcb + 0x3C);
            uint32_t pSemId = ldl_be_phys(as, tcb + 0x5C);
            uint32_t errnov = ldl_be_phys(as, tcb + 0x84);
            uint32_t pc     = ldl_be_phys(as, tcb + 0x130 + 0x8C);
            uint32_t lr     = ldl_be_phys(as, tcb + 0x130 + 0x84);
            const char *st = "?";
            switch (status) {
            case 0x0:     st = "READY"; break;
            case 0x2:     st = "PEND"; break;
            case 0x4:     st = "DELAY"; break;
            case 0x6:     st = "PEND+TO"; break;
            case 0x10000: st = "SUSP"; break;
            }
            fprintf(stderr,
                    "TCB-WATCH: t=%d.%02ds %-9s tcb=0x%08x status=0x%-5x %-7s "
                    "sem=0x%08x errno=0x%08x PC=0x%08x LR=0x%08x\n",
                    tick_count / 60, (tick_count % 60) * 100 / 60,
                    watchlist[w].name, tcb, status, st, pSemId, errnov, pc, lr);
        }
        fflush(stderr);
    }

    /*
     * CASCADE-PROBE (plan 2026-05-11): direct per-task read of TCB.status +
     * saved PC for the 5 tasks the previous run's task-list dumper showed
     * flipping PEND→READY after our READYQ-FORCE at vt=10s. Plan question:
     * is the cascade real, or did our forced readyQ writes corrupt their
     * TCB.status without changing their actual PEND state? Cross-check
     * against the task-list dumper at the same vt — if they disagree, the
     * task-list reader has a bug; if they agree on READY but saved PC is
     * still 0x002fe918 (sem-block), the tasks are inconsistent (READY
     * status but blocked-PC, would never run forward).
     *
     * Also dumps readyQ.first.key — if it flips back to 0xff (CpuloadLow's
     * idle key) between vt=15..25s, the kernel is undoing our READYQ-FORCE
     * write on every context switch.
     *
     * Fires once each at vt=15s, 20s, 25s. Saved-PC offset = TCB+0x130+0x8C
     * matches the existing task-list dumper.
     */
    if (tick_count == 60 * 15 || tick_count == 60 * 20 ||
        tick_count == 60 * 25) {
        AddressSpace *as = &address_space_memory;
        static const struct { uint32_t tcb; const char *name; } cascade[] = {
            { 0x07bede38, "tFecEndRx       (orig wake target)" },
            { 0x07fc1f30, "tNetTask                          " },
            { 0x07ccb5f0, "tFecEndRecover                    " },
            { 0x07fcbec8, "confLogMsg                        " },
            { 0x07fc9f10, "tLed                              " },
            { 0x07bc8fa0, "getFactoryVerison                 " },
        };
        const uint32_t SEM_BLOCK_PC = 0x002fe918;
        const uint32_t READYQ = 0x0099af58;
        uint32_t rq_first   = ldl_be_phys(as, READYQ + 0x00);
        uint32_t rq_first_k = rq_first ? ldl_be_phys(as, rq_first + 0x08) : 0;
        uint32_t rq_bmap    = ldl_be_phys(as, READYQ + 0x04);
        uint32_t rq_bmap_w0 = rq_bmap ? ldl_be_phys(as, rq_bmap + 0x00) : 0;
        fprintf(stderr,
                "CASCADE-PROBE: t=%ds  readyQ.first=0x%08x first.key=0x%02x "
                "bmap[0]=0x%08x\n",
                tick_count / 60, rq_first, rq_first_k & 0xff, rq_bmap_w0);
        for (int i = 0; i < (int)ARRAY_SIZE(cascade); i++) {
            uint32_t tcb    = cascade[i].tcb;
            uint32_t status = ldl_be_phys(as, tcb + 0x3C);
            uint32_t prio   = ldl_be_phys(as, tcb + 0x40);
            uint32_t pSemId = ldl_be_phys(as, tcb + 0x5C);
            uint32_t pc     = ldl_be_phys(as, tcb + 0x130 + 0x8C);
            uint32_t lr     = ldl_be_phys(as, tcb + 0x130 + 0x84);
            const char *st = "?";
            switch (status) {
            case 0x0:     st = "READY"; break;
            case 0x2:     st = "PEND"; break;
            case 0x4:     st = "DELAY"; break;
            case 0x6:     st = "PEND+TO"; break;
            case 0x10000: st = "SUSP"; break;
            }
            const char *pc_tag = (pc == SEM_BLOCK_PC) ? "[SEM-BLOCK]" :
                                 (status == 0x0)     ? "[runnable] " :
                                                       "[other]    ";
            fprintf(stderr,
                    "CASCADE-PROBE:   %s tcb=0x%08x status=0x%x %-7s "
                    "prio=%3u sem=0x%08x PC=0x%08x %s LR=0x%08x\n",
                    cascade[i].name, tcb, status, st, prio, pSemId, pc,
                    pc_tag, lr);
        }
        fflush(stderr);
    }

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

            /*
             * For tRootTask in PEND state: walk back-chain from saved SP
             * to dump the call chain. PowerPC EABI: sp[0] = caller's SP,
             * caller's SP[1] (i.e. *(caller_sp+4)) = saved LR of this
             * frame. This reveals which usrRoot bl call we're stuck in.
             */
            if (status == 0x2 && nm[0] == 't' && nm[1] == 'R' &&
                sp >= 0x07000000 && sp < 0x08000000) {
                uint32_t fp = sp;
                fprintf(stderr, "       stack chain (back-chain LR walk):\n");
                for (int f = 0; f < 24 && fp >= 0x07000000 &&
                                 fp < 0x08000000; f++) {
                    uint32_t next  = ldl_be_phys(as, fp);
                    uint32_t lr_at = ldl_be_phys(as, fp + 4);
                    fprintf(stderr,
                            "         [%2d] sp=0x%08x lr=0x%08x\n",
                            f, fp, lr_at);
                    if (next == 0 || next == fp || next < fp) {
                        break;
                    }
                    fp = next;
                }
            }
            cur = ldl_be_phys(as, cur);  /* DLL_NODE.next */
        }
        fflush(stderr);
    }

    timer_mod(s->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667ULL); /* ~60 Hz */
}

/* === FS-hook host-side helpers (Phase 4b) === */

static const char *fshook_root(void)
{
    static const char *cached;
    if (!cached) {
        const char *env = getenv("MPC5200_FS_ROOT");
        cached = env ? env :
            "/home/kasper/Vestas/bin/data_dump/firedrake/turbine_dump_node10_roye2/fs";
    }
    return cached;
}

/* Read a NUL-terminated guest string from physical memory. Returns false on
 * truncation (caller's buffer too small). */
static bool fshook_read_guest_path(uint32_t guest_va, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    for (size_t i = 0; i < cap; i++) {
        uint8_t b = 0;
        cpu_physical_memory_read(guest_va + i, &b, 1);
        out[i] = (char)b;
        if (b == 0) {
            return true;
        }
    }
    out[cap - 1] = 0;
    return false;
}

/* Translate "/fs" or "/fs/..." -> "<root>" or "<root>/...". Reject anything
 * else so non-/fs/ paths fail like a real "no FS mounted" environment. */
static bool fshook_translate(const char *vx, char *host, size_t cap)
{
    if (vx[0] != '/' || vx[1] != 'f' || vx[2] != 's') {
        return false;
    }
    if (vx[3] != '\0' && vx[3] != '/') {
        return false;
    }
    int n = snprintf(host, cap, "%s%s", fshook_root(), vx + 3);
    return n > 0 && (size_t)n < cap;
}

static int fshook_alloc_slot(MPC5200State *s)
{
    for (int i = 0; i < FSHOOK_NUM_FDS; i++) {
        if (!s->fshook.fd_proxy[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int fshook_lookup_slot(MPC5200State *s, uint32_t fake_fd)
{
    if (fake_fd < FSHOOK_FAKE_FD_BASE) {
        return -1;
    }
    int i = (int)(fake_fd - FSHOOK_FAKE_FD_BASE);
    if (i >= FSHOOK_NUM_FDS) {
        return -1;
    }
    if (!s->fshook.fd_proxy[i].in_use) {
        return -1;
    }
    return i;
}

static void fshook_handle_open(MPC5200State *s)
{
    char vx_path[256];
    char host_path[512];

    if (!fshook_read_guest_path(s->fshook.arg0, vx_path, sizeof(vx_path))) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = EINVAL;
        fprintf(stderr, "FS_HOOK: open(<bad ptr 0x%08x>) -> -1\n",
                s->fshook.arg0);
        fflush(stderr);
        return;
    }
    if (!fshook_translate(vx_path, host_path, sizeof(host_path))) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = ENOENT;
        fprintf(stderr,
                "FS_HOOK: open(\"%s\") -> -1 (path not under /fs/)\n",
                vx_path);
        fflush(stderr);
        return;
    }
    int slot = fshook_alloc_slot(s);
    if (slot < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = EMFILE;
        fprintf(stderr, "FS_HOOK: open(\"%s\") -> -1 (no free slot)\n",
                vx_path);
        fflush(stderr);
        return;
    }
    int hfd = open(host_path, O_RDONLY);
    if (hfd < 0) {
        int saved = errno;
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = saved;
        fprintf(stderr,
                "FS_HOOK: open(\"%s\") -> -1 (host \"%s\": %s)\n",
                vx_path, host_path, strerror(saved));
        fflush(stderr);
        return;
    }
    s->fshook.fd_proxy[slot].host_fd = hfd;
    s->fshook.fd_proxy[slot].in_use  = true;
    uint32_t fake = FSHOOK_FAKE_FD_BASE + slot;
    s->fshook.result = fake;
    s->fshook.err    = 0;
    fprintf(stderr,
            "FS_HOOK: open(\"%s\") -> fake_fd=%u host_fd=%d  (host=\"%s\")\n",
            vx_path, fake, hfd, host_path);
    fflush(stderr);
}

static void fshook_handle_read(MPC5200State *s)
{
    uint32_t fake_fd = s->fshook.arg0;
    uint32_t buf_va  = s->fshook.arg1;
    uint32_t n       = s->fshook.arg2;
    int slot = fshook_lookup_slot(s, fake_fd);
    if (slot < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = EBADF;
        fprintf(stderr, "FS_HOOK: read(fd=%u, ...) -> -1 (not our fd)\n",
                fake_fd);
        fflush(stderr);
        return;
    }
    if (n > 4096) {
        n = 4096;     /* host-side cap; BSP can re-call for more */
    }
    uint8_t buf[4096];
    ssize_t got = read(s->fshook.fd_proxy[slot].host_fd, buf, n);
    if (got < 0) {
        int saved = errno;
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = saved;
        fprintf(stderr, "FS_HOOK: read(fd=%u, n=%u) -> -1 (%s)\n",
                fake_fd, n, strerror(saved));
        fflush(stderr);
        return;
    }
    if (got > 0) {
        cpu_physical_memory_write(buf_va, buf, got);
    }
    s->fshook.result = (uint32_t)got;
    s->fshook.err    = 0;
    fprintf(stderr,
            "FS_HOOK: read(fd=%u, buf=0x%08x, n=%u) -> %zd\n",
            fake_fd, buf_va, n, got);
    fflush(stderr);
}

static void fshook_handle_close(MPC5200State *s)
{
    uint32_t fake_fd = s->fshook.arg0;
    int slot = fshook_lookup_slot(s, fake_fd);
    if (slot < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = EBADF;
        fprintf(stderr, "FS_HOOK: close(fd=%u) -> -1 (not our fd)\n",
                fake_fd);
        fflush(stderr);
        return;
    }
    int hfd = s->fshook.fd_proxy[slot].host_fd;
    int rc = close(hfd);
    int saved = errno;
    s->fshook.fd_proxy[slot].in_use  = false;
    s->fshook.fd_proxy[slot].host_fd = -1;
    s->fshook.result = (rc == 0) ? 0 : (uint32_t)-1;
    s->fshook.err    = (rc == 0) ? 0 : saved;
    fprintf(stderr, "FS_HOOK: close(fd=%u host_fd=%d) -> %d\n",
            fake_fd, hfd, rc);
    fflush(stderr);
}

static void fshook_handle_lseek(MPC5200State *s)
{
    uint32_t fake_fd = s->fshook.arg0;
    int32_t  off     = (int32_t)s->fshook.arg1;
    uint32_t whence  = s->fshook.arg2;
    int slot = fshook_lookup_slot(s, fake_fd);
    if (slot < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = EBADF;
        return;
    }
    off_t pos = lseek(s->fshook.fd_proxy[slot].host_fd, off, whence);
    if (pos < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = errno;
    } else {
        s->fshook.result = (uint32_t)pos;
        s->fshook.err    = 0;
    }
    fprintf(stderr, "FS_HOOK: lseek(fd=%u, off=%d, w=%u) -> %lld\n",
            fake_fd, off, whence, (long long)pos);
    fflush(stderr);
}

/* Stub fstat: writes only st_size into the guest stat buffer. VxWorks 5.5
 * struct stat layout puts st_size at offset 0x20; rest left undisturbed
 * (callers must zero-init beforehand or accept stale fields). */
static void fshook_handle_fstat(MPC5200State *s)
{
    uint32_t fake_fd = s->fshook.arg0;
    uint32_t out_va  = s->fshook.arg1;
    int slot = fshook_lookup_slot(s, fake_fd);
    if (slot < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = EBADF;
        return;
    }
    struct stat st;
    if (fstat(s->fshook.fd_proxy[slot].host_fd, &st) < 0) {
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = errno;
        return;
    }
    stl_be_phys(&address_space_memory, out_va + 0x20, (uint32_t)st.st_size);
    s->fshook.result = 0;
    s->fshook.err    = 0;
    fprintf(stderr, "FS_HOOK: fstat(fd=%u) -> size=%u\n",
            fake_fd, (uint32_t)st.st_size);
    fflush(stderr);
}

/* exists: translate guest path to host, run access(F_OK), return 1 on hit
 * and 0 on miss. Mirrors fshook_handle_open's path-validation path; never
 * allocates an fd slot. The BSP body at 0x13ffe4 used fopen() for the
 * same purpose; access() is cheaper and gives the BSP an honest miss when
 * the host file is absent (e.g. /fs/etc/force_safe_mode). */
static void fshook_handle_exists(MPC5200State *s)
{
    char vx_path[256];
    char host_path[512];

    if (!fshook_read_guest_path(s->fshook.arg0, vx_path, sizeof(vx_path))) {
        s->fshook.result = 0;
        s->fshook.err    = EINVAL;
        fprintf(stderr, "FS_HOOK: exists(<bad ptr 0x%08x>) -> 0\n",
                s->fshook.arg0);
        fflush(stderr);
        return;
    }
    if (!fshook_translate(vx_path, host_path, sizeof(host_path))) {
        s->fshook.result = 0;
        s->fshook.err    = ENOENT;
        fprintf(stderr,
                "FS_HOOK: exists(\"%s\") -> 0 (path not under /fs/)\n",
                vx_path);
        fflush(stderr);
        return;
    }
    int rc = access(host_path, F_OK);
    if (rc == 0) {
        s->fshook.result = 1;
        s->fshook.err    = 0;
        fprintf(stderr, "FS_HOOK: exists(\"%s\") -> 1  (host=\"%s\")\n",
                vx_path, host_path);
    } else {
        int saved = errno;
        s->fshook.result = 0;
        s->fshook.err    = saved;
        fprintf(stderr,
                "FS_HOOK: exists(\"%s\") -> 0  (host=\"%s\": %s)\n",
                vx_path, host_path, strerror(saved));
    }
    fflush(stderr);
}

static void fshook_dispatch(MPC5200State *s, uint32_t cmd)
{
    switch (cmd) {
    case FS_OPEN:   fshook_handle_open(s);   return;
    case FS_READ:   fshook_handle_read(s);   return;
    case FS_CLOSE:  fshook_handle_close(s);  return;
    case FS_LSEEK:  fshook_handle_lseek(s);  return;
    case FS_FSTAT:  fshook_handle_fstat(s);  return;
    case FS_EXISTS: fshook_handle_exists(s); return;
    default:
        s->fshook.result = (uint32_t)-1;
        s->fshook.err    = ENOSYS;
        fprintf(stderr, "FS_HOOK: unknown cmd=%u\n", cmd);
        fflush(stderr);
        return;
    }
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

    /* FS-hook doorbell read-back (Phase 4b): result/errno only. */
    if (offset >= FSHOOK_OFFSET && offset < FSHOOK_OFFSET + FSHOOK_REG_SIZE) {
        switch (offset - FSHOOK_OFFSET) {
        case FSHOOK_REG_RESULT:    return s->fshook.result;
        case FSHOOK_REG_ERRNO:     return s->fshook.err;
        case FSHOOK_REG_ARG0:      return s->fshook.arg0;
        case FSHOOK_REG_ARG1:      return s->fshook.arg1;
        case FSHOOK_REG_ARG2:      return s->fshook.arg2;
        case FSHOOK_REG_SEM_QUEUE: return s->fshook.sem_queue;
        case FSHOOK_REG_NETJOB_FUNC: {
            static unsigned shim_reads;
            static unsigned last_bucket = 0xFFFFFFFFu;
            shim_reads++;
            unsigned bucket = shim_reads / 60;
            if (bucket != last_bucket) {
                int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                fprintf(stderr,
                        "SHIM-FIRE: bucket=%u (%u reads) vt_ns=%lld\n",
                        bucket, shim_reads, (long long)ns);
                fflush(stderr);
                last_bucket = bucket;
            }
            return s->fshook.netjob_func;
        }
        case FSHOOK_REG_NETJOB_ARG1: return s->fshook.netjob_args[0];
        case FSHOOK_REG_NETJOB_ARG2: return s->fshook.netjob_args[1];
        case FSHOOK_REG_NETJOB_ARG3: return s->fshook.netjob_args[2];
        case FSHOOK_REG_NETJOB_ARG4: return s->fshook.netjob_args[3];
        case FSHOOK_REG_NETJOB_ARG5: return s->fshook.netjob_args[4];
        case FSHOOK_REG_NETJOB_PROBE: return s->fshook.netjob_probe;
        default:                   return 0;
        }
    }

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
        uint32_t v = 0;
        const char *src = "none";
        if (s->ic_fec_pending) {
            v = 0x25240000;
            src = "FEC";
        } else if (s->ic_sdma_pending) {
            v = 0x20000000;
            src = "SDMA";
        } else if (s->ic_pending) {
            /* SLT1 timer — legacy path; read-to-clear. */
            s->ic_pending = false;
            mpc5200_update_ext(s);
            v = 0x40000000;
            src = "SLT1";
        }
        static unsigned pst_log = 0;
        /* Always log non-zero non-SLT reads (FEC + SDMA dispatch),
         * but rate-limit SLT to avoid swamping. */
        bool log_it = (v != 0 && strcmp(src, "SLT1") != 0)
                      || (pst_log < 64);
        if (log_it && pst_log < 4096) {
            fprintf(stderr, "IC 0x524 read => 0x%08x (%s) "
                    "(fec=%d sdma=%d slt=%d)\n",
                    v, src, s->ic_fec_pending, s->ic_sdma_pending,
                    s->ic_pending);
            fflush(stderr);
        }
        pst_log++;
        return v;
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
        /*
         * BSP allocated skb buffers (data field is set to a DRAM addr) but
         * never wrote BCOM_BD_READY. The kernel task tFecEndRx that would
         * arm the ring is PEND'd at PC=0x2fe918 from boot — so the ring
         * stays half-initialised and we drop every inbound frame.
         *
         * Workaround (Phase 2.3): if skb_pa points into DRAM, treat the BD
         * as engine-owned and deliver the frame anyway. The frame write +
         * status (L | len) + raised RXF IRQ should wake tFecEndRx, which
         * will then re-arm the ring properly going forward.
         */
        bool skb_valid = (skb_pa >= 0x00100000 && skb_pa < 0x10000000);
        if (!skb_valid) {
            static unsigned drop_log = 0;
            if (drop_log++ < 8) {
                fprintf(stderr,
                        "BestComm RX: BD[start=0x%08x status=0x%08x skb=0x%08x] "
                        "not READY, no valid skb, dropping %zu B "
                        "(base=0x%08x last=0x%08x)\n",
                        bd_addr, status, skb_pa, len, bd_base, bd_last);
                fflush(stderr);
            }
            return;
        }
        static unsigned forge_log = 0;
        if (forge_log++ < 16) {
            fprintf(stderr,
                    "BestComm RX: BD[start=0x%08x] not READY but skb=0x%08x "
                    "valid — force-arming and delivering %zu B "
                    "(base=0x%08x last=0x%08x)\n",
                    bd_addr, skb_pa, len, bd_base, bd_last);
            fflush(stderr);
        }
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
     * BSP enables bit 3 in IntMask briefly during task init, then masks
     * it again before tFecEndRx ever runs (because tFecEndRx is PEND'd
     * waiting on a sem that's posted by the very IRQ we're trying to
     * deliver). To break the deadlock, also force-unmask bit 3 here so
     * the SDMA Main IRQ actually fires when a packet arrives.
     */
    uint32_t intp = mpc5200_bc_get32(s, 0x14);
    uint32_t mask = mpc5200_bc_get32(s, 0x18);
    mpc5200_bc_put32(s, 0x14, intp | BCOM_INTP_FEC_RX);
    if (mask & BCOM_INTP_FEC_RX) {
        mpc5200_bc_put32(s, 0x18, mask & ~BCOM_INTP_FEC_RX);
    }
    mpc5200_sdma_eval_irq(s);

    /*
     * Doorbell: tell the BSP to semGive(tFecEndRx_sem). The BSP's
     * sysClkInt tail-patch (~60Hz) reads FSHOOK_REG_SEM_QUEUE on each
     * tick and calls semGive when non-zero. Bypasses the MSR.EE=0 hold
     * on EXT dispatch (2026-05-05 finding) entirely.
     *
     * Sem ID 0x07bee080: tFecEndRx PEND target, observed across all
     * gate-9 dispatch experiments. Write is unconditional — if a prior
     * tick already took the previous queue value, we just queue another
     * give (semGive on a counting/binary sem is idempotent for our
     * purposes; tFecEndRx will wake on the first non-stale give).
     */
    s->fshook.sem_queue = 0x07bee080;

    /*
     * netJobAdd doorbell (plan 2026-05-11 step 4): wire RX-walker to also
     * enqueue a netJobAdd call. The deferred function (probe_addr 0x002acf80)
     * runs in netTask context (EE=1) and does:
     *   1. *(struct+848) = 8 (set the work-flag tFecEndRx checks on wake)
     *   2. write 0xCAFEBABE to NETJOB_PROBE MMIO (visibility)
     *   3. semGive(0x07bee080) (proper kernel wake of tFecEndRx)
     * This routes the wake through netTask's full kernel-context semGive,
     * so when tFecEndRx resumes from semTake, it sees flag=8 and runs its
     * main RX-processing path — picks up our just-delivered BD and TXes a
     * reply.
     *
     * Set unconditionally on every RX. If a prior arm hasn't been picked up
     * yet, the new value overrides (fine — same stub address).
     */
    s->fshook.netjob_func    = 0x002acf80;
    s->fshook.netjob_args[0] = 0;
    s->fshook.netjob_args[1] = 0;
    s->fshook.netjob_args[2] = 0;
    s->fshook.netjob_args[3] = 0;
    s->fshook.netjob_args[4] = 0;

    /*
     * READYQ-FORCE-RX (plan 2026-05-11 step 3): the wake-once at vt=10s
     * dispatched tFecEndRx ONCE (PC moved 0x002fe918 → 0x00175628 at
     * the post-windExit return point). After that, readyQ.first stays 0
     * and tFecEndRx never gets re-dispatched even though TCB.status is
     * READY — the kernel doesn't re-enqueue it when context-switching
     * out (our forced bmap state is inconsistent with the bucket FIFO).
     *
     * Brute-force on every RX-walker fire: re-write readyQ.first = TCB,
     * re-set bmap[0] bit 28, re-set bmap_byte[28] bit 2, also clear
     * TCB.status to READY (in case kernel set PEND between fires). This
     * keeps the task continuously dispatchable so each new frame has a
     * shot at being processed by the network stack.
     */
    {
        AddressSpace *as_force = &address_space_memory;
        const uint32_t TCB_FECRX = 0x07bede38;
        const uint32_t READYQ    = 0x0099af58;
        uint32_t rqbmap = ldl_be_phys(as_force, READYQ + 0x04);
        stl_be_phys(as_force, READYQ + 0x00, TCB_FECRX);
        if (rqbmap) {
            uint32_t b0 = ldl_be_phys(as_force, rqbmap + 0x00);
            stl_be_phys(as_force, rqbmap + 0x00, b0 | (1u << 28));
            uint32_t byte_off = rqbmap + 4 + 28;
            uint8_t b;
            cpu_physical_memory_read(byte_off, &b, 1);
            b |= (1u << 2);
            cpu_physical_memory_write(byte_off, &b, 1);
        }
        stl_be_phys(as_force, TCB_FECRX + 0x3C, 0);  /* status = READY */
        static unsigned force_log = 0;
        if (force_log++ < 8) {
            fprintf(stderr,
                    "READYQ-FORCE-RX: post-RX dispatch force #%u (skb=0x%08x)\n",
                    force_log, skb_pa);
            fflush(stderr);
        }
    }
}

/* Forward decl for the FEC -> BestComm RX hook installer. */
void mpc5200_fec_set_rx_hook(void (*hook)(const uint8_t *, size_t));

static void mpc5200_mmio_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    MPC5200State *s = opaque;

    /* FS-hook doorbell write (Phase 4b): writing to REG_CMD triggers the
     * dispatcher; arg/result/errno slots are plain storage. */
    if (offset >= FSHOOK_OFFSET && offset < FSHOOK_OFFSET + FSHOOK_REG_SIZE) {
        uint32_t v = (uint32_t)value;
        switch (offset - FSHOOK_OFFSET) {
        case FSHOOK_REG_CMD:    fshook_dispatch(s, v); return;
        case FSHOOK_REG_ARG0:   s->fshook.arg0   = v;  return;
        case FSHOOK_REG_ARG1:   s->fshook.arg1   = v;  return;
        case FSHOOK_REG_ARG2:   s->fshook.arg2   = v;  return;
        case FSHOOK_REG_RESULT: s->fshook.result = v;  return;
        case FSHOOK_REG_ERRNO:  s->fshook.err    = v;  return;
        case FSHOOK_REG_TRACE: {
            char buf[256];
            if (fshook_read_guest_path(v, buf, sizeof(buf))) {
                fprintf(stderr, "FS_HOOK: TRACE \"%s\" (NIP=0x%08x LR=0x%08x)\n",
                        buf, (unsigned)s->cpu->env.nip,
                        (unsigned)s->cpu->env.lr);
            } else {
                fprintf(stderr, "FS_HOOK: TRACE <bad ptr 0x%08x>\n", v);
            }
            fflush(stderr);
            return;
        }
        case FSHOOK_REG_SEM_QUEUE: {
            /* BSP-side sysClkInt tail patch reads this, calls semGive on a
             * non-zero value, and writes 0 back to clear. Logged once per
             * non-zero queue + once per BSP clear so we can correlate with
             * tFecEndRx wakes. */
            static unsigned sq_log = 0;
            if (sq_log++ < 64) {
                fprintf(stderr, "FS_HOOK: SEM_QUEUE write 0x%08x "
                        "(NIP=0x%08x LR=0x%08x)\n",
                        v, (unsigned)s->cpu->env.nip,
                        (unsigned)s->cpu->env.lr);
                fflush(stderr);
            }
            s->fshook.sem_queue = v;
            return;
        }
        case FSHOOK_REG_NETJOB_FUNC: s->fshook.netjob_func    = v;  return;
        case FSHOOK_REG_NETJOB_ARG1: s->fshook.netjob_args[0] = v;  return;
        case FSHOOK_REG_NETJOB_ARG2: s->fshook.netjob_args[1] = v;  return;
        case FSHOOK_REG_NETJOB_ARG3: s->fshook.netjob_args[2] = v;  return;
        case FSHOOK_REG_NETJOB_ARG4: s->fshook.netjob_args[3] = v;  return;
        case FSHOOK_REG_NETJOB_ARG5: s->fshook.netjob_args[4] = v;  return;
        case FSHOOK_REG_NETJOB_PROBE: {
            /* Guest probe stub at 0x002acfc0 writes 0xCAFEBABE here when
             * netTask deferred-invokes it via netJobAdd. Confirms the call
             * actually reached the function. */
            s->fshook.netjob_probe = v;
            fprintf(stderr, "NETJOB-PROBE: flag flipped, value=0x%08x "
                    "(NIP=0x%08x LR=0x%08x)\n",
                    v, (unsigned)s->cpu->env.nip,
                    (unsigned)s->cpu->env.lr);
            fflush(stderr);
            return;
        }
        default:                return;
        }
    }

    /*
     * IC register write: ack legacy SLT path. We deliberately do NOT
     * clear ic_fec_pending or ic_sdma_pending here — those are level-
     * sensitive sources that clear when their underlying IRQ register
     * (FEC EIR / SDMA IntPending) is acked by the BSP. Spurious clears
     * would silently drop FEC TX/RX completion IRQs.
     *
     * Per-offset logging (plan 2026-05-05 Phase 0.3): we currently
     * swallow PerMask (0x500), MainMask (0x510), CritMask (0x520)
     * silently. If the BSP wrote a non-zero mask there it could be
     * gating SDMA dispatch. Log the first 64 writes per offset so we
     * see which registers the BSP actually touches.
     */
    if (offset >= 0x0500 && offset <= 0x052c) {
        static unsigned ic_w_log[0x30 / 4];
        unsigned slot = (offset - 0x500) / 4;
        if (slot < ARRAY_SIZE(ic_w_log) && ic_w_log[slot]++ < 64) {
            const char *name = "?";
            switch (offset) {
            case 0x500: name = "PerMask";      break;
            case 0x504: name = "PerPri+Main";  break;
            case 0x508: name = "MainPri1";     break;
            case 0x50c: name = "MainPri2";     break;
            case 0x510: name = "MainMask";     break;
            case 0x514: name = "MainEnStat";   break;
            case 0x518: name = "Crit*";        break;
            case 0x51c: name = "Critx";        break;
            case 0x520: name = "PerEnable";    break;
            case 0x524: name = "PerStatEnc";   break;
            case 0x528: name = "PerErrSt";     break;
            case 0x52c: name = "MainStatEnc";  break;
            }
            fprintf(stderr,
                    "IC W +0x%03x (%s) = 0x%08x sz=%u  NIP=0x%08x LR=0x%08x\n",
                    (unsigned)offset, name, (unsigned)value, size,
                    (unsigned)s->cpu->env.nip, (unsigned)s->cpu->env.lr);
            fflush(stderr);
        }
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
