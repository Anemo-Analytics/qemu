# Session 2026-05-12 — sysClkInt-stop root cause: BSP NULL fn-ptr at vt~13s

Plan continuation from `SESSION_LOG_2026-05-11.md` ("keep sysClkInt
firing past vt~11.5s"). Plan hypothesis was missing intUnlock in
`m5200PscSioDevInit` holding MSR.EE=0.

## TL;DR

- ❌ **Plan hypothesis FALSIFIED.** MSR.EE toggles continuously
  throughout the run (~5000/s) even after sysClkInt stops. Not a
  missing-intUnlock issue. Not a QEMU CPU model bug.
- ✅ **Real root cause identified.** At vt~13s the BSP iterates a
  driver/hook chain and calls a NULL function pointer:
  - Call site: `0x001791dc bctrl` (after `mtctr r10` from struct[+24]).
  - Guard at `0x001791d0`: `bf 30, 0x1791e0` only checks `struct[+22]`,
    not `struct[+24]`. struct[+22] is the "installed" flag, struct[+24]
    is the handler ptr. They got out of sync (or +24 is cleared while
    +22 stays set).
  - Result: `bctrl` to NIP=0 → instruction at addr 0 is `0x00000000` =
    illegal opcode → POWERPC_EXCP_PROGRAM (errcode 0x21 = INVAL_INVAL).
  - QEMU classifies as HV_EMU(96), excp_helper.c line 421 remaps to
    PROGRAM. Per disasm, kernel handler at vector 0x700 doesn't
    recover — sysClkInt stops, no further mtdec.
- 🟡 **Outcome = "acceptable" per plan.** B3 / out-of-scope. Sharp
  pinpoint for the next session: identify the struct (caller chain
  to function at runtime ~0x00179000-0x00179210) and either populate
  +24 or patch the guard to test +24 too.

## Plan classification

Per the plan's A5 table, observation matches **B3: secondary
condition / out-of-scope** more than B1 or B2:

- **NOT B1** (missing intUnlock): MSR.EE returns to 1 after every
  EE=0 transition. ~5000 toggle pairs per second through the entire
  run. Filter analysis on dominant NIPs:
  - 0x00207f6c (1->0) / 0x00207fe8 (0->1): 27192/27115 — kernel
    scheduler critical section enter/exit, balanced
  - 0x00138a74 (1->0) / 0x00138a8c (0->1): 5524/5407 — intLock /
    intUnlock primitive, balanced
- **NOT B2** (QEMU CPU model bug): DEC timer fires correctly,
  pi.DECR is set, DECR exception is delivered (974 deliveries in
  20s before stop). The CPU model behaves correctly per spec.
- **B3 / out-of-scope**: actual root cause is BSP-side state
  corruption that crashes the kernel at vt~13s.

## What we actually saw

Diagnostics added to QEMU (left in tree for further work):

| File | Hook | Insight |
|---|---|---|
| `target/ppc/helper_regs.c:340` | MSR.EE-transition log | EE toggles ~5000/s through full run; "stuck at 0" hypothesis false |
| `hw/ppc/ppc.c:cpu_ppc_store_decr` | mtdec write log | sysClkInt does mtdec at NIP=0x00207a28 LR=0x001180b0 every ~13ms; stops at vt~13s |
| `hw/ppc/ppc.c:cpu_ppc_decr_cb` | DEC timer cb log | timer fires 977 times then stops |
| `target/ppc/excp_helper.c:1566` | exception delivery log | 977 DECR + 23 EXTERNAL + 2 HV_EMU exceptions; HV_EMU at vt=13.036s is the killer |
| `target/ppc/excp_helper.c:2381` | DECR delivery log | confirms each DECR cleared on delivery |
| `hw/ppc/mac_newworld.c:2236` | shim-fire counter on FSHOOK_REG_NETJOB_FUNC read | confirms shim runs ~76 Hz until vt~12.8s |

### Crash signature

```
EXCP #1000: HV_EMU (96) nip=0x00000000 msr=0x0000b032 lr=0x001791e0 errcode=0x21
```

Decode:
- HV_EMU(96) is remapped to POWERPC_EXCP_PROGRAM (excp_helper.c:421,
  applies to all server arch — 6xx/G2 inherits this remap).
- errcode 0x21 = POWERPC_EXCP_INVAL (0x20) | INVAL_INVAL (0x01) =
  illegal instruction.
- NIP=0 — CPU jumped to NULL.
- LR=0x001791e0 — return address from `bctrl` at 0x001791dc.

### Call site disasm (runtime 0x179180-0x1791e4)

```
1791b0: lwz  r10, 24(r31)   ; r10 = struct[+24] = handler ptr (NULL)
1791b4: sth  r9,  20(r31)
1791b8: lwz  r9,  0(r29)
1791bc: lwz  r3,  28(r31)   ; r3 = struct[+28] (call arg)
1791c0: addi r9,  r9, 1
1791c4: stw  r9,  0(r29)
1791c8: lhz  r9,  22(r31)   ; r9 = struct[+22] = "installed" flag (16-bit)
1791cc: cmpwi 7,  r9, 0
1791d0: bf   30, 0x1791e0   ; if r9 == 0, skip the call ← only checks +22
1791d4: mtctr r10
1791d8: crclr 6
1791dc: bctrl                ; CALL r10 (NULL!) ← crashes here
1791e0: lwz  r9,  0(r29)
```

Bug: guard checks struct[+22] (16-bit halfword) alone but call uses
struct[+24] (function pointer). If struct gets +22=non-zero with
+24=NULL (initialization race or de-init order bug), kernel calls
NULL.

There's also a SECOND indirect-call site near the function entry at
0x001791a8 (`mtctr r11; bctrl` with r11 from a different BSS
location) — that one didn't crash because r11 was valid at the time
of the run.

## What didn't work

### Workaround attempt: install `blr` at PHY 0

Wrote `0x4e800020` (blr) at physical address 0 hoping a NULL bctrl
would just blr-return harmlessly. Test result: WORSE.
- HV_EMU exceptions disappeared (good, NULL call returns).
- But virtual-time progress dropped from vt=13s to vt=9s.
- DECR-CB #676 logged with `nip=0x0 msr=0x0` — CPU eventually
  entered some unrecoverable state (possibly a checkstop, possibly
  cs->halted).
- Side effect: existing probe stub at 0x002acfb0 has a pre-existing
  infinite-loop bug (no mflr/mtlr around `bl semGive`), only
  surfaced when control reaches it more frequently.

Reverted. The blr-at-zero approach masks the kernel bug but
introduces new failure modes.

### Plan A1 sig-match for `m5200PscSioDevInit` failed

Tried full body (444 B), prologue (0x40, 0x60, 0x80), mid-body
(0x80-byte chunks at staggered offsets), tail (0x60 from +0x150),
caller `sysSerialHwInit2` (0x60+0xa0). NO matches in runtime kernel
`.text` (file 0x80..0x808390). Sanity-checked sig-match helper by
locating `intLock` (0x00138a3c, 0x00138a68 — matches session
2026-05-11 finding).

Either the runtime PSC driver is structurally different from the
bootrom version (likely, runtime is 2022 turbine dump vs older
bootrom), or it was inlined into the caller. Either way, even
finding it wouldn't have helped — the actual failure mode isn't
"EE=0 stuck", it's "kernel crashes on NULL fn ptr".

## Verification of the diagnosis

Phase C run (40s with host probes):
- 977 DECR + 23 EXTERNAL + 2 HV_EMU.
- HV_EMU at vt=13.036s with NIP=0, LR=0x001791e0 — same crash
  signature.
- Pcap: 1 frame from `00:1b:f0:00:00:0a` (boot ARP), nothing else.
  Host ARP requests for 169.254.254.15 unanswered.
- SHIM-FIRE last bucket=16 (960 reads) at vt=12.81s.

Pass criteria from plan §C all FAIL — confirming the kernel-crash
diagnosis (sysClkInt stops because kernel crashed, not because of
EE issues).

## Why m5200PscSioDevInit was a wrong lead

The plan inferred PSC init was suspicious because it has `bl
intLock` early and the symptom timing matched UART-init register
bursts. But:

1. The bootrom version has clean lock pairing (single bl intLock at
   +0x98, single tail-call to intUnlock via `b 0x100106c` at
   +0x18c). No bypass return paths.
2. The actual failure is at NIP=0x00179000-area, not in any PSC
   driver function range we know of.
3. PSC1/PSC4/PSC5 register-write bursts are likely a SYMPTOM
   (driver re-init after kernel crash) rather than CAUSE.

## Diagnostic infrastructure added (not yet gated)

All four logs print continuously at boot. Worth keeping as-is for
the next session, then gate behind `#ifdef DEBUG_MAC_NEWWORLD` or
similar before committing:

- `target/ppc/helper_regs.c`: MSR.EE-transition log with "noisy"
  filter (skips 0x00207f6c/0x00207fe8 scheduler pair and
  0x00138a74/0x00138a8c intLock/Unlock pair).
- `hw/ppc/ppc.c:cpu_ppc_store_decr`: mtdec write log (capped at
  4096).
- `hw/ppc/ppc.c:cpu_ppc_decr_cb`: DEC timer cb log (capped at
  4096).
- `target/ppc/excp_helper.c:1566`: exception delivery log (capped at
  4096).
- `target/ppc/excp_helper.c:2381`: DECR delivery log (capped at
  4096).
- `hw/ppc/mac_newworld.c:2236`: SHIM-FIRE counter on
  FSHOOK_REG_NETJOB_FUNC read (one print per 60-read bucket).

## Next-session next-steps

In dependency order, biggest leverage first:

1. **Identify the BSP function containing 0x00179000-0x00179210.**
   Disassemble the surrounding bytes and walk callers. The function
   has two indirect-call sites (0x1791a8 and 0x1791dc), suggesting
   a linker-style "iterate hook list" or "iterate driver list".
   Likely candidates: `excHookHandle`, `kernelHookCall`,
   `taskCreateHookShow`, `taskSwitchHookCall`, etc. — the VxWorks
   hook-table walker pattern. Check kernel BSS variables loaded at
   0x179168-0x17917c (`-31768(r25)`, `-31844(r26)`, `-31648(r23)`
   etc.).

2. **Find the struct address at the crash.** Track r31's value at
   the bctrl. r31 is non-volatile — usually loaded near function
   entry. Live-debug via QEMU monitor or add a register-dump on
   EXCP delivery for HV_EMU/PROGRAM specifically.

3. **Patch the guard.** Once the struct is identified, the simplest
   fix is to patch instruction at 0x001791d4-0x001791dc to test
   r10 == 0 too. Possibilities:
   - Replace `mtctr r10` (0x7d4903a6) at 0x1791d4 with a check + skip
     to 0x1791e0. Needs 2-3 instructions in 4 bytes — won't fit
     directly. Need to relocate via shim.
   - Or simpler: replace `0x4e800421` (bctrl) at 0x1791dc with
     `0x60000000` (nop). Call is never made even when valid. Tradeoff
     depends on how often the handler is expected to fire (and what
     for).
   - Best: install a 4-insn stub elsewhere (free space at
     0x002acfb4+); patch 0x1791d4 to `b <stub>`; stub does
     `cmpwi r10, 0; beq 0x1791e0; mtctr r10; bctrl`. Branch range to
     0x002acfb4 from 0x1791d4: 0x002acfb4 - 0x001791d4 = 0x133be0,
     within 26-bit `b` reach (max ~0x1ffffff). OK.

4. **Independent of (1)-(3): fix the probe stub LR-loop bug.** The
   stub at 0x002acf80 has no mflr/mtlr around `bl semGive`. After
   semGive returns, the blr at 0x002acfb0 returns to itself.
   Currently masked by sysClkInt's DECR preemption taking the CPU
   out, but it's a latent bug that surfaces under different timing.
   Save LR via `mflr r0; stwu r1, -16(r1); stw r0, 4(r1)` at entry,
   restore via `lwz r0, 4(r1); mtlr r0; addi r1, r1, 16` before the
   final blr.

## Time

- Plan A1+A2 sig-match attempts: ~30 min (sig-match failed; bootrom
  disasm + lock-pair census instead).
- Plan A3 MSR.EE log: ~10 min.
- Plan A4 build + 4 captures with progressively richer
  instrumentation: ~50 min.
- Plan A5 classification: ~10 min.
- Failed B1-style workaround (blr-at-zero): ~20 min, reverted.
- Plan C verification + writeup: ~30 min.

Total ~2.5 h.

## Files modified (not committed)

- `target/ppc/helper_regs.c`: MSR.EE log
- `hw/ppc/ppc.c`: mtdec + DECR-CB logs
- `target/ppc/excp_helper.c`: EXCP delivery log + DECR-DELIV log
- `hw/ppc/mac_newworld.c`: SHIM-FIRE counter
- `SESSION_LOG_2026-05-12.md`: this file

`/tmp/sigmatch_psc.py`: helper script used for A1.
