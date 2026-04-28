# MPC5200 X1226 EEPROM Completion — Design

**Date:** 2026-04-27
**Branch:** `mpc5200-stub`
**Author:** Kasper (with Claude)
**Scope tier:** A3-step-1 of the larger "boot VxWorks dump in QEMU" arc.

## Larger target (for reference, not this design)

End goal: run the Vestas turbine VxWorks dump (`/tmp/vxworks_romfs/vxworks.out`)
under `qemu-system-ppc -machine mac99 -cpu mpc5200` far enough that the kernel
mounts `vxworks.romfs.zlfs` (boundary "A3"). Eventually we want network
(Ethernet) so the Vestas Online Toolkit can connect over AP/Firedrake/NEON, but
that is later work.

Approach for the whole arc: pragmatic stubs in `hw/ppc/mac_newworld.c`, no new
machine type, no faithful device models. Grow the stub gradually as the kernel
hits each next blocker. **Eventual** scope includes PSC1 TX path, NOR flash
mapping for the romfs, and FEC Ethernet — none of those are part of this
design.

## This design's scope

Just one blocker: VxWorks is currently stuck polling I2C2 (~462
status/control reads vs only 3 data reads in the first 8 seconds). The chip
on that bus is an **Intersil X1226** (RTC + 512-byte EEPROM, accessed via the
`m5200i2c` driver). Our current stub is a static 72-byte blob with a stuck
status response, and the kernel never advances past the X1226
`readCalibration` path.

Goal of this step: kernel completes X1226 init and proceeds to the next
unknown blocker. Measured by:

- I2C2 status register (`0x3d4c`) polls drop from 230+ to ≤10 per logical
  transaction in a fresh trace; AND
- the MMIO log shows the kernel reaching MMIO addresses we have not seen
  before (PSC writes that look ASCII, flash CS reads, FEC, etc.) — evidence
  it left the I2C polling phase.

We will **not** chase any new blockers that appear after EEPROM completion in
this design — those become future steps.

## Background data

Already established this session:

- `mpc5200_tick` no longer raises `PPC_INTERRUPT_EXT`; it only sets
  `ic_pending`. Cleared the HV_EMU loop.
- VxWorks reaches its scheduler via DECR @ vector `0x900`. EXT @ `0x500` is
  never installed, so we must not deliver EXT.
- Top MMIO offsets in current 8s window: `0x3d48` (232×), `0x3d4c` (230×),
  `0x1200` (98×, PSC1 mode reg — out of scope here), `0x508/0x504/0x510/0x50c`
  (IC, low counts, init-time only).
- Kernel strings confirming X1226: `"RTC-x1226"`,
  `"x1226readCalibration WARNING: Using defaults"`,
  `"x1226_test_getalarm"`, `"x1226Eeprom_test_BP: ..."`.
- Existing 72-byte EEPROM blob covers board-type byte (offset 7 = 0x17,
  controller type 23) and a CRC32. Origin: prior session inference.

MPC5200 I2C register map (offsets relative to `MBAR + 0x3d00` for I2C1, +0x3d40
for I2C2; we are seeing I2C2):

| Off  | Name      | Notes                                                   |
|------|-----------|---------------------------------------------------------|
| 0x40 | MADR      | slave address                                            |
| 0x44 | MFDR      | divider                                                  |
| 0x48 | MBCR      | control: bits MEN, MIEN, MSTA(START), MTX, TXAK, RSTA   |
| 0x4c | MBSR      | status: bits MCF, MAAS, MBB, MAL, RXAK; MIF in low byte |
| 0x50 | MDR       | data                                                     |
| 0x54 | MDFSRR    | filter sample rate                                       |

In our stub, the I2C2 region is mapped at MBAR offsets `0x3d40–0x3d57`.
Existing handler:

```c
if (offset == 0x3d0c || offset == 0x3d4c) return 0x82000000; /* I2C SR */
if (offset == 0x3d50) { /* return next blob byte */ }
if (offset == 0x3d48 && (value & 0x10000000)) { s->i2c_byte = 0; }  /* WRITE */
```

Two specific defects this causes:

1. `MBSR` always returns `MCF=1, MIF=1` regardless of whether a transaction
   is actually in progress. The driver loop polls until `MIF=1` *after
   clearing*; we never let it go to 0, so the loop's "is this a new event"
   check fails.
