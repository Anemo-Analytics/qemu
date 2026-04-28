# vxworks.out static analysis (Agent A1)

## Binary metadata
- `/tmp/vxworks_romfs/vxworks.out`: **ELF 32-bit MSB PowerPC EXEC, statically linked, stripped**, 14,561,908 bytes
- One LOAD segment: `0x00100000 .. 0x009a8bb0` (RWE), `.text 0x100000-0x42ac34`, `.data 0x42b000`, `.bss 0x908310`
- Entry point: **`0x00100000`** (`_sysInit`)
- Bootrom (Rosetta): `0x01000000..0x011f35c0`. **Identical `_sysInit` prologue (first 0x14e bytes)**, then diverges — vxworks.out branches to its own init at `0x10740c`, bootrom branches to `0x10d85b8` inside its (much larger) `usrInit` chain. **The +0xF00000 offset mapping does NOT hold for non-`_sysInit` code.**

## Entry point disassembly (`_sysInit @ 0x00100000`)
First 30 instructions (first 4 bit-identical to bootrom):

| Addr | Insn | Effect |
|---|---|---|
| `0x100000` | `xor r5,r5,r5` | r5 = 0 |
| `0x100004` | `isync` | barrier |
| `0x100008` | `mtmsr r5` | **MSR := 0** |
| `0x10000c` | `isync` | |
| `0x100010` | `mr r30,r3` | save boot-arg |
| `0x100014-2c` | HID0 cache invalidate/enable | |
| `0x100040-48` | bl `0x1002b0`, `0x100390`, `0x100340` | early init A/B/C |
| `0x100068-6c` | `ori r3,r0,0x2000; mtmsr r3` | **MSR := 0x2000** (ME) |
| `0x10012c` | `mtmsr r4` (=0x2002) | enable FP |
| `0x10013c` | `stw r3, 0x900(r0)` | **install bare `rfi` at addr 0x900** |
| `0x100160` | `b 0x10740c` | jump to init dispatcher |

## Call graph (init chain)

| Caller | BL | Target | Identification |
|---|---|---|---|
| `_sysInit 0x100040/4/8` | bl | early HID/cache init A/B/C | |
| init disp `0x10741c` | bl | `0x001064b8` | bss/data init |
| init disp `0x10742c` | bl | `0x00138c40` | excVecInit-equivalent |
| init disp `0x107430` | bl | **`0x0011d180`** | **`sysHwInit2` equivalent** (writes IMMR `0xf000020c`, `0xf0000214`, `0xf0001f40`) |
| init disp `0x107438` | bl | `0x00304548` | trampoline → `0x306104` (**`usrKernelInit`**) |
| sysClkConnect `0x11ce58` | bl | **`0x1392e0` `excIntConnect(0x900, 0x117fd0)`** | **DEC vector install** |

## DEC SPR programming

**Two `mtdec` (mtspr 22) sites — identical structure to bootrom:**

| vxworks.out addr | Bootrom addr | Bootrom symbol |
|---|---|---|
| `0x00207a18` | `0x01038c1c` | `vxDecSet` |
| `0x00207a40` | `0x01038c44` | `vxDecReload` |

`sysClkInt @ 0x00118080` calls `vxDecReload` to re-arm. `sysClkEnable @ 0x0011aae0` tail-calls `vxDecSet`.

## Exception vector install

**Three `excIntConnect` calls in vxworks.out:**

| Caller | Vector | Handler | Identification |
|---|---|---|---|
| `0x00119148` | `0x0500` | `0x117890` | External Interrupt |
| `0x00119164` | `0x1400` | `0x117764` | System Management |
| `0x0011ce58` | **`0x0900`** | `0x00117fd0` | **DEC → `sysClkInt`** |

## Mapped names (Rosetta Stone)

| vxworks.out address | Name (from bootrom) |
|---|---|
| `0x00100000` | `_sysInit` |
| `0x0010740c` | init dispatcher (≈ `usrInit` body, shortened) |
| `0x00117fd0` | `sysClkInt` |
| `0x0011aae0` | `sysClkEnable` |
| `0x0011cdxx..0x11ce80` | `sysClkConnect` |
| `0x0011d180` | `sysHwInit2` (IMMR writes) |
| `0x001392e0` | `excIntConnect` |
| `0x00207a18` | `vxDecSet` |
| `0x00207a28` | `vxDecReload` |
| `0x002fc16c` | `kernelInit` |
| `0x00306104` | `usrKernelInit` |

## Notable observations

1. **DEC programming code is structurally IDENTICAL between vxworks.out and bootrom.** Both have 2 `mtspr 22` sites, 41 `mtmsr`, 7 `rfi`, 3 `excIntConnect` covering vectors 0x500/0x900/0x1400. **The presence of DEC code is not the divergence** — the question is whether the boot path *reaches* `sysClkConnect` + `sysClkEnable`.

2. **vxworks.out's "usrInit" at `0x10740c` is dramatically simpler than bootrom's** (`0x10d8718`):
   - Bootrom usrInit starts with a **sync-handshake spinlock** waiting for two magic values (`0x12348765` at `[r7+0x1c30]`, `0x5a5ac3c3` at `[r8+0x1c34]`) before proceeding.
   - vxworks.out skips this entirely — it's designed to be loaded BY the bootrom (or by something that has already done bootrom-equivalent setup).

3. **`_sysInit` writes `0x4c000064` (rfi) to memory address `0x900`** at `0x10013c` — bare `rfi` safety net at DEC vector.

4. **`tickAnnounce` and `windTickAnnounce` did not signature-match** in vxworks.out — different inlining/optimization or library version.

5. **For QEMU debugging targets:** breakpoint/NIP-trace on:
   - `0x0011aae0` (`sysClkEnable` entry)
   - `0x00207a18` (`vxDecSet`)
   - `0x0011ce58` (excIntConnect for vector 0x900)
   - `0x00117fd0` (`sysClkInt`)
   - `0x002fc16c` (`kernelInit`)
