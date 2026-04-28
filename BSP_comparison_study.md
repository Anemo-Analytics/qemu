# vxworks.out vs bootrom comparison study

**RECOMMENDED NEXT ACTION:** Add NIP-trip breakpoints in `mac_newworld.c` for the seven bootrom usrRoot stations (`0x010d6108`, `0x010d6114`, `0x010d611c`, `0x010d612c`, `0x010d6134`, `0x010d613c`, `0x01038c1c`) plus `usrMmuInit` entry (`0x010d6048`) and run the bootrom for 4 s — the last station logged identifies the wedge step within ½ day.

## Tl;dr

The two binaries share an identical DEC arming code path (`sysClkEnable → vxDecSet @ mtspr 22`); vxworks.out reaches it (291 DECR / 4 s) and the bootrom does not (0 DECR / 4 s). The difference is **not** that DEC delivery is broken in QEMU and **not** that MSR[IP] is wrong — entry MSR is `0x2002` for both binaries and the bootrom never sets bit `0x40`. The bootrom wedges somewhere in the small window `usrRoot+0x10..+0x44` (memInit / memAddToPool / usrMmuInit / sysClkConnect / sysClkRateSet) **before** `sysClkEnable` is ever called, which is why DEC is never armed.

## Reset state

| Field | vxworks.out @ `0x00100000` | bootrom @ `0x01000000` |
|---|---|---|
| First insn | `xor r5,r5,r5` | `xor r5,r5,r5` |
| MSR after `mtmsr r5` | `0` | `0` |
| MSR after `mtmsr r3` (`0x2000`) | `0x2000` (ME) | `0x2000` (ME) |
| MSR after `mtmsr r4` (`0x2002`) | `0x2002` (ME\|RI, EE=0, IP=0, FE0=0) | `0x2002` (same) |
| Bare `rfi` written to `*0x900` | yes (at `+0x13c`) | yes (at `+0x134`) |
| Vector base | low (`MSR[IP]=0`) | low (`MSR[IP]=0`) |
| HID0 cache invalidate | yes | yes |
| BAT/TLB clear | implicit (vxworks.out skips early) | yes (`bl sysClearBATs`, `sysInvalidateTLBs`, `sysClearSegs`) |

Both binaries enter with EE=0, IP=0, vectors at `0x0000_0nnn`. DEC SPR is left at its reset value (PowerPC architectural default = all-ones / freerunning, but never explicitly programmed at entry).

The bootrom does additional **BAT / TLB / segment-register clears** at `+0x40..+0x48` that vxworks.out's `_sysInit` does not run (vxworks.out's `_sysInit` jumps to a shorter init dispatcher at `0x10740c` which does the BSS/data init differently).

## First 100 ms behavior

Side-by-side view of what each binary touches, derived from stderr captures.

| Phase | vxworks.out | bootrom |
|---|---|---|
| BSS / data init | yes | yes (via `usrInit+0x68` bzero) |
| `excVecInit` (install 0x500 / 0x900 / 0x1400 stubs) | yes | yes |
| `sysHwInit` chain | yes | yes |
| PSC1 / PSC4 / PSC5 init | yes | yes |
| BestComm SDMA register init | yes | yes |
| FEC `ECR=0` (reset prep) | yes | yes |
| FEC MAC programming (PALR/PAUR `+0x024 / +0x028`) | yes (`+0x040 = 0x6f820000`) | **no** |
| FEC MII speed (`+0x044`) | yes | **no** |
| FEC MII command (`+0x140`) | yes | **no** |
| FEC `ECR.ETHER_EN` (`+0x024 = 2`) | yes | **no** |
| FEC RX DES active (`+0x010 = 0x01000000`) / TX active (`+0x014`) | yes | **no** |
| BestComm BSS pointers (FEC TX/RX cfg @ `0x008CFC00 / 04`) | yes (`0x009a3bbc / 0x009a4c68`) | **no** |
| KeySwitch bypass patches applied at `0x12d390 / 0x12ae60` | yes | yes (logged identically) |
| EXT vector populated (`*0x508`) | yes (`0x48138121`) | yes (`0x490006bd`) |
| EXT delivery armed | yes — 208 EXTERNAL events / 4 s | yes — 178 EXTERNAL events / 4 s |
| DECR fires | **291 / 4 s** | **0 / 4 s** |
| stderr line count | 576 | 82 |

