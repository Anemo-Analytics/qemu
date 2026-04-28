# Embedded VxWorks bootrom inside `vxworks.out`

The file `/tmp/vxworks_romfs/vxworks.out` contains a **second
standalone VxWorks bootrom ELF** embedded in its `.data` segment.
This is the key boot-time component — possibly the right binary for
us to actually be running in QEMU.

## Location and extraction

```
File offset:  0xbc2ad0
Size:         0x2207a4 (≈ 2.2 MB)
Format:       ELF32-BE PowerPC, statically linked, NOT stripped
Entry point:  0x01000000
Load segment: 0x01000000..0x011F35C0 (1.95 MB), MemSiz 2.06 MB
```

Extracted locally as `/tmp/bootrom_extracted.elf`. Reproduce with:

```bash
dd if=/tmp/vxworks_romfs/vxworks.out of=/tmp/bootrom_extracted.elf \
   bs=1 skip=$((0xbc2ad0)) count=$((0x2207a4))
```

The bootrom strings reference its own filename
`/romfs/bootrom_chain_mmc.elf` and version "2.33.101".

## Why this matters

**Symbols are PRESERVED.** Unlike `vxworks.out` (stripped, only
.data string-references give us function names), the bootrom has a
full symbol table. Any future binary analysis can use real names:

```
01000000 <entry>
010d50a8 T usrNetBoot
010d525c T usrNetBootConfig
010d57b4 T usrNetEndLibInit
010d6f18 T usrNetApplUtilInit
010d60f8 T usrRoot
010d8718 T usrInit
010e0b60 T sysHwInit
010e07fc T sysHwInit2
010e628c T m5200FecEndLoad      ← entry, runtime is at 0x12c184
010e4cf0 t m5200FecStart
010e4a88 t m5200FecStop
010e5af8 t m5200FecRestart
010e60c0 t m5200FecUnload
010e4374 t m5200FecPollSend
010e4238 t m5200FecPollReceive
010e3b44 t m5200FecInt
01054780 T muxDevLoad
01054a7c T muxDevStart
0100212c T tffsSetup
01002794 T tffsDrv
01002df0 T tffsDevCreate
0100261c T tffsSocketSetAccessHandlers
```

121 m5200Fec / BestComm / EndLoad-related symbols total.

## Boot-line format documented in bootrom strings

The bootrom understands two boot device strings:

```
fec(0,0):ct6003/vxworks e=169.254.254.254:ffff0000 h=169.254.254.252 \
         u=anonymous pw=test@cotas.dk o=fec f=0x08 tn=localtarget

tffs=0,0(0,0) u=ct6003 pw=ct6003 o=fec tn=localtarget f=0x08
```

Translating:

