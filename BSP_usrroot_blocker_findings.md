# usrRoot blocker — bootrom wedges on I2C1 probe

## TL;DR

Both verifying agents (A: WIND_TCB walk, B: usrRoot linear trace)
returned. **Agent B identified the precise blocker:**

`usrRoot` calls `usrI2CMasterInit` at `0x010d6144`, which calls
`i2cMasterDevLoad` (`0x01001764`), which probes the I²C bus by
polling status registers at `MBAR+0x3D00` (I2C1) and
`MBAR+0x3D40` (I2C2). Our QEMU has I2C2 fully modeled (X1226 RTC +
EEPROM state machine), but **I2C1 is a status-only stub** that
returns `0x82` on `MBSR` reads and 0 elsewhere. The probe routine
never gets the correct "address NACK" signal and spins.

Plus a second concurrent issue from Agent A: the bootrom uses the
**PowerPC DEC (decrementer)** for system tick, not the MPC5200 SLT
timer. Our `-d int` log on the bootrom run shows **534 EXTERNAL
events but 0 DECR events** — DEC isn't being delivered. So even if
we fix the I2C wedge, the next blocker is `taskDelay()` waiting on
DEC ticks that never arrive.

## Agent A — WIND_TCB walk findings

**Task spawned by usrRoot:** single task `tBoot` at entry
`bootCmdLoop` (`0x010d7aec`), priority 1, stack 10 KiB.

**Spawn site:** `taskSpawn` at `0x010d65c4`. But — `tBoot` is only
spawned AFTER three conditional `taskDelay()` calls in `usrRoot`:

- `0x010d62ac`: `taskDelay(sysClkRateGet()/2)` — gated by `GetUnitType()==0x17`
- `0x010d62f4`: `taskDelay(0x1e)` — gated by `GetMotherBoardType()==3`
- `0x010d6570`: `taskDelay(0x64)` — gated by AtaPresent / partition setup

**System clock chain:**

```
sysClkConnect (0x010e08a4)
  → excIntConnect(0x900, sysClkInt)         vector 0x900 = DEC
sysClkRateSet(60)
  → 60 Hz rate
sysClkEnable (0x010de598)
  → vxDecSet(rate_value)                    arms PowerPC SPR 22 (DEC)
on DEC underflow → 0x900 → sysClkInt → vxTicks++ → wakes tickQ
```

**No MPC5200 GPT/Slice-Timer/TCR involvement in the system tick.**
Our SLT timer driving EXT (vector 0x500) is something else — possibly
auxiliary clock, possibly board-watchdog. The kernel's primary tick
is DEC.

**Suspected cause for DEC non-delivery:** vector base address
mismatch. If `MSR[IP]=1` (high vectors at `0xFFF00xxx`) when DEC
fires, CPU jumps to `0xFFF00900` which is unmapped. VxWorks expects
to install handlers at `0x00000900` (low vectors). Need to verify
`MSR[IP]` is cleared before DEC arms.

## Agent B — usrRoot linear trace

First ~30 instructions of `usrRoot` (`0x010d60f8`) — I'll annotate
each `bl` with target symbol:

```
010d6108: bl memInit           (0x010a7fc0)
010d6114: bl memAddToPool      (0x010a9870)
010d611c: bl usrMmuInit        (0x010d6048)
010d612c: bl sysClkConnect     (0x010e08a4)  also calls sysHwInit2
010d6134: bl sysClkRateSet     (0x010de5e4)  60 Hz
010d613c: bl sysClkEnable      (0x010de598)  arms DEC
010d6140: bl i2cLibInit        (0x0100168c)  semMCreate only
010d6144: bl usrI2CMasterInit  (0x010ebefc)  ← BLOCKER ENTERS HERE
```

`usrI2CMasterInit` calls `i2cMasterDevLoad` (`0x01001764`) which:

```
1001800: lwz  r3, -10552(r9)    ; r3 = global I2C semaphore @ 0x011ed6c8
1001808: li   r4, -1            ; WAIT_FOREVER
100180c: bl   semTake           ; 0x010c66f0
1001810: mr   r3, r29
1001814: mtlr r27               ; r27 = master->probeFn
100181c: blrl                   ; ← indirect call to MPC5200 I²C probe
```

`semTake` returns immediately (sem is fresh). The actual hang is the
**indirect call at 0x0100181c** — dispatches into the MPC5200 I²C
master driver's bus-probe function. This polls the I²C status
register (MBSR at `MBAR+0x3D0C` for I2C1, `MBAR+0x3D4C` for I2C2)
waiting for "transfer complete" / "address acknowledged" bits.

For I2C2 (X1226), our state machine handles this. For I2C1, our
stub returns 0x82 (MCF+MIF set, RXAK=0) — RXAK=0 means "slave ACKed"
which makes the BSP think a slave responded, then it tries to read
data and gets garbage, then... loops.

## Concrete fix recommendation (Agent B)

**A. Stub I2C1 properly — make it always NACK** (no slave responding):

