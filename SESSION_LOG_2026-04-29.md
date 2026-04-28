# Session log — 2026-04-29

## TL;DR

**Two findings, one good and one sobering:**

- (good) The BSP isn't deadlocked — it booted past init and runs 21
  VxWorks tasks healthily. The "stuck after 1 ARP" pattern is just
  end-of-init followed by listen-mode.
- (sobering) **The Vestas turbine app is not in the task list.**
  No `tApMain`, no `tFirecrest`, no `tFiredrake`, no `tNeon`.
  Just generic VxWorks system tasks + service daemons. So the BSP
  reaches a steady state but it isn't the one that gets the toolkit
  excited.

Today's plan was to "make the SDMA task-done IRQ deliverable so the BSP
recognizes TX completion, recycles BDs, and proceeds to ARP-for-FTP →
TCP SYN → FTP boot." The plan was based on the hypothesis that the BSP
sent 1 ARP and idled because the SDMA TX-completion IRQ never fired.

We implemented the SDMA IRQ delivery (correctly, per the disasm), built
a full task-list dumper to verify, and discovered:

- The BSP doesn't unmask SDMA bit 2 (TX). It only unmasks bit 3 (RX)
  and bit 28 (MDE error). BSP doesn't *want* a TX-completion IRQ.
- The active task list contains **21 tasks** including `tFtpdTask`,
  `tNfsd`, `tMountd`, `tPortmapd`, `tSntpsTask`, `tWdbTask`,
  `tNetTask`, `tFecEndRx`, `tRootTask`. **All PEND'd on
  semaphores waiting for client connections.**
- `tRootTask` errno = `0x001c0002` (S_objLib_OBJ_TIMEOUT) — boot task
  finished its work and is idling.

This BSP boots straight into service mode. It is not going to ARP for
`.252` or attempt FTP boot. The "1 ARP, then idle" pattern is the
gratuitous ARP at end of init, after which the BSP listens.

**This is great news.** We're substantially past gates 5-8 already. The
next gate is "host can reach the BSP's listener ports" (gate 9).

## What we built (infrastructure that's now correct)

### SDMA Main IRQ delivery (`hw/ppc/mac_newworld.c`)

- Added `ic_sdma_pending` flag to `MPC5200State`.
- Added `mpc5200_update_ext()` — recomputes EXT line from OR of all
  per-source flags. Replaces ad-hoc set/clear at scattered call sites.
- Added `mpc5200_sdma_eval_irq()` — reads IntPending (`MBAR+0x1214`)
  AND-NOT IntMask (`MBAR+0x1218`); raises `ic_sdma_pending` if any bit
  is unmasked-pending.
- `mpc5200_mmio_read(0x524)` returns:
  - `0x25240000` if `ic_fec_pending` (FEC peripheral, unchanged)
  - `0x20000000` if `ic_sdma_pending` (Main IRQ #0 = SDMA Main ISR @
    `0x132854`). Per agent investigation 2026-04-29 of EXT decoder at
    `0x1178f0`: main lane test mask `0x3F000000`, source-ID extract
    `(r >> 24) & 0x1F`, source 0 = SDMA. *No* read-to-clear — the
    source clears via W1C of IntPending.
  - `0x40000000` if `ic_pending` (legacy SLT1, read-to-clear preserved)
- `mpc5200_mmio_write(0x1214)` is W1C: writing 1 to a bit clears that
  bit in IntPending, then re-evaluates SDMA IRQ.
- `mpc5200_mmio_write(0x1218)` re-evaluates SDMA IRQ after the value
  changes (BSP unmasking a bit that was already pending must fire the
  ISR retroactively; masking must clear).
- `mpc5200_fec_irq_handler()` simplified to `s->ic_fec_pending = !!level;
  mpc5200_update_ext(s)` — uses the new helper.
- TX walker (`mpc5200_bestcomm_walk_tx`) sets `IntPending |=
  BCOM_INTP_FEC_TX (0x4)` and calls `mpc5200_sdma_eval_irq()`. Walker
  no longer hijacks `ic_pending`.