- **`bootDev = fec(0,0)`** — network boot via FEC. `e` = our IP +
  netmask (`169.254.254.254` netmask `0xffff0000`). `h` = host IP
  (`169.254.254.252` — confirms Daniele's finding). `u` =
  `anonymous`, `pw` = `test@cotas.dk`. File path `ct6003/vxworks`.
- **`bootDev = tffs=0,0(0,0)`** — local boot from TFFS (TrueFFS,
  the NAND-flash filesystem). User `ct6003` / pw `ct6003`. This is
  the cold-start option mentioned earlier in `PLAN_Daniele.md`.

Both bootlines reference `o=fec` — the Ethernet device name is
`fec`. Other strings:

```
boot device: ata=ctrl,drive          file name: /ata0/vxWorks
boot device: tffs=drive,removable    file name: /tffs0/VxWorks
Attaching to TFFS...
%s is not compatible with CT6003.03   ← compatibility check
Loading %s (bootrom version %s) at offset 0x%x. File length: %d
MAC address from E2PROM on CT6003 : 0x%x,0x%x,...
```

## What the bootrom does

Standard VxWorks bootrom flow:

1. `usrInit` (`0x010d8718`) → `sysHwInit` (`0x010e0b60`) →
   `sysHwInit2` (`0x010e07fc`) — early hardware setup. This is what
   we currently see running in our QEMU trace (NIPs `0x11d2xx`
   inside our own — wait, those were in vxworks.out's text region,
   not bootrom space).
2. `usrRoot` (`0x010d60f8`) — task that runs after kernel comes up.
3. `usrNetInit` (`0x010d4c48`) → `usrNetBootConfig` (`0x010d525c`)
   → parses bootline, decides FEC vs TFFS.
4. If FEC: `usrNetEndLibInit` → `muxDevLoad("fec")` →
   `m5200FecEndLoad` (`0x010e628c`) — sets up FEC+BestComm. **TCR
   writes happen here.**
5. FTP client connects to `h=169.254.254.252` and downloads
   `ct6003/vxworks`.
6. Loaded image jumps to its entry point.

## TFFS support

The bootrom has full TFFS / NAND flash support:

```
01002794 T tffsDrv
01002df0 T tffsDevCreate
01002fd0 t tffsBlkRd
010032bc t tffsBlkWrt
01003b9c T tffsDevFormat
010044cc T tffsHowManyParts
01005800 t absMountVolume
```

So **the cold-start path via `tffs=0,0(0,0)` is real and supported
by the bootrom**. If we model NAND flash and populate it with the
runtime image, the bootrom would skip FTP entirely and load from
flash.

## Strategic implication

We've been loading `vxworks.out` directly via QEMU's
`-device loader,file=...`. This loads the runtime kernel at
`0x100000` and starts it executing. **But the runtime kernel
expects to be loaded by the bootrom**, not started cold. It assumes
network init / TFFS init has already been done and the bootline is
in known state.

When we cold-boot `vxworks.out`, the runtime kernel is in an
ill-defined state — it sees its own `sysHwInit` BestComm
configuration go through (which is what we observe), but
`usrNetBootConfig` may not run because the runtime expects the
bootrom to have done it already.

**Possible new path:** load the **bootrom** as the kernel image
instead. Entry point `0x01000000`. The bootrom would:
- Run its own init
- Parse bootline (we'd need to provide one)
- Either FTP from `h=169.254.254.252` (which our pyftpdlib serves)
  or boot from TFFS (which we'd need to model)

This is a significant pivot. Worth investigating before sinking
more time into making the runtime kernel work standalone.

## Compatibility with current work

Most of our progress translates:

- ✅ MPC5200 MMIO stub, IC dispatch fix, PHY model — all needed by
  bootrom's `m5200FecEndLoad`
- ✅ FTP server (pyftpdlib) — bootrom's FTP client connects to it
- ✅ KeySwitch patch — may need to be re-applied at bootrom address
  if the same gate exists there (the bootrom has its own m5200Fec
  driver copy)
- ⚠ The patch addresses `0x12d390` and `0x12ae60` are RUNTIME
  addresses. Bootrom equivalents would be at the bootrom's m5200Fec
  driver locations (need to find them — they're at `0x010e4cf0`
  area).

## Experimental result — booting the bootrom directly

Tried: `qemu-system-ppc -device loader,file=/tmp/bootrom_extracted.elf`.

**Bootrom boots and reaches kernel idle.** Same behavior shape as
`vxworks.out`:

- Same BestComm IPR config (NIPs in `0x010e0c3c..0x010e0c60`,
  matching bootrom's own m5200Fec driver region)
- Same TaskBar / IntMask / PtdControl writes (NIPs `0x010e9e7c..`)
- Same single FEC ECR=0 write
- **CPU permanently idle in `intUnlock` (`0x0100107c`)** — entered
  via `reschedule` (`0x010c5150`). Standard VxWorks idle.
- Zero TCR writes
- Zero PSC TX output
- 30-second run = only 2 FEC ops total

So the bootrom *also* gets stuck — kernel alive, scheduler running,
all tasks blocked. Same wedge as runtime, just a different binary
hosting it.

This rules out "we're running the wrong binary." The blocker is
upstream of both binaries.

## Boot configuration insights from bootrom strings

The bootrom supports **6 board variants** with hardcoded bootlines:

```
fec(0,0):ct6003/vxworks  ...  e=169.254.254.254 h=169.254.254.252 ...
fec(0,0):ct360/vxworks   ...
fec(0,0):ct296/vxworks   ...
fec(0,0):ct3603/vxworks  ...
fec(0,0):ct440/vxworks   ...
fec(0,0):def/vxworks     ...  (default)
tffs=0,0(0,0) u=ct6003 pw=ct6003 ...  (cold-start alternative)
```

The `f=0x08` flag means "quick autoboot (no countdown)" — so the
bootrom should NOT be waiting for a keypress. The `0x04` flag is
"don't autoboot"; we don't have that. So timeout-based wedge isn't
explained by autoboot prompt.

Board ID is read via I2C2 EEPROM on slave `0x50` — which our QEMU
already serves correctly (CT6003 board type 23). So board detection
should pick the `ct6003/vxworks` bootline.

## Bootline storage

```
010fb510 T getBootlineTlv
010fb474 T saveBootLineTlv
011f2414 D sysBootLine
010deab8 T sysNvRamGet
010df0f0 T sysNvRamSet
```

The bootline is stored in NVRAM as a TLV record. If our NVRAM
isn't initialized with valid bootline data, `getBootlineTlv` may
return empty/garbage, falling back to default `def/vxworks` (which
isn't our intended target).

For now this isn't blocking *progress* (the wedge is earlier), but
once we unblock task scheduling, NVRAM bootline content may matter
for which path the bootrom picks.

## Console output

`m5200PscSioDevInit` exists in symbols. The bootrom's stderr/console
goes through PSC. **We don't see any PSC TX output** in our trace —
which means either:

- BSP hasn't initialized the PSC console yet (idle wedge is before
  console init)
- BSP uses BestComm DMA for PSC writes too, and our PIO stub at
  PSC TX buffer (offset 0x0c) misses them

Without console output we're blind to what the bootrom is logging.
This is a major diagnostic limitation. Fixing PSC console output
should be high priority — it would tell us what "Press any key"
or error messages are firing.

## Updated open questions

1. **What blocks all tasks from progressing past kernel idle?**
   Same question for both binaries. Possible candidates: missing
   timer (sysClk vs SLT mismatch), missing peripheral init that a
   task is waiting on, NVRAM read returning unexpected values.
2. **Can we get PSC console output working?** Even a basic
   "echo to stderr" path would give us BSP log messages, which
   would massively accelerate diagnosis.
3. **Is sysClk firing properly?** Our SLT timer fires, EXT
   delivery works (per gates 1+2). But maybe the kernel uses a
   different timer for sysClk than what we model.
4. **What does `usrNetBootConfig` (0x010d525c in bootrom) do
   first, and is the BSP ever reaching it?**

## Strategic implications

- Loading bootrom directly didn't unblock — same wedge.
- Both binaries share fundamental boot path (sysHwInit → kernel
  init → tasks → idle waiting for something).
- The wedge is in **task scheduling / inter-task synchronization**,
  not specifically in FEC code. Building BestComm executor or
  fixing FEC further is unlikely to help until tasks actually run.
- **PSC console output** is the highest-leverage next investment —
  if we can see BSP log messages, we'll know what it's actually
  waiting on.

## Files referenced

- `/tmp/vxworks_romfs/vxworks.out` — original (runtime + embedded
  bootrom)
- `/tmp/bootrom_extracted.elf` — extracted bootrom (local only)
- `BSP_boot_mode_fifo_pio_hypothesis.md` — surfaced this finding
