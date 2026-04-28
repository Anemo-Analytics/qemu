# Kasper's plan: MPC5200 FEC implementation

**Owner:** Kasper + Claude
**Branch:** `mpc5200-stub` (continue current branch)
**Time budget:** ~3–5 days
**Touches:** `hw/net/*`, `hw/ppc/mac_newworld.c`, build glue
**Output:** working `mpc5200-fec` device, first kernel serial banner

---

## Reference docs (local copies committed to repo)

- `docs/MPC5200_Users_Guide.pdf` — full 732-page MPC5200UG Rev 3.1
  (03/2006). NXP source.
- `docs/MPC5200_FEC_Chapter14.md` — Chapter 14 only, ~56 pages.
- `docs/MPC5200_BestComm_Chapter13.md` — Chapter 13 (SDMA/BestComm),
  ~30 pages.

## Why this exists

Kernel parks at NIP `0x207fe8` after all current peripheral stubs init
clean. Hypothesis (see `PLAN_Daniele.md` for verification): boot-mode
FTP wait at link-local 169.254.254.254. The FEC stub currently returns
plausible config-register reads but moves no packets — RX poll never
produces a frame, so the boot loop stalls.

This track builds the missing FEC device and the host-side FTP
plumbing so packets actually flow. If the hypothesis holds, the kernel
proceeds and we get our first serial banner.

## Hard architectural constraint (from manual page 14-1)

> "BestComm data transfers are interrupt driven. **Interrupt driven
> data movement from the processor is not supported.**"

The FEC cannot move TX/RX data via direct register access — it
**requires** BestComm DMA. A naive fork of `imx_fec.c` (which assumes
processor-driven BD walking) will not produce packet flow.

Implication: this track now has two parts that cannot be skipped.
1. FEC register/CSR model (control, status, MII, link).
2. **BestComm task executor** — interpret writes to SDMA Task Control
   Registers (MBAR+0x121C/0x121E etc.), parse task descriptors stored
   in internal SRAM, execute as DMA copies between system memory and
   the FEC FIFO, fire task-done interrupts.

The current `bestcomm[0x100]` and `sram[0x8000]` register-as-RAM stubs
become the *backing store* for the executor — they're necessary but
not sufficient.

---

## Approach

Fork QEMU's existing Freescale FEC driver. Closest match:

- **`hw/net/imx_fec.c`** — i.MX FEC, same Freescale FEC IP block
  lineage, closest register layout. **Use this as base.**
- `hw/net/mcf_fec.c` — ColdFire FEC, also same family, simpler but
  further from MPC5200 in detail.

Don't `cp` and edit blindly — read `imx_fec.c` end-to-end first, then
fork with a clean rename and trim down to MPC5200's actual register
set.

---

## Concrete steps

### 1. Read `imx_fec.c`

Understand:
- Register layout struct + offset table
- TX/RX BD ring walker (descriptor format: 16-byte BD with status/length
  + data pointer — same as MPC5200)
- PHY MII frame access pattern
- IRQ wiring via `qemu_set_irq()`
- Netdev integration (`NICState`, `NICConf`, qemu_new_nic)

### 2. Create `hw/net/mpc5200_fec.c`

Fork from `imx_fec.c`. Adjust register offsets to MPC5200's actual
layout. Confirmed offsets observed in BSP traces during current
sessions:

| Offset | Register |
|---|---|
| `0x004` | EIR (event/interrupt) |
| `0x008` | EIMR (interrupt mask) |
| `0x024` | ECR (Ethernet control) |
| `0x040` | RDAR (receive descriptor active) |
| `0x044` | TDAR (transmit descriptor active) |
| `0x0E4` | MMFR (PHY MII data) |
| `0x0E8` | MSCR (PHY MII speed) |
| `0x118` | RDSR (RX BD ring base) |
| `0x11C` | TDSR (TX BD ring base) |
| `0x120`–`0x124` | additional ring config |
| `0x144` | MIBC (statistics counter control) |
| `0x188`–`0x1C8` | queue config |

Verify offsets against MPC5200 user manual section 26 (FEC) — the
vault should have it. Don't trust observation alone; we may have
missed offsets the BSP didn't poke yet.

Rename the QOM type to `TYPE_MPC5200_FEC = "mpc5200-fec"`.

### 3. BD ring format

MPC5200 uses standard Freescale 16-byte BDs:

```
struct {
    uint16_t status;   /* R/E/W/L/TC/etc bits */
    uint16_t length;
    uint32_t data_ptr; /* big-endian physical pointer */
    uint32_t reserved[2];
};
```

`imx_fec.c`'s BD walker should port nearly verbatim. Verify
endianness: MPC5200 is BE throughout, so no byteswap dance like the
i.MX has for ARM-LE-host vs FEC-BE.

### 4. Register and build

`hw/net/Kconfig`:
```
config MPC5200_FEC
    bool
    select PTIMER
    select FSL_IMX_FEC  # or NIC base
```