The bootrom emits ~7× fewer log lines and stops emitting them at the FEC `+0x024` write. Everything after that — MII frame issuance, PHY register access, MAC programming, ECR.ETHER_EN, BestComm BSS pointer plant — vxworks.out does and the bootrom does not.

This is consistent with the bootrom never reaching `sysNetHwInit`'s second phase / never running `m5200FecEndLoad`, which itself is consistent with the bootrom never reaching `usrRoot`'s post-`sysClkEnable` body (which is where the network-boot path starts spawning).

## Kernel scheduling entry

Both binaries reach `kernelInit` and spawn `usrRoot` — both are in the kernel idle/ready-queue regime, both have the scheduler alive.

| Symbol | vxworks.out addr | bootrom addr |
|---|---|---|
| `kernelInit` | `0x002fc16c` | `0x010c33c8` |
| `usrKernelInit` | `0x00306104` | `0x010d4e54` |
| `usrInit` body | `0x0010740c` (compact) | `0x010d8718` (full) |
| `usrRoot` | (not symboled, but exists) | `0x010d60f8` |
| `tickAnnounce` | not signature-matched | `0x010cbb18` |
| `windTickAnnounce` | not signature-matched | `0x010ccef8` |

Existing finding (Daniele's WIND_TCB walk): both kernels eventually idle in `intUnlock` via `reschedule` once `usrRoot` blocks. Difference: vxworks.out's `usrRoot` makes it past `sysClkEnable` (so DEC fires and the kernel actually has work to schedule); the bootrom's `usrRoot` never gets there.

## Divergence point — exact addresses

The two boot paths produce identical externally-visible behavior up to the FEC `ECR=0` write (gate 2 sequence). They diverge inside `usrRoot`.

vxworks.out's `usrRoot` runs to completion of all the post-`sysClkEnable` work: `i2cLibInit`, `usrI2CMasterInit`, FEC MAC programming, BestComm BSS pointer planting. It then sits in the idle/scheduler loop with `tBoot` blocked on FTP boot — but DEC fires fine.

The bootrom's `usrRoot` (`0x010d60f8`) wedges somewhere in this window:

| Station | Addr | Symbol | Likelihood it is the wedge | Reason |
|---|---|---|---|---|
| 1 | `0x010d6108` | `bl memInit` | low | Pure RAM bookkeeping; vxworks.out also runs an equivalent. |
| 2 | `0x010d6114` | `bl memAddToPool` | low | Same — just inserts a free chunk. |
| 3 | `0x010d611c` | `bl usrMmuInit` (target `0x010d6048`) | **HIGH** | First place bootrom does something vxworks.out's compact dispatcher does not. If it touches a BAT, segment register, or TLB in a way that mismatches our QEMU's `mac99 / mpc5200` setup we'd take a silent fault into the bare-`rfi` at `*0x900` and loop. Note vxworks.out's `_sysInit` does not run `sysClearBATs` etc, but the bootrom does — by the time `usrMmuInit` runs, the bootrom has *already cleared* the MMU state and is now trying to install something. |
| 4 | `0x010d612c` | `bl sysClkConnect` (target `0x010e08a4`) | medium | Calls `sysHwInit2` (idempotent gate at `[0x01204fcc]`), enables IRQs `0x27 / 0x28` (SDMA Tx/Rx). If our SDMA stub never asserts those IC bits and the kernel `semTake`'s on a SDMA-completion semaphore upstream of `sysClkEnable`, we wedge. Less likely because `intEnable` only arms — it doesn't take. |
| 5 | `0x010d6134` | `bl sysClkRateSet(60)` (target `0x010de5e4`) | low | Pure arithmetic on `[0x011f2430]` (sysCpu/bus clock). If `[0x011f2430]==0`, divides-by-zero produce a `PROGRAM` exception — we'd see it in `-d int`. We don't. |
| 6 | `0x010d613c` | `bl sysClkEnable` (target `0x010de598`) | n/a | If we ever get *into* this, DEC arms via `vxDecSet`. Definitionally not the wedge. |
| 7 | `0x010d6140 / 0x010d6144` | `bl i2cLibInit / usrI2CMasterInit` | n/a — past the DEC arming window | Earlier finding (Agent B I2C1 NACK) was wrong about which line wedges; we never see I2C1 / I2C2 reads from the bootrom run, confirming we don't even reach line 7. |

**Ranked top suspects:**

1. `usrMmuInit @ 0x010d6048` — the highest-confidence candidate. Bootrom-specific code that vxworks.out's compact dispatcher does not run. If our QEMU MMU setup conflicts with what `usrMmuInit` expects, the fault path is "silent rfi loop" (because the only thing at `0x900` is the bare `rfi` installed at entry).
2. `sysClkConnect → sysHwInit2 → intEnable(0x27 / 0x28)` — second-most-likely. SDMA IRQ arming on a stubbed peripheral.
3. `excIntConnect(0x900, sysClkInt)` inside `sysClkConnect` — if its bcopy stub-builder loops on misaligned destination or our `excVecBase` (`0x0120721c`) read returns garbage.

## DEC SPR programming

Identical code in both binaries.

| Item | vxworks.out | bootrom |
|---|---|---|
| `vxDecSet` (`mtspr 22, r3 ; blr`) | `0x00207a18` | `0x01038c1c` |
| `vxDecReload` | `0x00207a40` | `0x01038c2c` |
| Only arming caller | `sysClkEnable` (`0x0011aae0`) tail-call | `sysClkEnable` (`0x010de598`) `b vxDecSet` (offset `+0x20`) |
| Only re-arm caller | `sysClkInt` (`0x00117fd0`) | `sysClkInt` (`0x010db170`) |
| `excIntConnect(0x900, sysClkInt)` | yes (`0x0011ce58`) | yes (inside `sysClkConnect`) |

**Why vxworks.out fires DEC and bootrom does not:** vxworks.out reaches `sysClkEnable`; the bootrom does not. There is exactly one DEC-arm site in either binary, and the bootrom never executes it. Once `vxDecSet` runs, DEC underflow drives `sysClkInt → vxTicks++ → wakes tickQ → reschedule`. Without it, DEC is never written and never underflows.

vxworks.out's 291 DECR over 4 s ≈ ~73 Hz, a hair above the 60 Hz rate set by `sysClkRateSet(60)` — consistent with DEC reload happening from `sysClkInt`'s `vxDecReload` plus a small amount of additional re-entry from the EXT handler chain. Confirms DEC is fully functional in QEMU for this CPU model.

## Why MSR[IP] is REFUTED as a candidate

- Bootrom entry sets `MSR := 0x2002` at `0x0100012c`. Bit `0x40` (IP) is clear.
- The only later `mtmsr` we found that adjusts MSR via `ori` immediate is the `vxMsrGet ; ori r3,r3,0x1000 ; vxMsrSet` tail of `excVecInit` — bit `0x1000` is FE0, not IP.
- Disassembly of all 41 `mtmsr` sites in either binary shows none install bit `0x40`.
- Empirically: vxworks.out runs through the exact same MSR sequence and DEC fires for it. If MSR[IP] were the issue it would also affect vxworks.out.

Refuted.

## Why "DEC delivery broken in QEMU" is REFUTED as a candidate

- vxworks.out, on the same QEMU build / CPU model / machine model, fires DEC 291 times in 4 s.
- The `mtspr 22` opcode and the 0x900 vector handler chain are exercised by both binaries' identical code shapes.
- The bootrom logs zero DECR not because the CPU model fails to deliver DEC, but because the bootrom never *writes* DEC. With DEC at its reset value and never reloaded by `vxDecSet`, no underflow event is generated for QEMU to deliver.
- This was previously suspected in `BSP_usrroot_blocker_findings.md` but the static A2 analysis pinpointed the single arming site and the dynamic stderr captures confirm the bootrom never reaches `sysHwInit2`'s SDMA IRQ enable phase let alone `sysClkEnable`.

Refuted. The blocker is bootrom-side, not QEMU-side.

## Three plausible outcomes (per the plan, scored)

### Outcome 1 — single missing register / SPR / stub return value

**Probability: 55%.** Most-likely shape: a register read in `usrMmuInit` or `sysHwInit2` returns zero from our QEMU stub, and the BSP loops or faults silently.

**Evidence:**
- Bootrom is *much* closer to a working boot than the runtime ever was (it has full symbols + the BAT/TLB clear sequence + a real `usrRoot`).
- The wedge window is 5 instructions wide.
- We have explicit hand-off from a known-good register-write log (last good = FEC `ECR=0`) to silent loop, suggesting one specific stubbed device or SPR.

**Fix shape (½ day):** identify the wedge step via NIP breakpoints, add the specific MMIO/SPR behavior, retest. If it's `usrMmuInit`, look for the BAT/SDR1/SR write and confirm our QEMU CPU state matches. If it's `sysClkConnect → sysHwInit2`, check `[0x01204fcc]` gate and `intEnable(0x27 / 0x28)`. If it's `excIntConnect`, audit `excVecBase=0x0120721c` read path.

### Outcome 2 — architectural difference (BAT / TLB / segment register state)

**Probability: 30%.** Bootrom does `sysClearBATs / sysInvalidateTLBs / sysClearSegs` at `+0x40..+0x48`, then later runs `usrMmuInit`. vxworks.out skips both. If our QEMU `mac99 / mpc5200` reset state has BATs / SDR1 in a configuration `usrMmuInit` cannot tolerate, we silent-fault.

**Evidence:**
- Bootrom does extra MMU init that vxworks.out does not.
- `0x900` has only the bare `rfi`, so a DEC-or-other exception during MMU setup loops invisibly.
- We don't see any `PROGRAM` or `DSI` events in the bootrom `-d int` count, but we *do* see `IFTLB` (3) and `DSTLB` (2) hits — small but non-zero, consistent with TLB-miss handlers running.

**Fix shape (1 day):** dump CPU SPR state at entry vs. what `usrMmuInit` expects, possibly install minimal BAT mapping in QEMU before bootrom runs, or augment our exception trap to log every TLB miss target so we can see whether `usrMmuInit` is faulting on a specific page.

### Outcome 3 — no clear blocker; pivot to TFFS cold-start

**Probability: 15%.** If the wedge is "Outcome 1" but the missing register turns out to be one we can't reasonably model (e.g. the bootrom is reading a hidden chip-specific status that depends on real silicon state machines we'd need to reverse-engineer over weeks).

