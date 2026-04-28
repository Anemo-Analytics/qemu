# BSP PHY init expectations — m5200Fec driver

Investigation of `/tmp/vxworks_romfs/vxworks.out` to determine why the
Vestas BSP's m5200Fec driver was stuck in a tight MMFR poll loop.
Pre-fix: 3141 FEC ops in 30s, never proceeded. Post-fix: 335 FEC ops
in 30s, BSP enables FEC and is now in higher-level retry cycle.

## TL;DR

The BSP does **not** check vendor PHY IDs. It is a generic IEEE 802.3
clause-22 driver. The bug was that **our QEMU's PHY model dropped
writes to the BMCR ISOLATE bit**, and the BMCR low 6 bits were all
zero (failing a "PHY present" probe).

Both fixed in commit `ed449f1b6e`.

## Findings

### Expected PHY ID — none

No reads of PHYID1/PHYID2 (regs 2/3) in the init path. No string
references to specific PHY families (Realtek/Micrel/National/etc).
The driver is generic clause-22 only.

### Expected register values

| Register | Bit(s) | Where checked | Required |
|---|---|---|---|
| BMCR (0) | bits 0–5 | probe — `andi. r9, r9, 0x3f` at ~`0x12f084` | **at least one bit non-zero** (else "no PHY", returns 0xFF) |
| BMCR (0) | bit 10 (ISOLATE 0x400) | `m5200FecMiiIsolate @ 0x12ef84` | Must read back as **set** after BSP writes 0x400 — polled up to 427 ticks |
| BMSR (1) | bit 2 (LINK_STATUS) | `m5200FecMiiBasicCheck @ 0x12ec90` | Must be **set** |
| BMSR (1) | bit 4 (Remote Fault) | `m5200FecMiiBasicCheck @ 0x12ec90` | Must be **clear** |
| BMSR (1) | bit 5 (AN_COMPLETE) | `m5200FecMiiAnStart @ ~0x12fa68` | Must be **set** within 427 ticks |
| BMSR (1) | bits 13–15 | `0x12ee84` | Capability bits per AnOrderTbl |
| AN_ADV (4) | full reg | `m5200FecMiiAnRun @ 0x12f55c` | Read for logging + RMW |
| AN_EXP (6) | bits | `m5200FecMiiAnRun @ 0x12f598` | Read for logging |

`m5200FecPhyAnOrderTbl` at VMA `0x003F3A60`: 5×16-bit entries
(`0x0000, 0x0100, 0x2000, 0x2100, 0x2100`) used as fallback BMCR
force-values.

### The original stuck loop

`m5200FecMiiIsolate` at `0x12ef84`. Sequence:

```
12ef8c: li   r9, 0x400              ; ISOLATE bit (BMCR bit 10)
12efac: bl   0x12ebbc               ; m5200FecMiiWrite(end, phy, BMCR=0, 0x400)
;; --- POLL LOOP ---
12efd0: bl   taskDelay              ; one tick
12efe4: bl   0x12ec3c               ; m5200FecMiiRead -> MMFR=0x60020000
12eff0: lhz  r9, 8(r1)              ; BMCR readback
12eff4: andi. r9, r9, 0x400         ; test ISOLATE bit
12eff8: bf 2, 0x12f044               ; if ISOLATE set -> exit success
12effc: addi r31, r31, 1
12f004: cmplwi r31, 427
12f008: ble  0x12efcc                ; <=426 -> loop again
```

**Exit condition:** BMCR readback must have bit 10 (ISOLATE = 0x400)
set. Pre-fix: our PHY returned 0x3000 or 0x0000 — bit 10 never set.
Post-fix: writes to BMCR are stored verbatim and ISOLATE survives the
write→read cycle.

### Why the polling loop never timed out

`taskDelay(1)` gates each iteration. With our SLT timer firing at
60 Hz, 427 ticks ≈ 7 seconds. So the timeout *would* eventually fire.
The agent flagged a concern that ticks may not advance — verified
they do (gate 1 work). So the original wedge was the data bug
(ISOLATE drop), not a missing tick.

### Outer retry — `m5200FecRestart`

If `m5200FecPhyInit` returns -1 after iterating all `AnOrderTbl`
entries, `m5200FecStart` returns ERROR. The upper end-driver layer in
endLib retries via `m5200FecRestart`, which has its own retry counter
and reboots after too many failures. This is the cycle we see now
post-fix: BSP enables FEC, something fails (still TBD), restart.

### Concrete fix recommendation (applied)

1. **PHY register writes must be retained.** BMCR write of 0x400 must
   read back as 0x400. Implemented as a custom inline PHY model in
   `hw/net/mpc5200_fec.c` rather than reusing `lan9118_phy.c` whose
   write mask drops ISOLATE. ✓
2. **BMCR default = 0x3101** (AN_EN | RESTART_AN | DUPLEX_FULL + low
   bit 0 to satisfy `andi. r9,r9,0x3f` probe). ✓
3. **BMSR = 0x782D**: bits 14:11 capability + bit 5 AN_COMPLETE +
   bit 3 AUTONEG + bit 2 LINK_ST + bit 0 EXTCAP. ✓
4. **MII completion (EIR bit 23) on every MMFR write** to wake any
   `semTake` waiters. ✓ (already in mpc5200_fec_mmfr_write).

### What's still failing (gate 3 not yet cleared)

After fix:
- BSP escapes the PHY probe loop ✓
- BSP enables FEC: `ECR ← 0x02` ✓
- **Something fails** post-enable — BSP writes `ECR ← 0x00` (disable)
  and restarts the cycle (`ECR: 0 → 1 → 2 → 0 → 0 → 0 → 1` repeating).
- No TCR writes yet (BestComm tasks not enabled).
- BSP does NOT write any PHY register in the current path — so
  whatever fails is *not* the m5200FecMiiIsolate loop.

Next investigation should trace the post-`ECR.ETHER_EN` code path in
`m5200FecStart` to identify the next failure point.

## Sources

- Agent disassembly session — full `llvm-objdump` PowerPC dump at
  `/tmp/vx.dis` (local only)
- `/tmp/vxworks_romfs/vxworks.out` — VxWorks ELF (stripped, partial
  symbol table)
- `BSP_park_findings.md`, `BSP_fec_bestcomm_findings.md` — earlier
  context
