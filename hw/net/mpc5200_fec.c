/*
 * Freescale MPC5200 Fast Ethernet Controller (FEC) device model.
 *
 * Forked from hw/net/imx_fec.c. The MPC5200 FEC shares register
 * lineage with the i.MX FEC, but with a critical architectural
 * difference: per the MPC5200 user manual section 14.1,
 *
 *   "BestComm data transfers are interrupt driven. Interrupt driven
 *    data movement from the processor is not supported."
 *
 * That means the FEC itself owns no buffer-descriptor rings — there
 * are no RDSR/TDSR registers. Frame data flows between system
 * memory and the FEC's 1 KiB Tx/Rx FIFOs via BestComm DMA tasks.
 * This device-model therefore implements:
 *
 *   - The CSR (control/status) register file (MBAR+0x3000..0x3144)
 *   - The MII management interface (MMFR/MSCR) using the LAN9118
 *     PHY model as a stand-in
 *   - The Tx/Rx FIFO data registers (MBAR+0x3184/0x31A4) — used as
 *     a mailbox by the (separately-modeled) BestComm executor
 *   - A single sysbus IRQ output, asserted on EIR & EIMR
 *   - The standard QEMU NIC integration (`qemu_send_packet`,
 *     `.receive` callback) — but the data path is gated on having
 *     a BestComm executor wired up.
 *
 * Without a BestComm executor, this device will see no packet flow:
 * TX writes to the Tx FIFO never get drained into a frame, and
 * incoming RX frames in the .receive callback are dropped because
 * there's no DMA task to ferry them to the host. That's deliberate
 * scope — see PLAN_Kasper.md.
 *
 * Copyright (c) 2026 Anemo Analytics
 * Based on imx_fec.c, Copyright (c) 2013 Jean-Christophe Dubois.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/net/lan9118_phy.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_MPC5200_FEC "mpc5200-fec"
OBJECT_DECLARE_SIMPLE_TYPE(MPC5200FECState, MPC5200_FEC)

/* MMIO region size — covers MBAR+0x3000..0x33FF (1 KiB). */
#define MPC5200_FEC_MMIO_SIZE 0x400

/*
 * Register byte offsets (relative to MBAR+0x3000). Only registers we
 * either expect the BSP to touch, or which have well-defined reset
 * values, are explicitly named. Any unmapped offset reads as 0 and
 * writes are stored verbatim.
 */
#define FEC_FEC_ID          0x000   /* FEC ID */
#define FEC_EIR             0x004   /* Interrupt Event */
#define FEC_EIMR            0x008   /* Interrupt Mask */
#define FEC_RDAR            0x010   /* Rx Descriptor Active */
#define FEC_TDAR            0x014   /* Tx Descriptor Active */
#define FEC_ECR             0x024   /* Ethernet Control */
#define FEC_MMFR            0x040   /* MII Management Frame */
#define FEC_MSCR            0x044   /* MII Speed Control */
#define FEC_MIBC            0x064   /* MIB Control */
#define FEC_R_CNTRL         0x084   /* Receive Control */
#define FEC_R_HASH          0x088   /* Hash */
#define FEC_X_CNTRL         0x0C4   /* Tx Control */
#define FEC_PALR            0x0E4   /* Physical Address Low (MAC[0..3]) */
#define FEC_PAUR            0x0E8   /* Physical Address High (MAC[4..5]) */
#define FEC_OP_PAUSE        0x0EC   /* Opcode/Pause Duration */
#define FEC_IADDR1          0x118   /* Individual Address 1 (MAC filter) */
#define FEC_IADDR2          0x11C   /* Individual Address 2 */
#define FEC_GADDR1          0x120   /* Group Address 1 (multicast) */
#define FEC_GADDR2          0x124   /* Group Address 2 */
#define FEC_X_WMRK          0x144   /* Tx FIFO Watermark */
#define FEC_RFIFO_DATA      0x184   /* Rx FIFO Data */
#define FEC_RFIFO_STATUS    0x188   /* Rx FIFO Status */
#define FEC_RFIFO_CNTRL     0x18C   /* Rx FIFO Control */
#define FEC_RFIFO_LRF_PTR   0x190   /* Rx FIFO Last Read Frame Ptr */
#define FEC_RFIFO_LWF_PTR   0x194   /* Rx FIFO Last Write Frame Ptr */
#define FEC_RFIFO_ALARM     0x198   /* Rx FIFO Alarm Ptr */
#define FEC_RFIFO_RDPTR     0x19C   /* Rx FIFO Read Ptr */
#define FEC_RFIFO_WRPTR     0x1A0   /* Rx FIFO Write Ptr */
#define FEC_TFIFO_DATA      0x1A4   /* Tx FIFO Data */
#define FEC_TFIFO_STATUS    0x1A8   /* Tx FIFO Status */
#define FEC_TFIFO_CNTRL     0x1AC   /* Tx FIFO Control */
#define FEC_TFIFO_LRF_PTR   0x1B0   /* Tx FIFO Last Read Frame Ptr */
#define FEC_TFIFO_LWF_PTR   0x1B4   /* Tx FIFO Last Write Frame Ptr */
#define FEC_TFIFO_ALARM     0x1B8   /* Tx FIFO Alarm Ptr */
#define FEC_TFIFO_RDPTR     0x1BC   /* Tx FIFO Read Ptr */
#define FEC_TFIFO_WRPTR     0x1C0   /* Tx FIFO Write Ptr */

