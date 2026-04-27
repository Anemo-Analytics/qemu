# Plan: Boot VxWorks MPC5200B Kernel in QEMU

**Goal**: Produce working serial output (VxWorks shell/banner) from
`/tmp/vxworks_romfs/vxworks.out` running under `qemu-system-ppc mac99 -cpu mpc5200`.

---

## Current State

| Item | Status |
|------|--------|
| Kernel loads & executes | ✅ (`-device loader` at 0x00100000) |
| BAT / SPR init | ✅ (mpc5200 CPU model accepts all SPRs) |
| MPC5200 MMIO stub at 0xf0000000 | ✅ (1 MB, `create_unimplemented_device`-like) |
| I2C2 EEPROM stub (board type 23) | ✅ (version-4, CRC32=0xb8e1de02) |
| PSC UART TX ready bits | ✅ (returns 0x30303030 for 0x2000–0x2bff) |
| DECR interrupts | ✅ (firing, but too fast — harmless) |
| Slice Timer interrupt | ❌ **never delivered → scheduler dead** |
| Serial output | ❌ (no tasks run, PSC never written) |

**Root cause**: `sysClkInt` (ELF 0x118000) is only called when the MPC5200 Slice Timer fires
an external interrupt. Without it: `tickAnnounce` never runs, no tasks are ever scheduled,
and the PSC UART is never written.

**Confirmed**: EXT INT vector (0x500 in RAM) has real VxWorks code.
Critical interrupt vector (0xA00) is all zeros — Slice Timer uses `PPC_INTERRUPT_EXT`.

---

## Step 1 — Deliver the Slice Timer Interrupt  ← **DO THIS FIRST**

### 1a. Refactor MMIO stub to use a state struct

In `hw/ppc/mac_newworld.c`, replace bare `static` globals with a heap-allocated struct
so the QEMU timer and CPU pointer can be carried as opaque:

```c
typedef struct {
    MemoryRegion  mr;
    QEMUTimer    *timer;
    PowerPCCPU   *cpu;
    bool          ic_pending;
    unsigned int  i2c_byte;
} MPC5200State;
```

Add include (already present: `hw/ppc/ppc.h`; add if missing):
```c
#include "qemu/timer.h"
```

### 1b. Periodic timer callback at ~60 Hz

```c
static void mpc5200_tick(void *opaque)
{
    MPC5200State *s = opaque;
    s->ic_pending = true;
    ppc_set_irq(s->cpu, PPC_INTERRUPT_EXT, 1);
    timer_mod(s->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16666667ULL); /* 60 Hz */
}
```

### 1c. IC read responses

The MPC5200 IC register map (base MBAR+0x0500):

| Offset | Register | VxWorks use |
|--------|----------|------------|
| +0x004 (0x0504) | CRIT task priority | read in init loop |
| +0x008 (0x0508) | MAIN task priority / active | **read to identify main interrupt source** |
| +0x014 (0x0514) | CRIT_ENAB | VxWorks writes 0x00010000 to enable SLT |
| +0x024 (0x0524) | MAIN_PEND | main interrupt pending bits |
| +0x028 (0x0528) | PER_PEND | peripheral pending (stays 0 for Slice Timer) |

When `ic_pending` is true, return values indicating Slice Timer 1 (SLT1):

```c
if (offset == 0x0508 && s->ic_pending) return 0x00000001; /* SLT1 = source 1 */
if (offset == 0x0524 && s->ic_pending) return 0x40000000; /* bit 30 = SLT1 pending */
```

> **Note on encoding uncertainty**: The exact bit/field layout for SLT in 0x0508 is not
> definitively known. Start with source=1 (value 0x00000001 in the active field). If VxWorks
> takes the interrupt but calls the wrong handler (or no handler), enable MMIO logging
> (`remove && 0` from `log_count` check) and watch which IC offsets VxWorks reads during
> the ISR to narrow down the encoding. Try source=0 if source=1 doesn't dispatch correctly.

### 1d. IC write — ack and deassert

```c
if (offset >= 0x0500 && offset <= 0x052c) {
    s->ic_pending = false;
    ppc_set_irq(s->cpu, PPC_INTERRUPT_EXT, 0);
    return; /* don't fall through to logger */
}
```

### 1e. Wire up in `ppc_core99_init()`