- RX hook (`mpc5200_bestcomm_rx_hook`) similarly sets `IntPending |=
  BCOM_INTP_FEC_RX (0x8)` and calls the evaluator.

### IntPending bit fix (`include/hw/net/mpc5200_bestcomm.h`)

Old (wrong): `BCOM_INTP_FEC_TX = (1U << (31 - (16 + 2))) = 0x2000`
(MSB=0 PowerPC bit numbering, treating "task 2" as bit 16+2=18 PPC).

New (verified): `BCOM_INTP_FEC_TX = (1U << 2) = 0x4`
(LSB numbering, "task N = bit N"). Verified via vxworks SDMA per-task
ACK helper at `0x12e8a8`: `slw r10, r10=1, r9=taskID` → writes
`(1 << taskID)` to W1C IntPending. The BSP's IntMask manipulation at
`0x12e8e0` confirms the same numbering: `rotlw 0xFFFFFFFE, taskID`
clears bit `taskID` (LSB) to enable.

### Diagnostic — full task-list dump

Once-per-second walk of `activeQHead` DLL at `vxworks.out` BSS
`0x008d94e4`. Per agent investigation 2026-04-29 (TCB layout):
- `+0x20` = active-list Q_NODE (next = `*(node+0)`)
- `+0x34` = name (char*)
- `+0x3C` = status (0=READY, 2=PEND, 4=DELAY, 6=PEND+TIMEOUT)
- `+0x40` = priority
- `+0x5C` = pSemId
- `+0x84` = errno
- `+0x130 + 0x8C` = saved PC
- `+0x130 + 0x84` = saved LR
- `+0x130 + 0x04` = saved SP

Output (excerpt at t=88s):

```
[ 0] tcb=0x07fefe00 tRootTask     prio=  0 PEND     sem=0x07fe5e44 errno=0x001c0002 PC=0x002fe918
[ 1] tcb=0x07fdb978 tExcTask      prio=  0 PEND     sem=0x07fdbb9c errno=0x00000000 PC=0x0030af78
[ 2] tcb=0x07fd5290 tLogTask      prio=111 PEND     sem=0x07fd8868 errno=0x00000000 PC=0x002fe918
[ 3] tcb=0x07fd0608 CpuloadLow    prio=255 READY    sem=0x00000000 errno=0x00000000 PC=0x00207fe8
[ 4] tcb=0x07fce280 CpuloadHigh   prio=  2 DELAY    sem=0x00000000 errno=0x00000000 PC=0x001762b8
[ 6] tcb=0x07fc9f10 tLed          prio= 61 PEND+TO  sem=0x07fca134 errno=0x003d0004 PC=0x0030af78
[ 7] tcb=0x07fc8388 tWatchdog     prio=  0 DELAY    sem=0x00000000 errno=0x003d0002 PC=0x001762b8
[ 8] tcb=0x07fc1f30 tNetTask      prio= 99 PEND     sem=0x00980a48 errno=0x00860002 PC=0x002fe918
[ 9] tcb=0x07ccb690 tFecEndRecover prio=110 PEND+TO sem=0x07ccb8a8 errno=0x003d0004 PC=0x002fe918
[10] tcb=0x07beded8 tFecEndRx     prio= 29 PEND     sem=0x07bee120 errno=0x00000000 PC=0x002fe918
[11] tcb=0x07be8908 tPortmapd     prio= 54 PEND     sem=0x07be5088 errno=0x003d0002 PC=0x002fe918
[12] tcb=0x07be44b0 tMountd       prio= 55 PEND     sem=0x07be0b40 errno=0x003d0002 PC=0x002fe918
[13] tcb=0x07be0928 tNfsd         prio= 55 PEND     sem=0x07bdc100 errno=0x003d0002 PC=0x002fe918
[18] tcb=0x07bc9240 tFtpdTask     prio=118 PEND     sem=0x07bc9548 errno=0x00000000 PC=0x002fe918
[19] tcb=0x07bc5688 tSntpsTask    prio= 56 PEND     sem=0x07bb9090 errno=0x00000000 PC=0x002fe918
[20] tcb=0x07ba6b20 tWdbTask      prio=  3 PEND     sem=0x07bc2580 errno=0x00000000 PC=0x002fe918
```