/*
 * Number of registers stored as plain word storage. Registers that
 * have side effects on read/write are handled in the dispatch
 * functions; everything else just round-trips through `regs[]`.
 *
 * MMIO access is always 4-byte aligned, so we index by `offset/4`.
 */
#define FEC_REG_COUNT (MPC5200_FEC_MMIO_SIZE / 4)

/* ECR bits */
#define ECR_RESET   (1U << 0)
#define ECR_ETHER_EN (1U << 1)

/* EIR / EIMR bits we model */
#define EIR_HBERR   (1U << 31)
#define EIR_BABR    (1U << 30)
#define EIR_BABT    (1U << 29)
#define EIR_GRA     (1U << 28)
#define EIR_TXF     (1U << 27)
#define EIR_TXB     (1U << 26)
#define EIR_RXF     (1U << 25)
#define EIR_RXB     (1U << 24)
#define EIR_MII     (1U << 23)
#define EIR_EBERR   (1U << 22)
#define EIR_LC      (1U << 21)
#define EIR_RL      (1U << 20)
#define EIR_UN      (1U << 19)

/* MMFR — MII Management Frame */
#define MMFR_ST_SHIFT   30  /* start of frame */
#define MMFR_OP_SHIFT   28  /* opcode: 1=write 2=read */
#define MMFR_PA_SHIFT   23  /* PHY address (5 bits) */
#define MMFR_RA_SHIFT   18  /* register address (5 bits) */
#define MMFR_TA_SHIFT   16  /* turnaround (2 bits) */
#define MMFR_DATA_MASK  0xFFFF

struct MPC5200FECState {
    SysBusDevice parent_obj;

    NICState  *nic;
    NICConf    conf;
    qemu_irq   irq;
    MemoryRegion iomem;

    /* Backing storage for register reads/writes that have no side effect. */
    uint32_t regs[FEC_REG_COUNT];

    /*
     * Minimal IEEE 802.3 clause-22 PHY model.
     *
     * The Vestas BSP's m5200Fec driver does not check vendor PHY IDs (no
     * reads of PHYID1/PHYID2 in the init path — confirmed by binary
     * disassembly). It only requires:
     *   - BMCR low 6 bits non-zero on probe (so a "PHY present" check
     *     passes via `andi. r9, r9, 0x3f`)
     *   - BMCR writes preserved fully — including ISOLATE bit, which the
     *     BSP writes then polls until it reads back. This is why we model
     *     PHY directly rather than reuse hw/net/lan9118_phy.c whose write
     *     mask drops ISOLATE.
     *   - BMSR with link-up + AN_COMPLETE so the basic-check loop exits.
     *
     * Other registers (PHYID, AN_ADV, AN_LP, AN_EXP) are still served by
     * the embedded lan9118_phy_read for compatibility — the BSP only
     * reads them for logging.
     */
    uint16_t phy_bmcr;       /* Last value written to BMCR (reg 0) */
    uint16_t phy_bmsr;       /* Status: link up + AN done + capabilities */
    Lan9118PhyState mii;     /* For non-BMCR/BMSR PHY register access */
    IRQState mii_irq;
    uint8_t  phy_addr;       /* PHY address; BSP probes 0..31 looking for one */
};

