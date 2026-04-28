# Existing FEC + BestComm code audit (gate 3)

Audit produced by parallel agent on 2026-04-28 covering:
- `/home/kasper/qemu/hw/net/mpc5200_fec.c` (504 lines, current stub)
- `/home/kasper/qemu/hw/ppc/mac_newworld.c` (1474 lines)
- `hw/net/Kconfig`, `hw/net/meson.build`, `hw/ppc/Kconfig`

## Verdict

**Existing stub is ~80% there.** The FEC device is solid (PHY/MII works, registers covered, IRQ wiring done, build wiring done). The biggest gap is the BestComm TX/RX BD walker that ferries bytes between system memory and the FEC FIFO.

## Gap summary

| Item | Status |
|---|---|
| FEC register coverage (CSRs) | OK |
| PHY/MII handling | OK |
| BestComm SRAM backing store (16 KiB) | OK |
| BestComm register file (0x100 bytes) | OK with TCR-write detection logged |
| TaskBAR reset value `0xFC003000` | OK |
| FEC IRQ wiring → IC → EXT | OK |
| MBAR+0x524 PerStat dispatch | OK |
| TDAR/RDAR write side-effect | **GAP** — currently silently stored |
| ECR.ETHER_EN deassert side-effect | **GAP** — should zero RDAR/TDAR |
| `.receive` callback | **GAP** — currently drops frames |
| TCR enable trigger → walk BDs | **GAP** — biggest |
| FEC ↔ MPC5200State cross-reference | **GAP** — no `s->fec` pointer |

## Proposed 8-commit sequence

1. **bestcomm: add BD/TCR header with flag macros**
   New `include/hw/net/mpc5200_bestcomm.h` — `BCOM_BD_READY`, RX/TX BD layout, TCR enable mask. Pure definitions.

2. **mpc5200-fec: expose raise_eir helper**
   `void mpc5200_fec_raise_eir(DeviceState *dev, uint32_t bits)` ORs into EIR + calls update_irq.

3. **mac_newworld: store fec DeviceState pointer**
   `s->fec = fec` on MPC5200State after qdev_new.

4. **bestcomm: TX BD-walker on TCR[2] enable**
   When `0x1220` is written with `(value & 0xC0)`, read BSS[0x008CFC00] var-table, walk BDs, `dma_memory_read` → `qemu_send_packet` → clear READY → fire EIR.TXF.
   Verify: tcpdump shows guest-originated TCP SYN to 169.254.254.252:21.

5. **mpc5200-fec: route .receive into BestComm RX queue**
   Per-device receive queue + callback hook for executor.

6. **bestcomm: RX BD-walker on TCR[3] enable**
   On TCR[3] enable, drain queue into BDs at BSS[0x008CFC04]'s bd_base, set length + L flag, raise EIR_RXF.

7. **bestcomm: re-walk on TDAR/RDAR write**
   Some BSP code-paths kick TDAR/RDAR instead of re-writing TCR.

8. **trim diagnostic logs**
   Once stable, gate logs behind tracepoints.

## FEC register cheat-sheet (essential bits)

From parallel agent on `docs/MPC5200_FEC_Chapter14.md`:

- ECR @ 0x024: bit 0 = RESET (self-clearing), bit 1 = ETHER_EN. Mask `0x3`.
- EIR @ 0x004: W1C. Bit 27 = TFINT (TX done). Bit 23 = MII. Bit 25 = RXF.
- MMFR @ 0x040: standard MII frame; setting EIR.MII on completion is mandatory.
- MSCR @ 0x044: when 0, MMFR writes are queued (no MII frame issued).
- TFIFO_DATA @ 0x1A4 / RFIFO_DATA @ 0x184: 32-bit BE, BestComm-driven.
- XMIT_FSM @ 0x1C8: must be 0x03 for normal CRC append.
- All access 32-bit, big-endian.

## BestComm runtime data confirmed

- TX cfg pointer (BSS@`0x008CFC00`): `0x009a3bbc`
- RX cfg pointer (BSS@`0x008CFC04`): `0x009a4c68`
- TX var-table dump (after diag patch):
  `00000002 48139fb2 48139fb6 48139fba 48139fbe 48139fca 00000010 00000004`
  - field[0] = task slot = 2 ✓ (TX is slot 2 per BSP findings)
  - field[6] = initiator = 0x10 = 16 ✓ (FEC TX initiator per findings)
  - field[1..5] = 5 sequential addresses (0x48139fxx) — purpose TBD
- RX var-table dump:
  `00000003 48139fd2 48139fd6 48139fda 48139fde 48139fea 00000006 00000004`
  - field[0] = task slot = 3 ✓
  - field[6] = initiator = 0x06 ✓

**Open question:** the 0x48139fxx pointers don't match the Linux MOTbcommlib var-table layout (which has FIFO/enable/bd_base in those slots). Vestas BSP may use a different layout. To resolve, capture more data once TCR is actually enabled (BSP may populate fields lazily).

## Sources

- Parallel agents (4 of them) ran 2026-04-28
- `/home/kasper/qemu/hw/net/mpc5200_fec.c`
- `/home/kasper/qemu/hw/ppc/mac_newworld.c`
- `/home/kasper/qemu/docs/MPC5200_FEC_Chapter14.md`
- `/home/kasper/qemu/BSP_fec_bestcomm_findings.md`
- `/home/kasper/qemu/BSP_motbcommlib_layout.md`