**Evidence:**
- Per existing `BSP_usrroot_blocker_findings.md` we already know the cold-start path via `tffs=0,0(0,0)` is fully supported by the bootrom's symbol table — `tffsDrv`, `tffsDevCreate`, `tffsBlkRd`, `tffsBlkWrt`, `tffsDevFormat` all present.
- Existing `Røye2` dump can be staged into a NAND model.
- This skips the entire FEC + FTP path that gates 3–6 are still chasing.

**Fix shape (3 days):** model NAND in QEMU, populate from `Røye2` dump, set bootline to `tffs=0,0(0,0)`, jump straight to gate 7 (filesystem mount).

## Recommended next session task

**Concrete bounded next step:** instrument bootrom `usrRoot` in `mac_newworld.c` with NIP-trip breakpoints at the eight stations below; run the bootrom for 4 s; the last station logged is the wedge step's predecessor.

NIP breakpoints to add (bootrom addresses, after the `+0xF00000` offset is *not* applied — bootrom loads at `0x01000000`):

| Addr | Meaning |
|---|---|
| `0x010d6108` | `usrRoot+0x10` — about to call `memInit` |
| `0x010d6114` | `usrRoot+0x1c` — about to call `memAddToPool` |
| `0x010d611c` | `usrRoot+0x24` — about to call `usrMmuInit` |
| `0x010d6048` | `usrMmuInit` entry |
| `0x010d612c` | `usrRoot+0x34` — about to call `sysClkConnect` |
| `0x010d6134` | `usrRoot+0x3c` — about to call `sysClkRateSet` |
| `0x010d613c` | `usrRoot+0x44` — about to call `sysClkEnable` |
| `0x01038c1c` | `vxDecSet` entry — definitive proof of DEC arm |

