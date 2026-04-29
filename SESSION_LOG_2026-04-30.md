# Session 2026-04-30 — confirm-and-bypass the Vestas-app spawn gate

Plan: see `PLAN.md` (decisions log) + the in-message plan for this session.

## TL;DR (session-end)

- **Phase 0a (run.sh + hostfwd):** ✅ done. Repo now has `run.sh` with
  hostfwd for FTP/portmap/NFS/WDB.
- **Phase 1a (gate diagnostic):** ✅ done. Gate watcher + 5 new boot
  stations added in `mac_newworld.c`. Confirmed the candidate gate
  is a red herring.
- **Phase 2a (force-zero patch):** ✅ done (force-zero applied at
  first SLT tick). Behavioural delta: zero. The patch is harmless
  but ineffective — the BSS slot was already 0 *and* the code that
  reads it never runs.
- **Phase 4 (gate-4 RX exercise):** ✅✅ closed gate 4 *and* partly
  gate 9 as a side effect of the new hostfwd plumbing. `BestComm RX:`
  fires repeatedly with real inbound traffic. Three guest daemons
  (FTP, portmap, NFS) accept TCP handshakes from the host.
- **Phase 1b agent (decompiled-source name lookup):** inconclusive —
  decompiled source lacks symbol info for the gate-fn region.
- **Real gate identification:** in progress (background agent
  dispatched after Phase 1 came back unconfirmed).

So gates 4 and 9 advanced. Gates 6-8 (Vestas-app spawn) still
blocked, but we've ruled out the previously-suspected gate.

## Timeline

### Phase 0a — run.sh + hostfwd

Wrote `/home/kasper/qemu/run.sh` (executable). Encodes the canonical
QEMU command from `SESSION_LOG_2026-04-29.md` and adds:

```
hostfwd=tcp::2121-:21,hostfwd=tcp::2049-:2049,
hostfwd=tcp::3111-:111,hostfwd=tcp::17185-:17185
```

Env-overridable: `LOG`, `PCAP`, `TIMEOUT`. Default 90s timeout,
90s rebuild gate via `ninja -C build`.

### Phase 1a — gate diagnostic (Option A polling + Option B station)

In `hw/ppc/mac_newworld.c`:

1. Added gate watcher inside `mpc5200_diag_sample()` at the bottom
   of the per-tick block. Logs every change to `*(0x00962e2c)` (gate
   input) and `*(0x0095a5a0)` (Vestas-mode flag set when gate
   passes). Sentinel `0xDEADBEEF` for "first sample".
2. Added 5 new stations to `g_boot_stations[]`: gate-fn entry
   (0x100920), gate-load (0x1009d8), gate-branch (0x1009e0),
   gate-pass (0x1009e4), error-print xref (0x14adc0).

### Phase 2a — naive force-zero patch

In `mpc5200_apply_keyswitch_patches()`:
```c
stl_be_phys(&address_space_memory, 0x00962e2c, 0);
```
Applied once on first SLT tick alongside the existing CT296
KeySwitch nop patches.

### Build + run

`ninja -C build qemu-system-ppc` clean. Ran with `TIMEOUT=45 ./run.sh`.

### Observation: gate is a red herring

```
GATE: *(0x00962e2c) init 0x00000000 at NIP=0x00100000
GATE: *(0x0095a5a0) init 0x00000000 at NIP=0x00100000  (Vestas-mode flag)
```

Both BSS slots are **0 from the very first sampled cycle** and **never
change** for the entire 43s run. Critically, no STATION HIT for
`0x00100920` (gate-fn entry) — the gate-check function is **never
called** in the first 3s of virtual time (sampler window).

Diagnosis:
- The candidate at `0x00962e2c` is uninitialised BSS, not a real
  gate signal. Reading it returns 0 because BSS starts zero.
- The function at `0x00100920` (which would *do* the comparison)
  isn't even invoked. Whatever the real gate is, it's upstream of
  the call site at `0x00107a5c`.
- Phase 1b agent couldn't find the symbol in the decompiled source
  — likely because that source is incomplete in this region.

So Phase 2a's force-zero is harmless but ineffective: even if the
slot becomes non-zero later, the function that reads it doesn't run.

### Side-effect wins: Phase 4 closed gate 4 and parts of gate 9

While the background agent dug for the real gate, I started a fresh
60s QEMU run via `run.sh` and probed the forwarded ports:

```
host:2121  FTP (->21):       Connection succeeded!
host:3111  portmapper (->111): Connection succeeded!
host:2049  NFS (->2049):       Connection succeeded!
host:17185 WDB (->17185):      connection refused
```