2. Slave-address (`MADR`) and internal-address writes are silently dropped,
   so the BSP can't address the X1226 sub-registers (RTC vs EEPROM) — every
   read returns the same blob byte.

## Approach (Approach 2 — pragmatic stubs)

Replace the 72-byte static blob with a tiny **state-machine I2C model**
nested inside `MPC5200State`. Single new struct:

```c
typedef struct {
    enum { I2C_IDLE, I2C_ADDR, I2C_REG_HI, I2C_REG_LO, I2C_DATA } phase;
    uint8_t  slave_addr;     /* 7-bit, last write to MADR or first byte after START */
    uint16_t reg_addr;       /* X1226 16-bit internal address */
    bool     mif;            /* interrupt flag, cleared on MBSR read */
    bool     mcf;            /* transfer complete */
    bool     mbb;            /* bus busy */
    bool     reading;        /* true on RD bit in slave-addr byte */
} MPC5200I2CState;
```

State transitions:

- Write to `MBCR` (`0x3d48`) with `MSTA=1` → set `mbb=1`, `phase=ADDR`,
  clear `mif`/`mcf`.
- Write to `MBCR` with `MSTA=0` while `mbb=1` → STOP: set `phase=IDLE`,
  `mbb=0`, `mcf=1`.
- Write to `MDR` (`0x3d50`):
  - in `ADDR` phase → store low 7 bits as `slave_addr`, bit0 as `reading`,
    transition to `REG_HI` (X1226 expects 16-bit internal addr) or to `DATA`
    if `reading` (repeated start case)
  - in `REG_HI` → top byte of `reg_addr`, transition to `REG_LO`
  - in `REG_LO` → low byte of `reg_addr`, transition to `DATA`
  - in `DATA` (master TX) → store byte at `eeprom[reg_addr++]`
  - In each case: `mif=1`, `mcf=1`.
- Read from `MDR` while `phase==DATA && reading` → return
  `eeprom[reg_addr++]`, set `mif=1`, `mcf=1`.
- Read from `MBSR` (`0x3d4c`) → assemble byte: `MCF<<7 | MBB<<5 | RXAK<<0 |
  MIF<<1`; **then clear `mif`** so the next poll-after-event distinguishes
  cleanly.

X1226 protocol assumptions (from datasheet):

- Slave address `0x6f` = RTC (clock/control/status/alarm registers, ~0x30
  bytes).
- Slave address `0x57` = EEPROM (full 512-byte user space).
- All accesses use 16-bit internal address: write `0xS6, addr_hi, addr_lo,
  …data` for write; for read use repeated start: `0xS6, addr_hi, addr_lo,
  RESTART, 0xS7, data, data, …`.