Suggested instrumentation pattern (matches existing tick / KeySwitch hook style in `mac_newworld.c`): in our SLT timer / NIP-sampling hook, compare current NIP against each station; on first hit log `MPC5200: usrRoot reached station <name> @ <addr>` and clear the trip flag. After 4 s of run, count the highest station logged.

**Expected outcomes:**

- If we hit `0x010d611c` but never `0x010d6048` or `0x010d612c` → wedge is in the `bl usrMmuInit` call itself. Investigate MMU.
- If we hit `0x010d6048` (usrMmuInit entry) but never `0x010d612c` → wedge is *inside* `usrMmuInit`. Disassemble it.
- If we hit `0x010d612c` but never `0x010d613c` → wedge is in `sysClkConnect / sysHwInit2 / excIntConnect`. Check IRQ arming on stubbed peripherals.
- If we hit `0x010d613c` but never `0x01038c1c` → wedge is in `sysClkRateSet`. Check `[0x011f2430]` (sysCpu/bus clock).
- If we hit `0x01038c1c` and DEC still doesn't fire → genuine QEMU DEC delivery bug (would contradict the vxworks.out evidence; very unlikely).

**Time budget:** 4 hours coding + 1 hour analysis. Should produce a definitive verdict on which of Outcomes 1, 2, 3 we are in.