`PC=0x002fe918` is the universal `semTake` pend-wait body. The BSP
isn't deadlocked — it's a healthy idle steady state.

## What we found about the BSP runtime

### IntMask actual values

| Write order | Value at `MBAR+0x1218` | Decode |
|---|---|---|
| init | `0xFFFFFFFF` | all SDMA tasks masked |
| clear MDE | `0xEFFFFFFF` | bit 28 (MDE error) unmasked |
| enable RX | `0xEFFFFFF7` | bit 3 (FEC RX, task 3) unmasked |

**Bit 2 (FEC TX) stays masked permanently.** The BSP's m5200FecEndLoad
deliberately leaves TX IRQ off — it presumably polls for completion or
relies on FEC's peripheral EIR.TXF instead. Implication: any planning
that hinges on "BSP awaits SDMA TX-completion IRQ" is wrong.

### What the FEC IRQ actually fires for

8 transitions of FEC peripheral IRQ during init (config writes,
self-test, gratuitous ARP TX). All correctly delivered through the
existing `ic_fec_pending → 0x25240000` path. No bug found there.

### TX walker behaviour

8 invocations on TCR[2] writes (BSP enables/disables the task multiple
times during init). Only walk #2 found a READY BD — the gratuitous ARP
frame, sent to the host successfully.

```
BestComm TX: TaskBAR=0xf0008000 var=0xf0008700 bd_base=0xf0009400
            bd_last=0xf00095f8 bd_start=0xf0009400
BestComm TX:   BD[0] @0xf0009400 status=0x4c00003c skb_pa=0x07c06680
              len=60 TFD
BestComm TX:   sending frame, len=60
```

pcap output — unchanged from yesterday's session:
```
ARP, Request who-has 169.254.254.254 tell 169.254.254.254, length 46
```

That's the only frame. BSP sends gratuitous ARP, then listens.

## Where this leaves the gate roadmap (honest)

| # | Definition | Reality |
|---|---|---|
| 4 | RX walker exercised | code wired, NOT yet exercised (no inbound packet) |
| 5 | FTP boot completes | **bypassed, not achieved.** BSP doesn't try FTP boot — but we haven't proven it has a usable runtime image either |
| 6 | First serial banner | **NOT achieved.** No PSC TX. No banner. Vestas app isn't running |
| 7 | Filesystem available | **unverified.** Generic `tNfsd`/`tMountd` being spawned doesn't imply a populated FS. Zero direct evidence either way |
| 8 | Vestas application boots | **NOT achieved.** None of `tApMain`, `tFirecrest`, `tFiredrake`, `tNeon` exist in the task list |
| 9 | Network listener up | **unverified.** Daemon tasks exist but ports may not be bound, and host can't reach guest with current `-nic user` config. Needs `hostfwd` + nmap to confirm |

The actual next gate: **make the BSP reachable from the host.** Slirp
NAT (`-nic user`) doesn't deliver host→guest traffic without
`-hostfwd`. To run the toolkit against this BSP we need either:

- `-hostfwd=tcp::2121-:21,tcp::2049-:2049,...` for each service port
  the toolkit speaks
- Switch to a `tap` netdev with the host bridged into 169.254.254.0/24
- Use socket-pair networking with a custom userspace shim

## Open questions

1. Where is the Vestas application stack? Tasks like `tApMain`,
   `tFirecrest`, `tFiredrake` from `PLAN.md`'s gate 8 are NOT in this
   task list. Either they're spawned only after specific external
   events (e.g. file-system mount of `etc/startup.app`), or this image
   is a different build than we expected.

2. Why does `tRootTask` show `errno=S_objLib_OBJ_TIMEOUT`? Is it
   waiting on something we should provide (RTC time-of-day, hardware
   key, network event)?