static void mpc5200_fec_update_irq(MPC5200FECState *s)
{
    bool active = (s->regs[FEC_EIR / 4] & s->regs[FEC_EIMR / 4]) != 0;
    qemu_set_irq(s->irq, active);
}

/*
 * Public helper for the BestComm executor stub in mac_newworld.c.
 * Lets external code OR bits into EIR (TXF, RXF, etc.) and
 * re-evaluate the IRQ line in one call.
 */
void mpc5200_fec_raise_eir(DeviceState *dev, uint32_t bits);
void mpc5200_fec_raise_eir(DeviceState *dev, uint32_t bits)
{
    MPC5200FECState *s = MPC5200_FEC(dev);
    s->regs[FEC_EIR / 4] |= bits;
    mpc5200_fec_update_irq(s);
}

/* Same shape but lets the executor send a frame on the wire. */
void mpc5200_fec_send_packet(DeviceState *dev, const uint8_t *buf, size_t len);
void mpc5200_fec_send_packet(DeviceState *dev, const uint8_t *buf, size_t len)
{
    MPC5200FECState *s = MPC5200_FEC(dev);
    if (s->nic && qemu_get_queue(s->nic)) {
        qemu_send_packet(qemu_get_queue(s->nic), buf, len);
    }
}

/*
 * MII Management Frame: writing MMFR triggers a PHY transaction.
 *   ST(2) | OP(2) | PA(5) | RA(5) | TA(2) | DATA(16)
 * OP = 01 = write, OP = 10 = read.
 *
 * On completion we set EIR.MII (bit 23) and replace MMFR.DATA with
 * the read result.
 */
static void mpc5200_fec_mmfr_write(MPC5200FECState *s, uint32_t value)
{
    unsigned op  = (value >> MMFR_OP_SHIFT) & 0x3;
    unsigned pa  = (value >> MMFR_PA_SHIFT) & 0x1F;
    unsigned ra  = (value >> MMFR_RA_SHIFT) & 0x1F;
    uint16_t data = value & MMFR_DATA_MASK;

    s->regs[FEC_MMFR / 4] = value;

    /*
     * Respond as a single PHY at exactly s->phy_addr. If the BSP
     * scans multiple addresses it must see a single responding PHY —
     * replying everywhere confuses the scan and the BSP can't pick.
     */
    if (pa == s->phy_addr) {
        if (op == 1) { /* write */
            static unsigned phy_write_log = 0;
            if (phy_write_log++ < 32) {
                fprintf(stderr, "PHY W reg=%u data=0x%04x\n", ra, data);
                fflush(stderr);
            }
            switch (ra) {
            case 0: /* BMCR: store full value, including ISOLATE/PDOWN/etc */
                if (data & 0x8000) {
                    /* RESET (bit 15) self-clears: re-init to defaults */
                    s->phy_bmcr = 0x3101;
                } else {
                    s->phy_bmcr = data;
                }
                break;
            case 1: /* BMSR: read-only on real hardware; ignore writes */
                break;
            default:
                lan9118_phy_write(&s->mii, ra, data);
                break;
            }
        } else if (op == 2) { /* read */
            uint16_t r;
            switch (ra) {
            case 0: r = s->phy_bmcr; break;
            case 1: r = s->phy_bmsr; break;
            default: r = lan9118_phy_read(&s->mii, ra); break;
            }
            s->regs[FEC_MMFR / 4] = (value & ~(uint32_t)MMFR_DATA_MASK) | r;
        }
    } else {
        /* No PHY at that address — return all-ones on read. */
        if (op == 2) {
            s->regs[FEC_MMFR / 4] = (value & ~(uint32_t)MMFR_DATA_MASK)
                                  | 0xFFFF;
        }
    }

    s->regs[FEC_EIR / 4] |= EIR_MII;
    mpc5200_fec_update_irq(s);
}

