# bootrom_extracted.elf static analysis (Agent A2)

## Binary metadata

| Item | Value |
|---|---|
| File | ELF 32-bit MSB PowerPC, statically linked, **NOT stripped** |
| Entry | `0x01000000` |
| LOAD | `0x01000000..0x011F35C0` (FileSiz `0x1f35c0`, MemSiz `0x20f294`), RWE |

Disassembly via capstone (binutils PowerPC unavailable on host).

## Symbol table excerpt (DEC + scheduling)

```
01000000 T _sysInit
01001058 T intLock
0100106c T intUnlock
01038ad8 T vxMsrGet
01038ae0 T vxMsrSet
01038c1c T vxDecSet           ; mtspr 22, r3
01038c24 T vxDecGet
01038c2c T vxDecReload
010a2cd0 T excInit
010c33c8 T kernelInit
010cbb18 T tickAnnounce
010ccef8 T windTickAnnounce
010d4e54 T usrKernelInit
010d60f8 T usrRoot             ; the boot task
010d8718 T usrInit
010db170 t sysClkInt           ; vector 0x900 ISR
010de598 T sysClkEnable        ; tail-calls vxDecSet
010de5e4 T sysClkRateSet
010e07fc T sysHwInit2
010e08a4 T sysClkConnect       ; calls excIntConnect(0x900, sysClkInt)
010e0b60 T sysHwInit
010f2154 T excVecInit
010f27f8 T excIntConnect
0120721c B excVecBase
011f1c70 d decCountVal
011f2418 d sysClkConnectFirstTime
011f2428 d sysClkTicksPerSecond
011f2430 d (bus clock)
01204fcc b initialized$15628   ; sysHwInit2 done-flag
012054b0 b sysClkRunning
```

## Entry point sequence (first ~50 insns)

```
0x01000000  xor r5,r5,r5 ; mtmsr r5         ; MSR=0
0x01000010  save boot-arg, HID0 cache toggles
0x01000040  bl sysClearBATs
0x01000044  bl sysInvalidateTLBs
0x01000048  bl sysClearSegs
0x0100006c  mtmsr r3 (=0x2000)              ; FP enable
0x01000098  bl ifpdrValue                   ; FP reg init
0x0100012c  mtmsr r4 (=0x2002)              ; FP|RI, IP=0, EE=0
0x01000134  stw 0x4c000064, 0x900(0)        ; bare rfi at DEC vector
0x01000160  b usrInit
```

**At entry MSR=0x2002. MSR[IP]=0, MSR[EE]=0.** Vectors at low addresses.

## Call graph from entry

```
_sysInit -> usrInit
usrInit (0x010d8718)
  +0x68 -> bzero (BSS)
  +0x78 -> intVecBaseSet
  +0x7c -> excVecInit (install vector stubs)
  +0x80 -> sysHwInit (board init)
  +0x84 -> usrKernelInit (init kernel libraries)
  +0x8c -> cacheEnable
  +0xa8 -> sysMemTop
  +0xdc b  kernelInit              ; tail-call; spawns usrRoot

kernelInit (0x010c33c8)
  -> intLockLevelSet
  -> bfill (zero stacks)
  -> windIntStackSet
  -> taskInit (creates usrRoot tcb)
  -> taskActivate (schedules usrRoot)
  ; falls into idle loop

usrRoot (0x010d60f8)              ; the boot task
  +0x10  -> memInit
  +0x1c  -> memAddToPool
  +0x24  -> usrMmuInit            ; (0x010d6048)
  +0x34  -> sysClkConnect(handler, 0)
  +0x3c  -> sysClkRateSet(60)
  +0x44  -> sysClkEnable          ; tail-calls vxDecSet -> mtspr 22
  +0x48  -> i2cLibInit
  +0x4c  -> usrI2CMasterInit      ; (per existing finding, never reached)
```

## DEC SPR programming (CRITICAL)

```
0x01038c1c  vxDecSet:    mtspr 0x16, r3 ; blr
0x01038c24  vxDecGet:    mfspr r3, 0x16 ; blr
0x01038c2c  vxDecReload: mfspr r5, 0x16 ; ... ; mtspr 0x16, r5
```

Callers of `vxDecSet`:
```
0x010de5b8  b vxDecSet  (sysClkEnable+0x20)   ; THE arming site
0x010db238  bl vxDecReload (sysClkInt+0xc8)    ; re-arm in ISR
```

**The only path that arms DEC is `sysClkEnable → vxDecSet`. Reached only from `usrRoot+0x44`.**

If usrRoot blocks before `0x010d613c`, **DEC is never written and never underflows** → 0 DECR exceptions. **This perfectly explains "0 DECR events" in `-d int`.**

## Exception vector install

`excVecInit` (called from `usrInit+0x7c`):
- BSS `excExtendedVectors=0` → "regular vectors" path
- Loops over vector table at `0x111bf34` (.data); for each, calls `excConnectVector(vec, handler, param)`
- Helper bcopies 76-byte stub to `[excVecBase + vec_offset]` and patches handler immediates via `lis/ori`
- `excVecBase=0` → vectors land at `0x0000_0nnn`. **DECR slot `0x900`.**
- Final tail: `vxMsrGet ; ori r3,r3,0x1000 ; vxMsrSet` — **this is FE0 bit (`0x1000`), NOT IP (`0x40`)**. **MSR[IP] is never set anywhere.** (Earlier MSR[IP] hypothesis = REFUTED.)

