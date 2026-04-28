# Strategy review & documentation rhythm

**Date:** 2026-04-28
**Status:** approved (Kasper, this conversation)
**Supersedes:** ad-hoc plan-update pattern from earlier in this branch

## Context

End goal restated: **the VMP6000 Toolkit (Windows VM) connects to our
QEMU turbine, recognises it as a CT6003, and successfully performs a
software-load operation against it.**

We're at gate 2 of a 12-gate roadmap (PLAN.md). Long horizon —
gates 10–12 likely span months because they involve
reverse-engineering Vestas-proprietary protocols. We need a
documentation rhythm that survives that timeline without becoming
stale or chaotic, plus a concrete near-term work plan.

This spec captures three decisions made in the strategy review:

1. Strategic direction (which boot path)
2. Documentation rhythm (how PLAN.md and findings docs evolve)
3. Near-term work plan (BestComm executor)

## Decision 1 — Strategic direction: A1 (FTP recovery boot)

The brainstorming surfaced three viable paths to "real Vestas
application running in QEMU":

- **A1** — FTP recovery boot. BSP fetches runtime via FEC + FTP,
  application launches. Models the technician's recovery path on a
  broken turbine.
- **A2** — Pre-loaded runtime. Decompress `vxworks.romfs.zlfs`,
  load directly via `-device loader,file=...`. Skip boot kernel
  entirely. Pragmatic but bypasses real boot mechanism.
- **A3** — Cold-start via `tffs=0,0(0,0)`. Model NAND flash, kernel
  boots from flash. Most production-realistic.

**Choice: A1.** Continue the current direction.

**Reasoning:** A1 gives the richest test surface (we'll later be
able to test recovery flows, not just steady-state). The
near-term work — BestComm executor — is required for application
networking anyway (gate 9, AP listener), so building it now isn't
wasted. The cost is ~2 extra weeks vs A2, but with debugging
benefit later.

A2 and A3 remain documented shortcut options in PLAN.md if A1's
BestComm work turns out harder than estimated.

## Decision 2 — Documentation rhythm: living PLAN.md + per-gate discipline

The chosen rhythm combines two patterns:

### Living PLAN.md (the "d" pattern)

- PLAN.md is the **single source of strategic truth**. When a
  finding changes the plan, PLAN.md is rewritten in place. Old
  versions live in git history — no "stale section" markers, no
  contradictions between sections.
- If it's in PLAN.md, it's current. If a section is no longer true,
  it gets removed or rewritten, never marked "deprecated".

### Per-gate discipline (the "b" pattern)

- Every gate clear or gate block triggers a commit cycle:
  1. Update the gate status table in PLAN.md
  2. Write or update a findings doc with the evidence
  3. Commit with the gate number in the message (e.g.
     "gate 3: bestcomm tx executor — guest sends arp")
- Findings docs (`BSP_park_findings.md`,
  `BSP_fec_bestcomm_findings.md`, future siblings) are
  **snapshots / citations**: raw intel frozen at a moment in time.
  They are NOT continuously maintained.