## Open questions / things this study did NOT resolve

1. **What does `usrMmuInit @ 0x010d6048` actually do?** Static analysis identified it but did not disassemble the body. A2 listed it as a candidate but didn't enumerate its BAT / SDR1 / segment-register writes. Next session should disassemble.
2. **What is the value of `[0x011f2430]` (sysCpu / bus clock) at the time `sysClkRateSet` runs?** Static says "could be 0 → divide produces 0 → DEC fires continuously, not silence." Confirmed not the silence cause but worth pinning down.
3. **Why does the bootrom `-d int` count show 3 IFTLB and 2 DSTLB events?** Small but suspicious — vxworks.out doesn't show them. May indicate the bootrom is taking TLB misses during `usrMmuInit` that vxworks.out skips.
4. **`sysHwInit2`'s IRQ table memset at `0x12054e0c`** — does our QEMU back that BSS region the way the bootrom expects? Worth a sanity check.
5. **Could the SDMA-IRQ-arming in `sysHwInit2` (0x27 / 0x28) be the wedge if a downstream `semTake` is hidden in `sysClkConnect`?** Static found `intEnable(0x27 / 0x28)` but did not trace whether anything blocks on those IRQs upstream of `sysClkEnable`.
6. **Is there a `usrInit`-level wedge we're missing?** A2 walked `usrInit` to `kernelInit → usrRoot`. We assume `usrRoot` actually starts running. The single non-idle NIP we caught in the existing `BSP_usrroot_blocker_findings.md` (sample at `workQDoWork = 0x010ce4d4`) suggests the kernel scheduler is alive and `usrRoot` did get started — but a longer NIP-sampling run is the cheap way to confirm.
7. **Does the KeySwitch bypass at `0x12d390 / 0x12ae60` apply to bootrom code?** Those are *runtime* addresses. The bootrom has its own m5200Fec driver at `0x010e4cf0`. If a CT296 KeySwitch gate exists in the bootrom too, we'd need a second patch. Not the current wedge (we don't even reach FEC config), but worth noting for downstream.