`hw/net/meson.build`:
```
system_ss.add(when: 'CONFIG_MPC5200_FEC', if_true: files('mpc5200_fec.c'))
```

Add `CONFIG_MPC5200_FEC=y` to whichever PPC machine config pulls in
`mac99` (likely `configs/devices/ppc-softmmu/default.mak`).

### 5. Wire into `mac_newworld.c`

In `ppc_core99_init()`:

```c
DeviceState *fec = qdev_new(TYPE_MPC5200_FEC);
qdev_set_nic_properties(fec, &nd_table[0]);
sysbus_realize_and_unref(SYS_BUS_DEVICE(fec), &error_fatal);
sysbus_mmio_map(SYS_BUS_DEVICE(fec), 0, 0xf0003000);
qdev_connect_gpio_out(fec, 0, /* IRQ into IC stub */);
```

Remove the `0xf0003000..0xf0003FFF` range from the MMIO stub
fall-through in `mpc5200_mmio_read` / `mpc5200_mmio_write` (currently
swallowed silently as "unimplemented").

IRQ wiring: the FEC drives interrupt source `gpio-out 0`; route it
into our IC stub by reusing the `mpc5200_tick`/`ic_pending` plumbing
or extending the stub with a per-source latch. Simplest first cut:
when FEC IRQ asserts, set `ic_pending` and `ppc_set_irq(EXT, 1)`.

### 6. Host-side FTP

QEMU command line:

```bash
-netdev user,id=n0,net=169.254.254.0/24,host=169.254.254.253,\
hostfwd=tcp::21-:21 \
-device mpc5200-fec,netdev=n0,mac=00:1b:f0:00:00:0a
```

SLIRP doesn't ship an FTP server. Two options:

**Option A — host-side `vsftpd` (try first):**
- Bind vsftpd to 169.254.254.253:21
- Serve `/path/to/turbine_dump/bin/release_diab_ppc/` as serve root
- SLIRP forwards control + data channels via `hostfwd`
- Risk: passive-mode FTP data channel may need extra forwards

**Option B — `-netdev tap` (fallback):**
- Configure a host TAP interface with 169.254.254.253/24
- Run vsftpd bound to the TAP
- More setup, no SLIRP weirdness

Try A first. If it doesn't pass FTP control+data correctly, switch to
B.

### 7. Verify

```bash
ninja -C build qemu-system-ppc

timeout 30 ./build/qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -netdev user,id=n0,net=169.254.254.0/24,host=169.254.254.253 \
  -device mpc5200-fec,netdev=n0,mac=00:1b:f0:00:00:0a \
  -display none -serial stdio 2>&1 | head -80
```

**Pass:** serial banner appears. Match patterns:
- `MPC%s -- Wind River BSP`
- `Starting VxWorks in runmode`
- Or any human-readable boot string

**Fail:** still parked at `0x207fe8`. Falsifies the FTP-wait
hypothesis. Pivot using Daniele's findings.

---

## Risks and unknowns

1. **Register layout differs from i.MX6 in non-obvious ways** —
   ~~Need MPC5200 FEC manual~~. **Resolved:** manual is now in
   `docs/MPC5200_FEC_Chapter14.md`. Read it before coding the BD
   walker.

2. ~~**BestComm DMA dependency may exist**~~ — **Confirmed by
   manual**, see "Hard architectural constraint" above. Now part of
   the plan, not a risk. The executor must be built; the question is
   whether to build a minimal "task interpreter for FEC tasks only"
   or a general one.

3. **Proprietary upload protocol** — Vault references "Falcon" and
   "Snoopy WCF" but doesn't fully document the boot-mode upload
   protocol. Probably standard FTP given APIPA pattern, not
   guaranteed. Daniele is checking this. If proprietary, swap vsftpd
   for a Python protocol shim.

4. **BestComm microcode** — real BestComm tasks are programs in a
   small instruction set executed by the SDMA engine. We do not need
   to interpret the microcode; the BSP will program the engine, and
   we observe the *effect* (which buffer to copy from, which BD to
   update). But we need to identify in the BSP which task slots are
   used for FEC TX/RX, and what their source/dest fields point at.
   This is BSP reverse-engineering — overlaps with Daniele's work.

---

## What "done" looks like

**Pass:** serial banner appears. Commit + push. Update `PLAN.md`
status. Phase 2 closed; Phase 3 (app-layer protocols) opens.

**Fail:** kernel still parked. Useful too — falsifies the hypothesis
and forces us to use Daniele's findings to redirect. Document failure
mode in a follow-up findings note before pivoting.

---

## Out of scope

- ARCnet (multi-node — only needed once single-node Ground works)
- Firecrest / AP / Firedrake / NEON application-layer protocols
- Booting all the way to `etc/startup.app` execution
- WIND_TCB 5.5.1 decode (Daniele's territory)
- Any binary analysis of `vxworks.out` (Daniele's territory)