```c
/* allocate state, replacing g_new(MemoryRegion,1) */
MPC5200State *mpc = g_new0(MPC5200State, 1);
mpc->cpu   = POWERPC_CPU(first_cpu);
mpc->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mpc5200_tick, mpc);
/* start first tick after 100 ms of guest time (let BSP init finish) */
timer_mod(mpc->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000000ULL);
/* pass mpc as opaque to MemoryRegion ops */
memory_region_init_io(&mpc->mr, NULL, &mpc5200_mmio_ops, mpc,
                      "mpc5200-mmio", 1 * MiB);
memory_region_add_subregion(get_system_memory(), 0xf0000000, &mpc->mr);
```

### 1f. Verify

```bash
cd /home/dpi/qemu/build && ninja qemu-system-ppc

# Should now see "EXT" interrupts interleaved with DECR:
./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null -d int 2>&1 | grep -v "^MPC5200" | head -30

# Watch for serial output (PSC TX characters emitted to stderr):
./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null 2>&1 | head -40
```

---

## Step 2 — Fix PSC UART TX if output still missing

If the interrupt fires (EXT INT appears in `-d int`) but no characters appear on stderr,
the PSC TX write offset might be wrong.

**Current stub** matches writes at `(offset & 0xff) == 0x0c` within 0x2000–0x2bff.

MPC5200 PSC register map (per-PSC, base MBAR+0x2x00):
- `+0x00`: PSC mode register
- `+0x04`: PSC status register (SR) — read for TxRDY/TxEMP
- `+0x08`: PSC clock select
- `+0x0c`: PSC transmit buffer ← current stub target
- `+0x14`: PSC FIFO control

**If offset 0x0c is wrong**, enable MMIO logging and look for writes to the PSC range
that arrive after EXT INT starts firing. The VxWorks log task will be the source.

Also check: the stub filters characters below 0x07. VxWorks init prints `\r\n` (0x0d, 0x0a)
which are fine, but check that the filter isn't accidentally dropping control chars.

---

## Step 3 — EEPROM follow-up (if kernel panics after scheduling)

The EEPROM stub currently only sets board type (data[7]=0x17). Additional fields VxWorks
may read and act on:

| data[] index | Likely field | Current value | Risk |
|---|---|---|---|
| [0–6] | serial number / MAC prefix | all 0x00 | probably OK |
| [7] | board type = 23 | 0x17 ✅ | needed |
| [8–11] | hardware revision? | 0x00 | unknown |
| [12+] | calibration / callbacks | 0x00 | risk of NULL ptr |

If kernel panics after scheduling starts (indicated by getting past the banner), re-enable
MMIO logging to trace I2C reads and correlate with disassembly of the e2prom driver.

---

## File to Modify

`hw/ppc/mac_newworld.c` — lines 79–165 (MMIO stub) and the `ppc_core99_init()` function
(search for `mpc5200 = g_new(MemoryRegion, 1)` to find the insertion point).

**Currently**: MMIO logging disabled — `log_count < 2000 && 0`. Remove `&& 0` when debugging.

---

## Key Reference Addresses (ELF vxworks.out)

| Symbol | Address | Notes |
|--------|---------|-------|
| Entry point | 0x00100000 | |
| `sysClkInt` | 0x00118000 | DEC reload + tickAnnounce — called on SLT interrupt |
| DEC add-period | 0x00207a28 | adds period to current DEC value |
| DEC set (simple) | 0x00207a18 | `mtspr DEC, r3; blr` |
| DECR context-switch | 0x00207f58 | entered when DECR fires |
| `tickAnnounce` call | 0x001180b8 | inside sysClkInt, advances tick counter |
| EXT INT vector (RAM) | 0x00000500 | VxWorks stub installed here |
| DECR vector (RAM) | 0x00000900 | VxWorks stub installed here |
| IC dispatcher | 0x001195b8 | reads IC 0x0504 for priority |

---

## Build & Run (canonical)

```bash
cd /home/dpi/qemu/build
ninja qemu-system-ppc

# Interactive run — serial on stdio, QEMU monitor via Ctrl+A C
./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -nographic -serial mon:stdio
```

**Expected outcome after Step 1**: VxWorks banner and shell prompt appear within ~5 s.