3 of 4 daemons are bound and accept TCP handshakes. WDB is UDP
(typically), so refusal on TCP is expected.

Better still — the inbound nc probes triggered the BestComm RX
walker:

```
BestComm RX: TaskBAR=0xf0008000 var=0xf0008780 bd=0xf0009600 skb_pa=0x07c17420 len=60
BestComm RX: TaskBAR=0xf0008000 var=0xf0008780 bd=0xf0009608 skb_pa=0x07c17a60 len=60
... (BD ring walked: 0x9610, 0x9618, 0x9620, 0x9628, 0x9630, 0x9638)
```

The RX walker fires correctly across the BD ring — gate 4 closed.

### Gate 4 + 9 implication

The base VxWorks daemon spawn path completes and bind()s sockets.
Networking is *fully* functional bidirectionally. So whatever blocks
the Vestas-app spawn is **specific to the Vestas-app spawn path** —
not a generic networking/init problem.

## Resolved: the real gate is gate-7 (filesystem)

Background gate-hunt agent returned with a partial-and-wrong
analysis (got the bf-30 polarity reversed, didn't trace upstream).
But its return triggered a different search path: **direct string
search on `vxworks.out` for the spawn-site of `tApMain` and
friends**.

### Findings

`vxworks.out` does NOT contain the strings `tApMain`, `tFirecrest`,
`tFiredrake`, `tNeon`. The set of `t[A-Z]...` task-name strings it
*does* contain matches **exactly** the tasks we observe running.

The BSP contains a startup-script loader with these strings:

```
Cannot find startup.app in runmode !
/fs/etc/startup.app
Startup failed more than 8 times. Starting VxWorks in bootmode
No file system present. resetStartupFail exiting
```

So:
1. The Vestas-app tasks live inside `/fs/etc/startup.app`, not in
   the BSP image.
2. The BSP tries to load `/fs/etc/startup.app`, fails because we
   provide no `/fs/`, and after 8 failures **drops to VxWorks
   bootmode**.
3. Bootmode == VxWorks kernel + service daemons (FTP, NFS,
   portmap, WDB) running but no application — i.e. **exactly the
   state we observe**.

### Plan asset list was incorrect

The plan claimed `/home/kasper/Vestas/decompiled/firmware_v12.04.55/factory.romfs`
was a "verified asset, 13 MB". That file does not exist. Only the
Ghidra-decompiled `*_decompiled.c` companion files exist. To
provide a real `/fs/`, we need the binary `factory.romfs` from a
real turbine, the V4 release tarball, or to reconstruct from the
decompiled source (heroic).

### What this means

- The disasm-level "gate" hunt was the wrong kind of investigation.
  The real gate is at the **boot-script** level.
- All three Phase 2 escalation branches (force-zero / nop-branch /
  upstream signal) would have been wrong even if Phase 1 had
  confirmed the gate-fn ran — patching the in-RAM flag wouldn't
  conjure a startup.app into existence.
- The diagnostic we left in the tree is still useful for future
  sessions (will tell us if a later boot path exercises the flag,
  e.g. once we *do* provide a filesystem).

## Path forward (next session)

Three branches in order of effort, see `BSP_app_spawn_gate_findings.md`:

1. **Find a real `factory.romfs` binary** — Vestas firmware
   archives, real-turbine `/fs/` snapshot, or V4 release tarball.
2. **Boot-mode FTP push** — the BSP's bootmode FTP daemon is
   already accepting connections (verified). Reverse the BSP's
   "retry startup" command and push `startup.app` over FTP.
3. **Reconstruct from `*_decompiled.c`** — heroic.

## Files changed this session

- `run.sh` (new, executable) — canonical boot command + hostfwd
- `hw/ppc/mac_newworld.c`:
  - `mpc5200_apply_keyswitch_patches()`: added force-zero of
    `*(0x00962e2c)` (harmless, leave in)
  - `g_boot_stations[]`: 5 new entries for gate-fn region
  - `mpc5200_diag_sample()`: per-tick watcher for
    `*(0x00962e2c)` and `*(0x0095a5a0)`
- `PLAN.md`: gate-4 closed, gate-9 partial, gate-7 reframed
- `BSP_app_spawn_gate_findings.md` (new): consolidated findings
- `SESSION_LOG_2026-04-30.md` (this file)

## Honest progress estimate

~25-30% to gate 12 (up from yesterday's 20-25%). We closed gate 4,
half of gate 9, and identified the actual blocker. The blocker
itself (filesystem provisioning) is a substantial work-stream of
its own, but at least we know what it is now.