The model only needs to recognize these two slave addresses. Everything else
returns 0xFF (NACK behavior — we'll set RXAK if the BSP cares).

EEPROM content (initial 512 bytes):

- Byte 0..3: X1226 calibration "version" magic the kernel checks. Concrete
  value to be filled by reading what the BSP expects from disassembly of
  `x1226readCalibration` (which prints `"Compatible version %d found,
  current is %d"`). For first cut: leave zero-filled, rely on the kernel's
  `"Using defaults"` fallback path.
- Byte 7: 0x17 (board type 23) — keep from current stub.
- Byte 60..63: CRC32 of preceding bytes — keep computation from current stub.
- All others: 0x00.

RTC content (X1226 reg space at slave 0x6f): zero-filled is fine. Time is read
during boot but the BSP doesn't seem to gate on it; if it does we add fields.

## Discovery sequence

Implementation will be iterative:

1. Re-enable MMIO logging, narrowed to I2C2 range only (avoid 800-line cap).
2. Implement the state machine empty (no EEPROM data, return 0xff).
3. Run, capture trace, see what addresses the BSP hits and what status
   transitions it expects.
4. Fill EEPROM bytes the kernel reads, in the order it reads them.
5. Stop when status polls drop to ≤10 per logical transaction *and* new MMIO
   addresses appear in the log.

## Out of scope

- PSC1 TX (next step, separate design).
- NOR flash / `vxworks.romfs.zlfs` mapping.
- FEC Ethernet.
- IC interrupt delivery — kept disabled.
- Real I2C clock timing — we ignore MFDR, return ready immediately.
- I2C1 (`0x3d00–0x3d3f`) — we only model I2C2.
- Cleanup of unrelated debug `fprintf`s — defer to a final cleanup commit
  for the larger arc.

## Files to modify

- `hw/ppc/mac_newworld.c` only.
  - Lines ~96–117: replace `mpc5200_eeprom[72]` blob with `eeprom[512]` and
    add `MPC5200I2CState` to `MPC5200State`.
  - Lines ~148–211: extend `mpc5200_mmio_read` / `mpc5200_mmio_write` with
    the I2C state machine for offsets `0x3d40–0x3d57`. Drop the existing
    `0x3d4c == 0x82000000` shortcut and the `0x3d48 START → counter reset`
    shortcut.
  - Lines ~286: `g_new0` already covers extra struct fields.

## Verification

```bash
cd /home/kasper/qemu
ninja -C build qemu-system-ppc
timeout 8 ./build/qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null 2>/tmp/qemu_stderr.txt
grep -oE "off=0x[0-9a-f]+" /tmp/qemu_stderr.txt | sort | uniq -c | sort -rn | head -20
```

Pass criteria:

- `0x03d4c` access count drops below 50 (was 230).
- At least one MMIO address never seen before appears in the top-20 list
  (e.g. flash CS region, FEC `0x3000–0x33ff`, or new PSC offset).
- No HV_EMU or PROGRAM exceptions reintroduced (`-d int` shows DECR only).

## Risks

1. The X1226 protocol assumption (16-bit reg addr, slave 0x6f vs 0x57) might
   be wrong for this Vestas board's wiring — they may use a non-standard
   address. Mitigation: the trace from step 1 will reveal the real
   `MADR` / first-byte-after-START values; correct before filling EEPROM.
2. The kernel's `x1226readCalibration` may NOT take the "Using defaults"
   branch and instead loop forever waiting for a specific version match.
   Mitigation: if step 4 reveals an infinite read of a specific offset,
   inject a known-good calibration version byte at that offset.
3. The 8-second test window may be too short to see the kernel reach the
   next blocker. Mitigation: extend timeout to 20 s if needed.

## Verification result (post-implementation)

**Implemented as designed**, then traced. Findings:

- The kernel never targets X1226 addresses (0x57/0x6f). It only touches
  **slave 0x50** — the CT6003 motherboard board-id EEPROM, AT24Cxx-style
  with 1-byte internal addressing. Risk #1 from above hit; state machine
  patched to also accept 0x50.
- Trace shows exactly **13 START/ADDR/STOP transaction pairs**, alternating
  WR-probe and RD-probe. **Zero data reads.** This is the
  `m5200i2c::I2CBitBangProbe` bus-init/recovery sequence — confirms both
  directions ACK on the slave, then moves on. All 13 transactions complete
  in well under 1 s; I2C2 is never touched again in the observed window.
- Kernel progresses to FEC Ethernet init (`0x3000+` MMIO range) and PSC1
  polling (`0x1200`, 98 reads — the next blocker). Earlier MMIO offsets
  (`0x528`, `0x70c-0x71c`, `0x31xx`) appear that we hadn't seen before.
- No HV_EMU / PROGRAM exceptions reintroduced.

**What is NOT verified:** the kernel's real EEPROM reads (`e2promRead*`,
`x1226readCalibration`) run downstream of PSC1 init, which is still
blocked. So the fact that our state machine *would* return version-4
formatted bytes if asked has not actually been exercised. This becomes
something to confirm when the next step unblocks PSC1.

**Plausibility of model under future real reads:** kernel strings show the
BSP expects a CT6003 board, version 4 EEPROM layout (matches existing
blob), and has graceful `"Using default"` / `"Using defaults"` fallbacks
for both missing MAC and bad x1226 calibration — so even an imperfect
return value is likely to keep boot alive rather than panic.

**Spec status:** this step delivered "I2C2 init phase no longer blocks"
(the actual measured outcome), which is narrower than the spec's stated
goal of "kernel completes X1226 init / x1226readCalibration / EEPROM
probe". Real EEPROM-content correctness deferred to a later step where it
can actually be measured.
