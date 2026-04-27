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
| MPC5200State struct + QEMUTimer + IC stub | ✅ (implemented, builds clean) |
| Slice Timer EXT interrupt fires | ✅ (fires at ~60 Hz after 1 s delay) |
| VxWorks EXT handler installed | ✅ (confirmed at 0x500 after 2 s) |
| VxWorks handles EXT without faulting | ❌ **HV_EMU → program check loop** |
| Serial output | ❌ (scheduler still dead) |

---

## What Was Done This Session

### Implemented in `hw/ppc/mac_newworld.c`

1. Added `#include "qemu/timer.h"`
2. Replaced `static unsigned int mpc5200_i2c_byte` with heap-allocated `MPC5200State` struct:
   - `MemoryRegion mr`, `QEMUTimer *timer`, `PowerPCCPU *cpu`, `bool ic_pending`, `unsigned int i2c_byte`
3. Added `mpc5200_tick()` callback: sets `ic_pending = true`, calls `ppc_set_irq(cpu, PPC_INTERRUPT_EXT, 1)`, reschedules at 60 Hz
4. Updated `mpc5200_mmio_read()` to use `MPC5200State *s` as opaque; added IC responses:
   - offset `0x0508` when `ic_pending`: returns `0x00000001` (SLT1 source in active field)
   - offset `0x0524` when `ic_pending`: returns `0x40000000` (bit 30 = SLT1 pending)
5. Updated `mpc5200_mmio_write()`: IC writes (0x0500–0x052c) clear `ic_pending` and deassert EXT
6. Wired up in `ppc_core99_init()`: `mpc5200->cpu = POWERPC_CPU(first_cpu)`, timer scheduled 1 s after init, `memory_region_init_io(&mpc5200->mr, ...)` with `mpc5200` as opaque

**Timer confirmed firing** (debug prints visible in stderr when capturing correctly).

---

## Current Blocker: HV_EMU at EXT Vector Entry

**Symptom**: EXTERNAL (4) exception fires at 0x207fe8 → jumps to EXT vector 0x500 → instruction at 0x508 causes `HV_EMU (96)` → converted to program check (0x700) → infinite loop.

**RAM at 0x500 (bytes in memory order)**:
```
0x500: 0x0c 0x0c 0xf8 0x49  → word 0x0c0cf849  (opcode 3 = twi, TO=0 = never-trap NOP)
0x504: 0x81 0x98 0x00 0x9b  → word 0x8198009b  (opcode 32 = lwz)
0x508: 0x13 0xe0 0x0c 0x08  → word 0x13e00c08  ← HV_EMU fires here
```

**Root cause of HV_EMU**: `0x13e00c08` has primary opcode 4 (bits 31–26 = `000100`). On G2/MPC5200, opcode 4 is unimplemented (reserved/AltiVec territory) → QEMU raises `gen_inval_exception` → `POWERPC_EXCP_HV_EMU` → converted to program check (0x700). But 0x700 also loops.