static uint64_t mpc5200_fec_read(void *opaque, hwaddr offset, unsigned size)
{
    MPC5200FECState *s = opaque;
    unsigned idx = offset / 4;

    if (idx >= FEC_REG_COUNT) {
        return 0;
    }

    uint32_t v;
    switch (offset) {
    case FEC_FEC_ID:
        v = 0; /* Real silicon returns a constant ID; drivers ignore it. */
        break;
    default:
        v = s->regs[idx];
        break;
    }

    {
        static unsigned log_count = 0;
        /* Always log MMFR/EIR reads for diagnostics */
        if (offset == FEC_MMFR || offset == FEC_EIR) {
            if (log_count < 4000) {
                fprintf(stderr, "FEC R +0x%03x => 0x%08x\n",
                        (unsigned)offset, v);
                fflush(stderr);
                log_count++;
            }
        } else if (log_count++ < 100) {
            fprintf(stderr, "FEC R +0x%03x => 0x%08x\n",
                    (unsigned)offset, v);
            fflush(stderr);
        }
    }

    return v;
}

static void mpc5200_fec_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    MPC5200FECState *s = opaque;
    unsigned idx = offset / 4;
    uint32_t v = (uint32_t)value;

    if (idx >= FEC_REG_COUNT) {
        return;
    }

    {
        static unsigned log_count = 0;
        if (log_count++ < 100) {
            fprintf(stderr, "FEC W +0x%03x = 0x%08x (sz=%u)\n",
                    (unsigned)offset, v, size);
            fflush(stderr);
        }
    }

    switch (offset) {
    case FEC_EIR:
        /* EIR bits are W1C — write-1-to-clear. */
        s->regs[idx] &= ~v;
        mpc5200_fec_update_irq(s);
        break;

    case FEC_EIMR:
        s->regs[idx] = v;
        mpc5200_fec_update_irq(s);
        break;

    case FEC_ECR:
        s->regs[idx] = v;
        if (v & ECR_RESET) {
            /* Soft reset: drop ETHER_EN and clear all interrupt state. */
            s->regs[idx] &= ~ECR_ETHER_EN;
            s->regs[FEC_EIR / 4] = 0;
            mpc5200_fec_update_irq(s);
            /* Hardware self-clears RESET after one cycle. */
            s->regs[idx] &= ~ECR_RESET;
        }
        break;

    case FEC_MMFR:
        mpc5200_fec_mmfr_write(s, v);
        break;

    case FEC_TFIFO_DATA:
        /*
         * BestComm-driven Tx data path: writing a word into the Tx
         * FIFO. Without a BestComm executor we silently drop the
         * data. The plan is for the BestComm executor to: collect
         * writes here into a frame buffer, and on the watermark
         * trigger call qemu_send_packet().
         */
        s->regs[idx] = v;
        break;

    default:
        s->regs[idx] = v;
        break;
    }
}

static const MemoryRegionOps mpc5200_fec_ops = {
    .read  = mpc5200_fec_read,
    .write = mpc5200_fec_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static bool mpc5200_fec_can_receive(NetClientState *nc)
{
    MPC5200FECState *s = qemu_get_nic_opaque(nc);
    /*
     * Accept frames whenever the FEC is enabled. The actual delivery
     * to system memory is the BestComm executor's job; we drop here
     * if it isn't wired up.
     */
    return (s->regs[FEC_ECR / 4] & ECR_ETHER_EN) != 0;
}

static ssize_t mpc5200_fec_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t len)
{
    MPC5200FECState *s = qemu_get_nic_opaque(nc);
    /*
     * No BestComm Rx task model yet — drop frame on the floor.
     * Once the executor is in, this becomes a producer to a small
     * Rx FIFO that the executor drains.
     */
    (void)s;
    (void)buf;
    return len;
}

