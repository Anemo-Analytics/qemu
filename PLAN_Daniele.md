# Daniele's plan: BSP park diagnosis

**Owner:** Daniele
**Branch:** `mpc5200-diagnosis` off `mpc5200-stub`
**Time budget:** ~1 day
**Touches:** docs only — no code changes
**Output:** one markdown report at `BSP_park_findings.md` (repo root)

---

## Why this exists

Kasper's track (FEC implementation) is built on a hypothesis that the
kernel is parked at `0x207fe8` waiting for FTP from 169.254.254.253.
Strong hypothesis — link-local APIPA pattern, FEC stub responds
plausibly but moves no packets — but unverified.

If the hypothesis is wrong, Kasper sinks 3–5 days into a FEC model that
doesn't unblock anything. This track derisks his work by answering the
question directly from the binary.

---

## The question

**At NIP `0x207fe8`, what is the BSP actually waiting for?**

Sub-questions:
1. What memory location or peripheral register is being polled?
2. Which task (in WIND_TCB sense) is parked, and on what blocker?
3. If the answer is "FTP push", what protocol exactly — RFC 959, or
   something Vestas-proprietary?

---

## Inputs

- **Binary:** `/tmp/vxworks_romfs/vxworks.out` — VxWorks 5.5.1 PowerPC
  ELF, ~14 MB. Symbol table preserved.
- **Current QEMU state:** `mpc5200-stub` HEAD at commit `9ec6a690fd`.
  Run with `-d in_asm,int` after the park to see the polling loop in
  action.
- **Vault:** Vestas internal docs. Search terms to start:
  - "boot mode" / "boot loader" / "169.254"
  - "Falcon" (binary upload tool referenced in vault)
  - "Snoopy WCF" (referenced as upload-related)
  - "vxWorks bootrom" / "bootApp"
  - "CT6003" + "FTP"
- **Real controller PCAP:** ask if one exists in the vault from a real
  CT6003 boot sequence. Would short-circuit most of this analysis.

---

## Method

### Step 1 — disassemble around the park

Use Ghidra or radare2 on `vxworks.out`. Locate `0x207fe8` and the basic
block it lives in. Walk back ~30 instructions to find:

- The load that the loop tests (e.g. `lwz r3, 0x0(r28); cmpwi r3, 0; beq -8`)
- What `r28` (or whichever register holds the polled address) was set
  to. Trace back through the function prologue.

If the polled address is in MMIO range (`0xf0000000..0xf0010000`):
identify which peripheral block by offset:
- `0xf0003000..0xf0003FFF` = FEC
- `0xf0001200..0xf00012FF` = BestComm/SDMA
- `0xf0000600..0xf00006FF` = GPT timers
- `0xf0003D40..0xf0003D57` = I2C2 (X1226 RTC + EEPROM)
- `0xf0002000..0xf0002BFF` = PSC1–6 UARTs

If the polled address is in RAM: it's a kernel data structure. Check
symbol table for what lives there.

### Step 2 — identify the blocked task

In VxWorks 5.5.1, the running task TCB pointer is at a known offset
from the kernel symbol `taskIdCurrent`. Approach:

1. `nm vxworks.out | grep -iE "taskidcurrent|taskhash|kernelis"` →
   locate `taskIdCurrent`
2. At runtime (qemu monitor `xp /1xw <addr>`), read the pointer to find
   the parked TCB
3. The TCB struct in WIND 5.5.1 begins with the task name pointer at
   offset 0x18 (verify with `nm | grep tNetTask` or similar)
4. Read the name → identifies who's parked
5. The TCB also has a status field; status `0x12` = pended,
   `0x14` = pended+timeout. Decoding requires WIND_TCB layout —
   `h/private/taskLibP.h` from a WIND 5.5.1 source dump if available

Quick heuristic without full WIND_TCB decode: if `taskIdCurrent`
doesn't change tick-to-tick at all, the parked task is whoever is
currently running. If it cycles, it's the idle task and we need to
look at the ready queue.

### Step 3 — protocol identification

If Step 1+2 point at network wait:

1. Vault search "Falcon" + "upload" + "boot" — Falcon is referenced as
   the technician-side upload tool
2. Check `bin/release_diab_ppc/` in the dump for any client-side hints
   (`.cdf`, `.cfg`, `.bsp` files that name protocols/ports)
3. If a real PCAP exists: definitive answer (port, handshake, payload)
4. If not: best-effort guess (RFC 959 FTP is the Occam's razor answer
   given the APIPA pattern)

---

## Deliverable

Write `BSP_park_findings.md` (repo root) with:

```markdown
# BSP park diagnosis findings

## Verdict

**Hypothesis:** vxworks.out parks at 0x207fe8 waiting for FTP push from
169.254.254.253.

**Result:** CONFIRMED / REFUTED / PARTIAL

**Real wait condition:** <one sentence>

## Evidence

### Polled address
<MMIO offset or RAM symbol — what register/word is the loop reading>

### Disassembly snippet
```
0x207fd8: <insn>
...
0x207fe8: <the park insn>
```

### Parked task
Task name: <e.g. tBootInit>
Blocked on: <semaphore / message queue / direct poll loop>
TCB status: 0x<hex>

### Protocol (if network wait)
- Standard FTP / TFTP / proprietary
- Port:
- Expected directory layout:
- Handshake notes:

## Implications for Kasper's track

<one paragraph: does the FEC plan still hold? what changes?>

## Open questions

<anything you couldn't answer that needs Kasper or vault access>
```

Commit on `mpc5200-diagnosis`, push, ping Kasper.

---

## What "done" looks like

- Verdict line is unambiguous (CONFIRMED or REFUTED, not "probably")
- Evidence section cites specific addresses and bytes
- "Implications" tells Kasper exactly whether to charge ahead or pivot
- If you ran out of time, the report still says what was checked and
  what wasn't — partial findings are useful, silent gaps are not

---

## Out of scope

- Writing any QEMU device code
- Modifying `hw/ppc/mac_newworld.c`
- Building a working FEC model
- Booting all the way to userspace
- Any application-layer protocol work (Firecrest, AP, etc.)

If during analysis you find a quick obvious bug in the existing stubs
(e.g. an MMIO offset that's clearly wrong), note it in the report —
don't fix it. Kasper owns code changes during this phase.