**Why opcode 4?** This is suspicious — VxWorks MPC5200B BSP should not emit AltiVec instructions. Possible explanations:
1. The bytes at 0x508 are NOT an instruction — VxWorks's EXT dispatch uses an inline table or jump-vector at the start of the 0x500 area, and the first `lwz` at 0x504 loads an address, after which execution branches elsewhere — but something goes wrong before the branch.
2. The G2 CPU model in QEMU mis-decodes a valid G2 instruction as opcode-4 because it lacks a G2-specific instruction (e.g., `mtspr` of a G2-only SPR, or some Book E extension that the mpc5200 CPU model doesn't fully support).
3. The `lwz` at 0x504 loads a bad address and causes an ISI before 0x508 — but the exception log shows HV_EMU at 0x508, not an ISI at 0x504.

---

## Step 2 — Fix the EXT Handler Fault  ← **DO THIS NEXT**

### 2a. Enable MMIO logging and trace the IC dispatch

Remove the `&& 0` from the MMIO log guard in `mpc5200_log_access()` to see all IC reads/writes:

```c
if (log_count < 2000) {   /* was: && 0 */
```

Run with `-d int` and capture both stderr (MMIO log) and the interrupt log:

```bash
timeout 5 ./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null -d int 2>/tmp/qemu_err.txt
grep -E "MPC5200|EXTERNAL|HV_EMU|0x05" /tmp/qemu_err.txt | head -40
```

This will show which IC offsets VxWorks reads during the EXT dispatch before the fault.

### 2b. Decode the instruction at 0x508

Use Python + capstone to disassemble the bytes `13 e0 0c 08`:

```python
import capstone
cs = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_32)
for i in cs.disasm(bytes.fromhex('0c0cf8498198009b13e00c08b8000264'), 0x500):
    print(f"0x{i.address:x}: {i.mnemonic} {i.op_str}")
```

If capstone decodes it as `mfspr` or `mfmsr`, the G2 CPU model may lack that SPR → QEMU raises HV_EMU. Fix: add the SPR to QEMU's G2 definition in `target/ppc/cpu_init.c`.

### 2c. Check G2 SPR coverage for the failing instruction

If the instruction is an `mfspr`/`mtspr` of an unknown SPR, find the SPR number and add it to the G2 register set. Common G2 SPRs not always included in QEMU:

- SPR 526/527 (IBAT4U/L, IBAT5U/L, etc. — extended BATs)
- SPR 947 (IABR2, G2 instruction address breakpoint)
- SPR 1009 (HID1, hardware implementation dependent)

### 2d. Alternative: check if the EXT vector area is being mis-fetched

Verify VxWorks actually installed the handler (not zeros) by checking 0x500 at t=2s:

```bash
timeout 3 bash -c '(sleep 2 && echo "xp /32b 0x500") | \
  ./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null -monitor stdio 2>/dev/null'
```

---

## Step 3 — Fix PSC UART TX if output still missing after EXT is fixed

If EXT starts being handled without faults but no characters appear on stderr:

- Enable MMIO logging and look for writes to PSC range (0x2000–0x2bff) after ticks start
- Verify the TX buffer offset: current stub captures `(offset & 0xff) == 0x0c`; adjust if VxWorks writes elsewhere
- The PSC TX might use a different offset in the FIFO mode (e.g., offset 0x40 in FIFO mode)

---

## Step 4 — EEPROM follow-up (if kernel panics after scheduling)

The EEPROM stub only sets board type (data[7]=0x17). If NULL-ptr panics occur after scheduling starts, re-enable MMIO logging, trace I2C reads, and add any missing fields.

---

## File to Modify

`hw/ppc/mac_newworld.c` — all stub code is in lines ~79–200 and `ppc_core99_init()`.

**Debug toggle**: MMIO logging disabled with `&& 0` on `log_count` check (line ~132). Remove `&& 0` to enable.

**Debug prints to remove before final commit**:
- `fprintf(stderr, "MPC5200: init timer, now=...")` in `ppc_core99_init()`
- `static int tick_count` + `fprintf` in `mpc5200_tick()`

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
| EXT INT vector (RAM) | 0x00000500 | VxWorks handler installed here (~2 s into run) |
| DECR vector (RAM) | 0x00000900 | VxWorks stub installed here |
| IC dispatcher | 0x001195b8 | reads IC 0x0504 for priority |
| Program check vector (RAM) | 0x00000700 | currently loops — HV_EMU lands here |

---

## Build & Run (canonical)

```bash
cd /home/dpi/qemu/build
ninja qemu-system-ppc

# Capture stderr to file to avoid pipe-buffering hiding output:
timeout 5 ./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null 2>/tmp/qemu_err.txt; cat /tmp/qemu_err.txt

# With interrupt log:
timeout 5 ./qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -display none -serial null -d int 2>/tmp/qemu_err.txt
grep -E "EXTERNAL|HV_EMU|MPC5200" /tmp/qemu_err.txt | head -20
```

**Note**: Always redirect stderr to a file — pipe to `head` hides output due to buffering.