## windTickAnnounce / scheduler entry

```
windTickAnnounce (0x010ccef8):
  prologue
  r9 = [0x011f3960]            ; vxTicks
  r11:r12 = [0x011f3968:6c]    ; sysAbsTicks (64-bit)
  vxTicks++ ; absTicks++
  store back
  blrl                         ; tail-call computed handler chain
```

Path: **DEC underflow → 0x900 vector → sysClkInt → vxTicks++ → wakes tickQ → reschedule**. With DEC unfired, none of this happens.

## sysHwInit walk

`sysHwInit @ 0x010e0b60` (called from usrInit+0x80, BEFORE kernelInit):
1. `sysCpuCheck`, `vxPvrGet` ×3
2. Write CDM `0xF000020C` (CFG)
3. Write `0xF0000210` (CCS divider) = `0x15555`
4. Write `0xF0000214` = `0xfffff` (or `0xffcff`)
5. Write ICTL `0xF0001F70` = `0x1A`
6. Write ICTL `0xF0001F40` = `0x8000`
7. Series of stb writes to `[0x011f2578 + 0x3c..0x4c]` (IRQ priority table)
8. `bl m5200IntrInit`
9. `[0xF0001F40] |= 0x2000`
10. `bl initCS`, `sysGpioHwInit`, `sysGPTGpioInit`, `sysSerialHwInit`, `sysSdmaInit`, `sysPhysMemTop`, `sysNetHwInit`
11. Re-write `0xF0000210 = 0x15555`

**sysHwInit does NOT touch DEC, MSR[IP], or MSR[EE].**

## sysHwInit2 walk

`sysHwInit2 @ 0x010e07fc` (called from sysClkConnect+0x28):
- Gate: `[0x01204fcc]` (initialized$15628). If set, return.
- Set gate=1
- `bl sysSerialHwInit2` (PSC console phase 2)
- Zero-init 0x38×8 IRQ table at `0x12054e0c`
- `intConnect(0x27, ...)`, `intConnect(0x28, ...)` (SDMA Tx/Rx via IC)
- `intEnable(0x27)`, `intEnable(0x28)`
- Tail: `b sysAtaInit`

Vectors `0x27/0x28` are MPC5200 IC (interrupt controller) bits, not exception offsets — they route via the External-IRQ dispatcher (exception 0x500). DEC (exc 0x900) is untouched.

## Notable observations / divergence candidates

1. **DEC arming is `usrRoot+0x44` only.** Existing finding says wedge is BEFORE `usrI2CMasterInit (+0x4c)`, so wedge is in the small window:

   ```
   0x010d6108 memInit
   0x010d6114 memAddToPool
   0x010d611c usrMmuInit         <-- candidate; could fault on uninit MMU?
   0x010d612c sysClkConnect      <-- calls sysHwInit2 (idempotent), excIntConnect(0x900)
   0x010d6134 sysClkRateSet(60)  <-- divides by sysCpu (bus clock)
   0x010d613c sysClkEnable       <-- arms DEC; if reached, DEC fires
   ```

2. **MSR[IP] hypothesis REFUTED.** Entry sets MSR=0x2002 (IP=0). No `ori` constant sets bit `0x40` anywhere. The `ori r3,r3,0x1000` in excVecInit's tail is FE0, not IP.

3. **`sysHwInit2` enables IRQs 0x27 and 0x28 (SDMA Tx/Rx)** — if SDMA stub never raises those IRQs but kernel does `semTake` on SDMA-completion semaphore, we'd block. Not visible statically but matches symptom.

4. **`sysClkRateSet` divides `[0x011f2430]` (sysCpu/bus clock) by rate.** If sysCpu=0 → divw produces 0 → DEC armed with 0 = continuous fire (storm, NOT silence). So divider misuse is not the cause of "0 DECR".

5. **Bare `rfi` at *0x900 hand-installed by entry+0x13c**: pre-excVecInit safety net. If DEC fires before vector install, CPU just rfi's. This is fine.

6. **Bottom line:** all the code needed for DEC to fire exists. The reason it never fires is that `usrRoot` wedges in `memInit..sysClkRateSet` window, BEFORE `sysClkEnable`. **Top suspects: `usrMmuInit` faulting, `sysHwInit2→intEnable` arming an IRQ that fires on a stubbed peripheral, or `excIntConnect(0x900)` looping in bcopy.**

## QEMU instrumentation candidates

NIP breakpoints at:
- `0x010d6108` (memInit entry)
- `0x010d6114` (memAddToPool entry)
- `0x010d611c` (usrMmuInit entry)
- `0x010d612c` (sysClkConnect entry)
- `0x010d6134` (sysClkRateSet entry)
- `0x010d613c` (sysClkEnable entry)
- `0x01038c1c` (vxDecSet entry — definitive proof of DEC arm)