- `MBSR` (`0x3D0C`) read returns `0x83000000` (bits 7+1+0 = MCF+MIF+RXAK).
- `MDR` (`0x3D10`) read returns `0xFF000000` (open-bus).
- Writes swallow.

This will make `i2cMasterDevLoad` see "no slave" and continue. We'd
expect to reach the first `printf` (which would surface in our PSC
console logging once we get there).

**B. (later) Real I2C1 EEPROM model** — if the BSP needs to actually
read board-ID from I2C1 in addition to I2C2, we'd extend the X1226
state machine to also serve I2C1. But Agent B's recommendation is
that NACK-everything is sufficient for the bootrom to declare "no
slave" and move on.

## Diagnostic confirmation: DEC not firing

```
$ qemu-system-ppc -d int (bootrom)
  ===exception types===
        2 => DSTLB
      534 => EXTERNAL    ← our SLT timer
        1 => HV_EMU
        3 => IFTLB
        0 => DECR        ← absent!
```

Compared to vxworks.out runs (Daniele's diagnosis) which had hundreds
of DECR events. So DEC delivery to the bootrom kernel is broken in
some way. Two paths to investigate AFTER the I2C fix:

1. Check if MSR[IP]=0 when bootrom is running (low vectors).
2. Check if `vxDecSet` is being called and what value is being
   loaded into the DEC SPR.

## Verification result — Agent B's hypothesis was wrong about *which* line wedges

We applied the I2C1 NACK fix per Agent B's recommendation, then
instrumented every I2C1 register access. **Result: zero I2C1 reads
observed in the bootrom run.** Same for I2C2. The BSP **never reaches
`usrI2CMasterInit`**. So the wedge is *earlier* in the linear flow
than Agent B identified — somewhere between `memInit` and
`usrI2CMasterInit`.

Aggressive NIP sampling (every tick, log when NIP not in idle) over
10 seconds caught only **one** non-idle sample at `workQDoWork`
(`0x010ce4d4`). That's the kernel work-queue processor, running
when it has items to process. So the kernel scheduler IS alive and
processing work — but the boot task is blocked.

## The real root cause: DEC not firing for the bootrom

`-d int` log over 10s of bootrom run:

```
        2 => DSTLB
      534 => EXTERNAL    ← our SLT timer (60 Hz)
        1 => HV_EMU
        3 => IFTLB
        0 => DECR        ← NEVER FIRES
```

For comparison, Daniele's earlier vxworks.out runs had hundreds of
DECR events. Same QEMU, same CPU model. So bootrom's DEC delivery
is broken in some way we don't understand yet.

If the bootrom uses DEC for sysClk (per Agent A) and DEC never
underflows, **every `taskDelay()` blocks forever**. usrRoot has
multiple taskDelay calls upstream of usrI2CMasterInit:

- After memInit / memAddToPool / usrMmuInit, `usrRoot` may call
  routines that themselves taskDelay.
- The conditional `taskDelay(rate/2)` at `0x010d62ac` is one
  candidate.

Possible reasons DEC doesn't fire:

1. **DEC SPR is never written.** QEMU's e300 model may rely on the
   guest writing DEC SPR to start the counter. If bootrom hasn't
   reached `vxDecSet` (`0x01038c1c`, `mtspr 0x16, r3`) yet, DEC
   stays at reset value (possibly 0 or doesn't tick).
2. **MSR[IP]=1** means DEC vector at `0xFFF00900` (high) instead of
   `0x00000900` (low). If guest hasn't cleared IP yet, DEC fires
   into bad address.
3. **vxworks.out has DEC working because its sysHwInit does
   something the bootrom's doesn't** — different startup code paths,
   even though both eventually need DEC.

## Strategic implications

- "Run the bootrom directly" route needs DEC to work. Without it,
  even fixing I2C1, the next blocker is taskDelay → infinite wait.
- "Run vxworks.out" route already has DEC working. The wedge there
  is "all app tasks blocked on FTP boot" (Daniele's diagnosis), not
  DEC.
- The DEC issue is **QEMU-side, not BSP-side** — probably a CPU
  init/reset detail. Worth investigating but beyond this session.

## Updated next steps

Options going forward, in increasing order of effort:

1. **Pivot back to vxworks.out** — DEC works there. Continue with
   BestComm executor for FTP-boot path. Drop bootrom direct-load
   experiment.
2. **Investigate DEC delivery for bootrom** — find what vxworks.out
   does that the bootrom doesn't. Possibly a single mtspr DEC
   somewhere. Could be simple if we find it.
3. **Hack DEC firing** — manually call `cpu_ppc_decr_excp(cpu)` from
   our SLT timer alongside EXT. Ugly but unblocks taskDelay quickly.
4. **TFFS cold-start pivot** — model NAND, populate from Røye2 dump,
   bootline `tffs=0,0(0,0)`. Skip the network-boot path entirely.
   ~3 days but most production-realistic.

## Files referenced

- `/tmp/bootrom_extracted.elf` — un-stripped bootrom, full symbols
- `BSP_embedded_bootrom_findings.md` — bootrom symbol context
- `BSP_psc_console_findings.md` — confirmed BSP wedges before printf
- Agent reports preserved in conversation history