- PLAN.md *references* findings docs ("see `BSP_park_findings.md`
  for evidence") but absorbs the conclusions, so PLAN.md remains
  readable standalone without diving into findings.

### Strategic decisions log

- Append-only section at the bottom of PLAN.md, header
  `## Strategic decisions log`.
- One-line entries: date, decision, one-sentence reason.
- Captures pivots like today's "A1 over A2/A3" so future-you
  doesn't have to dig through git history to understand why we're
  on a path.

### Per-person plans retire when work completes

- `PLAN_Daniele.md` — Daniele's diagnosis is complete. Findings
  landed in `BSP_park_findings.md`. The plan doc itself can be
  archived or left in place; either way it's no longer load-bearing.
- `PLAN_Kasper.md` — current working spec for the FEC + BestComm
  code work. Folds into PLAN.md once gate 6 (first serial banner)
  is cleared. Until then it's a useful per-step checklist.
- `PLAN_Phase2.5_filesystem.md` — placeholder, gated on Daniele's
  findings (which now show FS path is needed for gates 7–8).
  Promote to a real plan when we reach gate 7.

### What this looks like in practice

Coming back to the repo cold after a week away:

- Open `PLAN.md` → see current gate position, status of each gate,
  decisions log explaining how we got here
- Click into the most recent findings doc to see raw evidence for
  the current state
- Open whichever per-person plan is in flight to see the active
  work checklist

That's three docs, in priority order, no contradiction between them.

## Decision 3 — Near-term work plan: BestComm executor

### Goal

Land gate 3 (BestComm TX executor — guest sends a SYN to the FTP
server) and gate 4 (BestComm RX executor — FTP server sees the
connection from `.254`, not just QEMU's startup probe).

### Approach: snoop the BSS task config pointer (option B from earlier)

Daniele's investigation gave us concrete BSS addresses:

- `0x008CFC00` — pointer to **TASK_FEC_TX** config struct
- `0x008CFC04` — pointer to **TASK_FEC_RX** config struct
- TCR[2] at `MBAR+0x1220`, enable pattern `sth 0xC2`
- TCR[3] at `MBAR+0x1222`, enable pattern `sth 0xC3`

When BSP writes a non-NULL pointer to those BSS slots, we know the
per-task config struct address. Walk its fields (Freescale
MOTbcommlib `tasksetup_fec_*_bd.c` layout) to find the BD ring base.

### Implementation steps

1. **Add TCR + BSS write logging** to `mac_newworld.c`'s
   `mpc5200_mmio_write` and `cpu_physical_memory_write` paths, so
   we see when BSP enables tasks and writes config pointers. Useful
   diagnostic before any executor logic runs.
2. **Find the MOTbcommlib config-struct layout.** Background agent
   task: search u-boot source / GPL releases for
   `tasksetup_fec_rx_bd.c`, identify the offset of the BD ring
   base within the config struct.
3. **Implement TX executor.** When TCR[2] enabled and we have a
   non-NULL TX config pointer, follow the chain to the BD ring,
   walk descriptors, copy payload bytes via `dma_memory_read`,
   call `qemu_send_packet`. Mark BD as "sent", advance.
4. **Implement RX executor.** In `mpc5200_fec_receive`, look up
   the RX BD ring (via the same BSS pointer chain), find next
   free BD, write the frame, fire BestComm task-done IRQ via the
   FEC's IRQ output line.
5. **Verification:** at gate 3 we expect to see a guest-originated
   ARP or SYN to 169.254.254.252:21 in either the FTP server log
   or via `tcpdump -i lo`. At gate 4, we expect the FTP server log
   to show a session opened with source IP `169.254.254.254`
   (the guest), not 127.0.0.1 (QEMU's own startup probe).

### Risks

- **Config struct offset uncertainty.** If we can't find
  MOTbcommlib source, we'll need to reverse-engineer from the
  binary. Adds 1–2 days.
- **Task descriptor format.** The agent already documented the
  high-level approach. If actual descriptor walking turns out
  more complex than expected (e.g. the BSP uses non-standard
  Freescale offsets), we add 1–2 days of microcode-aware work.
- **CT296 KeySwitch gate.** Daniele noticed BSP strings about a
  hardware key switch that gates Ethernet. If observed, we'll
  need to fake the key in either GPIO state or by patching a BSS
  flag. Surfaces during gate 3 testing.

## Out of scope for this design

- Phase 3 protocol stubs (gates 10–12). Documented in PLAN.md as
  the "long pole" — separate spec when we get there. The Python
  AP server (`/home/kasper/WindowsVMDeploy/Builds/v2_vmp6000_simulator/ap_server.py`)
  becomes a test fixture / minimum-viable-spec at that point.
- ARCnet multi-node. Out of scope until we know whether toolkit
  acceptance requires it.
- Toolkit acceptance criteria research. Folded into pre-gate-10
  prep, not now.

## Acceptance criteria for this design

This spec is "done" when:

- PLAN.md is updated to reflect the rhythm decisions (decisions
  log section added, retire-when-done note added)
- Implementation plan for the BestComm executor is written
  (writing-plans skill)
- First commit on the executor work cites this spec

## References

- `PLAN.md` — gate roadmap (gates 0–12)
- `BSP_park_findings.md` — Daniele's diagnosis, gates 0–2 evidence
- `BSP_fec_bestcomm_findings.md` — task slots, register addresses,
  BSS pointers
- `PLAN_Kasper.md` — concrete steps for the FEC implementation
  track (steps 1, 2, 4, 5, 6 done; step 3 = this spec's BestComm
  executor)
- `docs/MPC5200_BestComm_Chapter13.md` — manual chapter on SDMA
- `docs/MPC5200_FEC_Chapter14.md` — manual chapter on FEC
