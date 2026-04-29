# Session 2026-05-04 — FEC RX delivery works, IRQ-to-tFecEndRx still broken

Plan: `STATUS_2026-05-03.md` + Phase 2 of in-message plan ("wire FEC
RX-done IRQ end-to-end"). Phase 1 wrap-up done (run.sh hostfwd
commit + push + status note). Phase 2 partially advanced — frames
now reach guest DRAM but tFecEndRx still doesn't wake.

## TL;DR

- ✅ **Phase 1 wrap-up.** Committed `run.sh` (hostfwd 9482 + 8080),
  wrote `STATUS_2026-05-03.md`, pushed `mpc5200-stub` to origin
  (11 commits ahead → 0 after push).
- ✅ **Diagnosed RX-drop root cause.** BSP allocates skb buffers
  (BD.data field set to valid DRAM addr) but **never sets
  BCOM_BD_READY**. `tFecEndRx` is PEND'd at PC=0x2fe918 from boot
  before its driver init can arm the BD ring. So every inbound
  packet drops at "BD not READY".
- ✅ **Phase 2.3 force-arm workaround works.** When the RX walker
  finds skb_pa is valid (in DRAM range) but READY isn't set, treat
  the BD as engine-owned anyway. After this: 9 frames delivered,
  zero drops, BD cursor advances cleanly through the ring
  (`0xf0009600 → 0xf0009608 → ...`).
- ❌ **IRQ-to-tFecEndRx wake path still broken.** Even with the
  workaround actively raising both FEC EIR.RXF and SDMA IntPending
  bit 3, `tFecEndRx` stays PEND'd. No FTP banner returns. Two
  separate failures stack up (see below).

## Diagnostic findings

### Failure 1: FEC EIR.RXF doesn't fire FEC IRQ

After the BSP completes its FEC bring-up sequence, it sets
`FEC.EIMR = 0` (writing to MBAR+0x3008). With the mask zeroed,
`(EIR & EIMR) == 0` regardless of which EIR bits we set. So
`mpc5200_fec_raise_eir(s->fec, MPC5200_FEC_EIR_RXF)` raises EIR.RXF
but `qemu_set_irq(s->irq, 0)` (active=0). FEC IRQ line never
asserts.

Sample: `FEC raise_eir bits=0x02000000 EIR=0x0a000000 EIMR=0x00000000 active=0`

This is consistent with FEC-via-BestComm normal operation: with
DMA, FEC EIR isn't the primary RX completion source. SDMA bit 3
IntPending is.

### Failure 2: BSP doesn't service SDMA EXT exception

The BestComm RX walker also sets SDMA IntPending bit 3 and calls
`mpc5200_sdma_eval_irq(s)`. The BSP's IntMask had bit 3 unmasked
briefly during init (`0xeffffff7`) then re-masked it (`0xefffffff`)
before any RX traffic arrived. To unblock, the workaround
**force-unmasks bit 3 inside the RX walker** (clears the bit in
the BSP's IntMask).

After force-unmask: SDMA RAISE fires (`IntPending=0x0000000c
IntMask=0xeffffff7 unmasked=0x00000008 RAISE`). `update_ext` should
drive the EXT line high. But:

- Zero `IC 0x524 read => 0x20000000 (SDMA)` events post force-arm.
- `IntPending=0x0000000c` stays at `0x0000000c` for the rest of the
  run — BSP never W1C-clears bit 3, meaning no ISR ran.
- `tFecEndRx` stays PEND on sem `0x07bee080` at PC=0x2fe918 for
  the entire run.

The BSP's CPU is not taking the EXT exception when ic_sdma_pending
becomes 1. Possible reasons (not investigated this session):

1. **Priority shadowing by ic_fec_pending.** FEC MII PHY polling
   keeps `ic_fec_pending` flickering 0↔1. Our 0x524 priority logic
   reports FEC before SDMA. Each EXT entry returns FEC, BSP
   services MII, returns. SDMA never gets reported.
2. **Edge vs level trigger semantics.** `ppc_set_irq(s->cpu,
   PPC_INTERRUPT_EXT, 1)` may not re-fire if the line was already
   high when sdma transitioned 0→1.
3. **SIU (System Interrupt Unit) modeling.** The MPC5200 has a
   per-source enable/mask register at MBAR+0x500..0x53F that we
   don't model. The BSP may have explicitly disabled the SDMA
   source there at end of init.

Phase 2 outcome (3) "worst (~20%)" from the plan: needs more
investigation than a 2-4h session.

## What changed in code

- `hw/net/mpc5200_fec.c`:
  - Diagnostic in `mpc5200_fec_raise_eir` logging EIR/EIMR/active.
- `hw/ppc/mac_newworld.c`:
  - **RX walker workaround (Phase 2.3):** when BD has skb_pa in
    DRAM but READY isn't set, deliver the frame anyway. Frame is
    written to skb buffer, status set to `(len & 0x7FF) | L`,
    cursor advanced.
  - **Force-unmask SDMA IntMask bit 3** inside RX walker (clears
    the bit when the BSP has masked it).
  - Diagnostic logging on `update_ext` line transitions (fec/sdma/slt
    sources), `0x524` reads with source decode, and SDMA eval state.

No changes to `run.sh` (already done in Phase 1).

## Verification

```bash
LOG=/tmp/qemu_5o.log TIMEOUT=70 ./run.sh &
sleep 22
exec 3<>/dev/tcp/localhost/2121; timeout 10 cat <&3 | xxd | head
# expected: empty (BSP didn't respond)
grep "force-arming" /tmp/qemu_5o.log     # ≥9 (frames delivered)
grep "SDMA eval.*RAISE" /tmp/qemu_5o.log # ≥1 (IRQ raised)
grep "0x524.*SDMA" /tmp/qemu_5o.log      # 0 (BSP didn't service)
```

## Honest progress

- Last session: 25% → 40-45% to gate 12. The 40-45% banked on Phase
  2 closing this session.
- Reality this session: still ~40-45%. Frames now reach DRAM (real
  improvement on the BSP side — buffers are populated for whenever
  tFecEndRx eventually wakes) but the wake itself is not happening.
- Gate 9 status unchanged: TCP handshake works, byte-level comms
  doesn't.

## Next session plan

Investigate why BSP doesn't take EXT exception for ic_sdma_pending.
Tier of attempts:

1. **Reverse SDMA-vs-FEC priority in 0x524.** If MII is shadowing,
   reporting SDMA first should let it through. Cheap test, ~10 min.
2. **Force EXT line low briefly when raising SDMA.** Triggers a
   fresh 0→1 edge in case PowerPC EXT is edge-latched somewhere.
3. **Investigate SIU at MBAR+0x500..0x53F.** Check if BSP wrote a
   mask there. If so, model the register so our IRQ raise doesn't
   get blocked.
4. **Last resort: poke the sem directly.** VxWorks sem at
   0x07bee080 has a known layout. semGive() just decrements/posts
   a count. We could write the right bytes from the QEMU side via
   `cpu_physical_memory_write` and trigger the scheduler. Hacky
   but unblocks gate 9.

Out of scope: PSC1 TX IRQ (would let us drop printf no-op),
zlfs/decompression for `loadModule`.
