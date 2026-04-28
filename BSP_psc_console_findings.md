# PSC console diagnostic — BSP configures but doesn't print

## What we did

Added broad PSC range logging to `mac_newworld.c` to capture every
read and write in `MBAR+0x2000..0x2C00`. Goal: route any TX-buffer
writes to QEMU's stderr so we'd see BSP log messages.

## Results

### PSC1 init sequence (all 4-byte writes are CR command bits)

```
PSC1 W +0x14 sz=2 val=0x0000      IMR (interrupt mask = 0)
PSC1 W +0x08 sz=1 val=0x20        CR: reset receiver
PSC1 W +0x08 sz=1 val=0x30        CR: reset transmitter
PSC1 W +0x08 sz=1 val=0x40        CR: reset error
PSC1 W +0x08 sz=1 val=0x50        CR: reset break-change
PSC1 W +0x08 sz=1 val=0x10        CR: enable transmitter
PSC1 W +0x38 sz=1 val=0x01        clock register (?)
PSC1 W +0x14 sz=2 val=0x0000      IMR clear
PSC1 W +0x8e sz=2 val=0x0000      reserved/extended
PSC1 W +0x08 sz=1 val=0x1a        CR: enable RX/TX
PSC1 W +0x08 sz=1 val=0x30        CR: reset transmitter
PSC1 W +0x08 sz=1 val=0x20        CR: reset receiver
PSC1 W +0x00 sz=1 val=0x13        MR1
PSC1 W +0x00 sz=1 val=0x07        MR2
PSC1 W +0x08 sz=1 val=0x04        CR
PSC1 W +0x08 sz=1 val=0x01        CR
PSC1 W +0x1c sz=1 val=0x12        CTLR
PSC1 W +0x18 sz=1 val=0x00        CTUR
```

### PSC4 init sequence

Same pattern as PSC1 — board configures both PSC1 AND PSC4. PSC4
is likely a secondary serial channel (debug/Snoopy/auxiliary).

### Critical observation

**Zero PSC reads. Zero TX buffer writes.** BSP never reads
status, never writes characters. The init sequence completes, then
nothing.

This means the BSP wedges between PSC init complete and first
printf call.

## Implication

PSC console as a diagnostic tool is **not useful** for the current
wedge. The BSP doesn't reach the printf phase, so even a perfect
console wouldn't show messages.

## What this rules out / in

**Rules out:**
- "BSP is printing but our stub silently drops it"
- "BSP needs PSC interrupts to proceed" (BSP never reads PSC status,
  never enters polling/interrupt-wait loop)

**Rules in:**
- The wedge is in **task synchronization** — some task is blocked
  on a semaphore/queue/event that we're not delivering, and the
  blocked task is the one that would run printf
- Possible candidates: NVRAM read (bootline parse), some
  hardware-init handshake (e.g. BestComm task setup waiting for
  task-done interrupt that never fires)

## Suggested next moves

1. **Walk the bootrom's WIND_TCB to identify which task is parked
   and what it's blocked on.** The bootrom is NOT stripped — we
   can identify task names directly. This is the same technique
   Daniele used for `vxworks.out`.

2. **Disassemble `usrRoot` (`0x010d60f8`) and trace the early
   linear flow.** Find the first call that would block on something
   we don't model.

3. **Check NVRAM-related calls.** `getBootlineTlv`, `sysNvRamGet`
   — if NVRAM is unmapped or returns garbage, bootline parse may
   fail and the BSP may sit waiting for a key prompt that we don't
   provide.

4. **Pivot to TFFS cold-start.** The bootrom supports
   `tffs=0,0(0,0)`. If we model NAND and pre-populate it with
   runtime image, the bootrom skips network init entirely. May
   sidestep the current wedge — but requires modeling NAND.

## Code state

PSC range logging is now in `mac_newworld.c` (writes + reads). PSC1
TX bytes (any of `+0x0c`, `+0x10`, `+0x40`) routed to stderr as
ASCII. Currently emits nothing — but ready for the day BSP starts
actually printing.

## Sources

- `/tmp/vxworks_romfs/bootrom.elf` — extracted unstripped bootrom
- `BSP_embedded_bootrom_findings.md` — bootrom symbol list
- Manual chapter 15 (PSC) — register layout reference
