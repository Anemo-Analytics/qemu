# Plan: Stage 1 follow-up — get tNetTask to actually wake

Created 2026-04-30, after commit `daf99bc0` (EE-FORCE).

## Context

After today's EE-FORCE fix, sysClkInt fires reliably for the full 120 s
run (152 buckets vs ~20 before). The next wedge surfaced one layer
deeper:

```
shim @ 0x002acef0  → calls netJobAdd(probe_stub, 0,…)   ✓ runs
netJobAdd          → semGive(netTaskSem at 0x00980a48)   ? unknown
tNetTask           → still PEND throughout, never READY
probe stub @ 0x002acf80  → UNREACHED (boot station never hit)
tFecEndRx          → still PEND on its sem all 120 s
```

Two independent observations point to the same flavor of bug:
- `tNetTask` stays PEND despite (presumably) `semGive(netTaskSem)`
  being called from inside `netJobAdd`.
- `tFecEndRx` stays PEND despite the shim's path-B `qPriBMapPut`-based
  direct wake (per the existing boot-message description).

So the BSP's "task wake" mechanics — semGive resolving a PEND, or
qPriBMapPut force-readying a TCB — aren't completing in our emulation,
even when invoked from kernel context with EE=1 (now that EE-FORCE
keeps sysClkInt alive).

## Goal

Prove which step in the wake chain is failing, then break the chain
at the cheapest point. Acceptance is unchanged: any SYN-ACK from the
guest, or any daemon banner over FTP / WDB / Firedrake.

## Phase A — bisect the wake chain (~45 min)

Three candidate failure points. We need a single run that distinguishes
them.

### A.1 — does the shim's netjob branch actually call netJobAdd?

Add boot stations at:
- `0x0022c288` (netJobAdd entry — already known per boot message)
- netJobAdd return — find by reading the function epilogue

If "netJobAdd entry" is HIT but "netJobAdd return" is UNREACHED,
netJobAdd hangs or crashes partway through.
If both UNREACHED, the shim never reaches netJobAdd (shim bug).
If both HIT, netJobAdd runs to completion — go to A.2.

### A.2 — does netJobAdd's semGive actually run?

The semGive entry symbol address isn't listed in the boot-stations
dump — needs investigation. Two ways to find it:

- Disassemble netJobAdd at `0x0022c288` and find its `bl <semGive>`
  instruction.
- Or: look in the existing `g_semgive_hist` sampler (already in the
  code, dumps PCs at vt=15s) — semGive's body should already show up
  there. Pick the most-hit cluster and station-hit its entry.

Add a boot station at semGive entry. If HIT once per shim fire, A.2
passes — semGive is being called.

### A.3 — does semGive move tNetTask PEND → READY?

We already have `TCB-WATCH` for tNetTask. If A.1 + A.2 confirm that
netJobAdd → semGive runs, but TCB-WATCH still shows `status=0x2`
(PEND) in the next sample, then **semGive is broken inside the BSP**
— the actual transition step (dequeue from waitQ, update TCB.status,
enqueue in readyQ) isn't completing.

This would be the same flavor as the existing `READYQ-FORCE-RX`
failure: kernel data structures don't match what semGive's body
expects.

## Phase B — fix per A outcome

Branch by where Phase A says the chain breaks. Each fix is small if
the diagnosis is right.

### B.1 — if shim never calls netJobAdd

Bug in the 35-instruction shim at `0x002acef0`. Disassemble the
netjob path; check that the args are set correctly and the `bl`
target resolves to `0x0022c288`.

### B.2 — if netJobAdd runs but semGive doesn't fire

Probably `netJobInfo+12` (the sem ID) is wrong or stale. Read the
netJobInfo struct from QEMU side at the right offset and compare to
expected.

### B.3 — if semGive fires but tNetTask doesn't transition (most likely)

Sidestep the BSP's semGive entirely. The shim's path-B already
attempts this for tFecEndRx via `qPriBMapPut` and fails for the same
reason. Higher-leverage fix: force the wake from QEMU side by
directly writing the kernel's readyQ + TCB.status — the way
`READYQ-FORCE-RX` already does for tFecEndRx — but applied to
tNetTask, and verified that the scheduler actually picks it up.

If the scheduler still doesn't dispatch (same as the existing
`READYQ-FORCE` failure mode), the kernel scheduler needs deeper
instrumentation: trace what `readyQ.first` / `readyQBmap` look like
**immediately before** the scheduler picks the next task, vs what we
wrote. That's a separate ~2-hour investigation, treated as "Phase D"
if we get there.

## Phase C — verify

Same probe pattern as before:

```bash
LOG=/tmp/qemu_stage1_e.log PCAP=/tmp/qemu_stage1_e.bin TIMEOUT=120 ./run.sh &
QEMU_PID=$!
sleep 30
nc -w 5 127.0.0.1 2121     # FTP
nc -w 5 127.0.0.1 17185    # WDB
nc -w 5 127.0.0.1 9482     # Firedrake
wait $QEMU_PID
tcpdump -nn -r /tmp/qemu_stage1_e.bin 'src host 169.254.254.254'
```

Pass on any one of: FTP banner, WDB bytes, Firedrake bytes, or
SYN-ACK in pcap.

## Honest about risk

- Phase A is cheap diagnosis. Low risk, high information.
- Phase B.1 / B.2 are surgical edits if diagnosis points there.
- Phase B.3 (likely) drags us back into kernel scheduler territory
  we already know is fragile. The `READYQ-FORCE` mechanism has been
  tried and fails — repeating it for tNetTask without first
  understanding *why* the existing one fails would just produce the
  same outcome.
- If we end up in Phase D (deep scheduler readyQ investigation),
  that's a separate plan, not a quick continuation.

## Time budget

| Phase | Est | Confidence |
|---|---|---|
| A.1 — netJobAdd entry/exit stations | 15 min | high |
| A.2 — semGive entry station | 20 min | medium (need to locate symbol) |
| A.3 — TCB-WATCH already exists | 5 min | high |
| B (one of 1/2/3) | 30–90 min | medium |
| Verify | 15 min | high |
| **Total** | **~2 hours** if B.1/B.2 / **half a day** if B.3 | medium |

## Today's findings to carry over

- `EE-FORCE` (commit `daf99bc0`) keeps sysClkInt alive. Without it,
  none of the doorbell mechanisms can fire past vt~14s.
- Pcap shows guest sends one ARP each for `172.31.197.254` and
  `172.30.1.254` and gives up — peer-node absence is **not** the
  cause of any of the wedges seen so far. May matter at higher
  layers (toolkit upload) but not for getting a daemon to answer.
- The probe stub at `0x002acf80` was UNREACHED across the run. Once
  Phase B unblocks `tNetTask`, this should start hitting.