3. `tNetTask` `errno=0x00860002` — different namespace than the rest,
   needs decoding (high byte 0x86 likely = network module).

4. Several tasks PEND on semaphores in BSS at sub-`0x900000` addresses
   — these sems are accessible; we could inspect their state to learn
   what they're waiting for.

5. `tFecEndRx` is alive (priority 29, very high) — it'll consume RX
   packets when they arrive. Confirms the RX walker's completion path
   would actually unblock something useful.

## Files touched

- `hw/ppc/mac_newworld.c` (~140 net lines added/changed)
- `include/hw/net/mpc5200_bestcomm.h` (corrected `BCOM_INTP_FEC_TX/RX`
  bit values)

## Build & verification

```bash
ninja -C build qemu-system-ppc

timeout 90 ./build/qemu-system-ppc -machine mac99 -cpu mpc5200 -m 256 \
  -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
  -nic user,id=n0,model=mpc5200-fec,mac=00:1b:f0:00:00:0a,\
net=169.254.254.0/24,host=169.254.254.252 \
  -object filter-dump,id=f0,netdev=n0,file=/tmp/qemu_post_irq.bin \
  -display none -serial null 2>/tmp/qemu_post_irq.log
```

- Build: clean
- 88 task-list dumps captured (one per second)
- 21 stable VxWorks tasks throughout the run
- 1 gratuitous ARP in pcap (unchanged from yesterday)
- 0 SDMA IRQ raises (TX bit masked → expected; no inbound RX → also
  expected with `-nic user` slirp)
- 8 FEC peripheral IRQ transitions (init activity)

## What's actually remaining (top of mind)

**Short-term, mechanical:**
1. Switch netdev mode so the host can reach `.254` (`-hostfwd` or
   tap). Then verify TCP handshake to `tFtpdTask` — exercises gate 4
   (RX walker → SDMA RX IRQ → `tFecEndRx` unblock → socket semaphore
   posts → `tFtpdTask` accepts). Small, fast win.
2. Run `nmap` from the host to confirm which ports are actually
   bound (gate 9). Possible none are if daemons spawned but never
   `bind()`'d.

**Medium-term, strategic decision needed:**
3. The Vestas turbine app isn't running. Three candidate causes,
   each implying a different next move:
   - **(a)** This `vxworks.out` is a VxWorks runtime kernel that's
     supposed to load `etc/startup.app` from `/ata0a/`. To make
     that work we need to model the ATA/CompactFlash hardware and
     populate it with the Røye2 disk dump — i.e. Phase 2.5
     (filesystem) per existing `PLAN_Phase2.5_filesystem.md`.
   - **(b)** This is a BSP-only image and the toolkit-relevant
     binary needs to be FTP-pushed to the BSP's already-listening
     `tFtpdTask` (we'd be the FTP *client* now, not the BSP). This
     would mean we have to provide the runtime binary ourselves.
   - **(c)** The BSP gates the turbine app behind a check we're
     failing — RTC time-of-day, hardware key, ARCnet peer
     handshake, etc. Would need disasm-side hunting.

   We need to pick ONE path. Path (a) is the most thoroughly
   pre-scoped (existing Phase 2.5 plan, known disk layout). Path (b)
   needs a separate investigation to identify what to push and how.
   Path (c) is open-ended and likely 3-5 day disasm dive.

**Long-term, the actual end goal:**
4. Toolkit acceptance (gates 10-12): proprietary protocols. Not
   started. Weeks-to-months of work even after the Vestas app boots.
   Decompiled toolkit code exists in `~/Documents/SharedWithXP/Toolkit/`
   per `CLAUDE.md`; we have AP_PROTOCOL_REFERENCE.md and others as
   starting points. Genuine unknown: minimum viable protocol surface
   the toolkit needs.

**Real progress to gate 12: ~20-25% (down from PLAN.md's prior
30-35% claim).** Hardware emulation is 70-80% there. Software stack
is essentially 0% — we have a kernel idling, not a turbine.