static void mpc5200_fec_set_link(NetClientState *nc)
{
    MPC5200FECState *s = qemu_get_nic_opaque(nc);
    lan9118_phy_update_link(&s->mii, nc->link_down);
}

static NetClientInfo mpc5200_fec_net_info = {
    .type           = NET_CLIENT_DRIVER_NIC,
    .size           = sizeof(NICState),
    .can_receive    = mpc5200_fec_can_receive,
    .receive        = mpc5200_fec_receive,
    .link_status_changed = mpc5200_fec_set_link,
};

static void mpc5200_fec_reset_hold(Object *obj, ResetType type)
{
    MPC5200FECState *s = MPC5200_FEC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    /* MAC address into PALR/PAUR. */
    uint8_t *m = s->conf.macaddr.a;
    s->regs[FEC_PALR / 4] = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) |
                            ((uint32_t)m[2] <<  8) | ((uint32_t)m[3]      );
    s->regs[FEC_PAUR / 4] = ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16) |
                            0x00008808; /* PAUR low 16 = standard pause type */

    lan9118_phy_reset(&s->mii);

    /*
     * PHY defaults the BSP requires:
     *   BMCR = 0x3100  AN_EN(12) | RESTART_AN(9) | ...low bits make probe pass
     *     The probe is `andi. r9,r9,0x3f` against BMCR — must be non-zero.
     *     0x3100 has bit 8 (FD) and bits 9 (RESTART_AN) and 12 (AN_EN) set;
     *     low 6 bits are zero so we add 0x0001 (a vendor-reserved low bit
     *     that some PHYs return non-zero, which makes the probe pass).
     *   BMSR = 0x782D  bits 14:11 capabilities + bit 5 AN_COMPLETE
     *                  + bit 3 AUTONEG + bit 2 LINK_ST + bit 0 EXTCAP
     */
    s->phy_bmcr = 0x3101;
    s->phy_bmsr = 0x782D;

    mpc5200_fec_update_irq(s);
}

static void mpc5200_fec_realize(DeviceState *dev, Error **errp)
{
    MPC5200FECState *s = MPC5200_FEC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mii), errp)) {
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &mpc5200_fec_ops, s,
                          "mpc5200-fec", MPC5200_FEC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&mpc5200_fec_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)),
                          dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    /*
     * Force PHY link up. SLIRP-backed NICs report link up by default,
     * but the order of nic-realize / phy-reset / link-status-changed
     * means the PHY's status register may have been initialized with
     * link_down=true. Forcing it after realize guarantees BMSR has
     * AN_COMP|LINK_ST set when the BSP polls it.
     */
    lan9118_phy_update_link(&s->mii, false);
}

static void mpc5200_fec_init(Object *obj)
{
    MPC5200FECState *s = MPC5200_FEC(obj);
    object_initialize_child(obj, "mii", &s->mii, TYPE_LAN9118_PHY);
}

static const Property mpc5200_fec_properties[] = {
    DEFINE_NIC_PROPERTIES(MPC5200FECState, conf),
    DEFINE_PROP_UINT8("phy-addr", MPC5200FECState, phy_addr, 0x00),
};

static const VMStateDescription vmstate_mpc5200_fec = {
    .name = TYPE_MPC5200_FEC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MPC5200FECState, FEC_REG_COUNT),
        VMSTATE_UINT8(phy_addr, MPC5200FECState),
        VMSTATE_END_OF_LIST()
    },
};

static void mpc5200_fec_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mpc5200_fec_realize;
    dc->vmsd = &vmstate_mpc5200_fec;
    dc->desc = "Freescale MPC5200 Fast Ethernet Controller";
    rc->phases.hold = mpc5200_fec_reset_hold;
    device_class_set_props(dc, mpc5200_fec_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo mpc5200_fec_info = {
    .name           = TYPE_MPC5200_FEC,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(MPC5200FECState),
    .instance_init  = mpc5200_fec_init,
    .class_init     = mpc5200_fec_class_init,
};

static void mpc5200_fec_register_types(void)
{
    type_register_static(&mpc5200_fec_info);
}

type_init(mpc5200_fec_register_types)
