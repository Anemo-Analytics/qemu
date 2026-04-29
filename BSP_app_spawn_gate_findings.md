# Vestas-app spawn gate — findings (2026-04-30)

## TL;DR — the gate is the filesystem, not a flag

The previously-suspected gate at `*(0x00962e2c)` is a red herring.
The Vestas application tasks (`tApMain`, `tFirecrest`, `tFiredrake`,
`tNeon`) **do not exist as static taskSpawn call-sites in
`vxworks.out`**. They live in a runtime-loaded `/fs/etc/startup.app`
boot script that the BSP cannot find because we provide no `/fs/`
filesystem.

The BSP's response to repeated startup-script load failure is to
**give up and stay in VxWorks bootmode** — exactly the 21-task
idle daemon state we see.

## Evidence chain

### 1. The candidate flag-gate is dead code in our run

Disassembly at `0x001009d4` reads `*(0x00962e2c)`; if zero, sets
`*(0x0095a5a0) = 1` (Vestas-mode flag).

```
1009d4: lis  r9, 0x0096
1009d8: lwz  r9, 11820(r9)         # r9 = *(0x00962e2c)
1009dc: cmpwi cr7, r9, 0
1009e0: bf   cr7, 0x1009f0          # bf bit 30: branch when EQ=0,
                                    # i.e. r9 != 0 → SKIP set-flag
1009e4: lis  r9, 0x0095
1009e8: li   r10, 1
1009ec: stw  r10, 0x72a0(r9)        # *(0x0095a5a0) = 1
```

Polarity: `r9 == 0 → fall through → set flag`.

We instrumented:
- BSS-watcher in `mpc5200_diag_sample()`: log changes to `*(0x962e2c)`
  and `*(0x95a5a0)`.
- 5 new boot stations in `g_boot_stations[]`: gate-fn entry
  `0x100920`, the cmpwi+branch slots, and the error-print xref
  at `0x14adc0`.
- A force-zero in `mpc5200_apply_keyswitch_patches()` to set
  `*(0x962e2c) = 0` at first SLT tick.

After 30 000 samples × 100 µs = 3 s of virtual time:

```
GATE: *(0x00962e2c) init 0x00000000 at NIP=0x00100000
GATE: *(0x0095a5a0) init 0x00000000 at NIP=0x00100000  (Vestas-mode flag)
```

Neither slot ever changes for the entire 43 s run. **No STATION
HIT for `0x00100920`** — the gate-check function is never
invoked. The flag remains 0 because no code runs to set it.

So even if our force-zero patch worked semantically, it's
harmless — the function that *reads* the slot doesn't run.

### 2. The Vestas-app task names aren't in `vxworks.out`

```
$ strings /tmp/vxworks_romfs/vxworks.out | grep -E '^t[A-Z][a-zA-Z]{4,}$' | sort -u
tArcFastRx tArcRcv tArcService tBatMon tCapReceive tCapTimeout
tDcacheUpd tDhcpsTask tExcHiTask tExcTask tFecEndRecover
tFecEndRx tFtpdTask tKeybTask tLockd tLogTask tMountd tMtNtO
tNetSniff tNetTask tOsStatusService tPFTask tPoolRel tPortmapd
tRandom tRestart tRootTask tRtcControl tShell tShowRtc
```

The set is exactly the tasks we observe in our task-list dumper
(plus a few that aren't enabled in this run — tShell, tDhcpsTask,
tArcService, etc.). Conspicuously **absent**: `tApMain`,
`tFirecrest`, `tFiredrake`, `tNeon`, `tApplCommon`.

The binary does contain Firedrake-related code (error strings,
`.cpp` filenames embedded by the build system) — but those are
library code, not the spawn-site of a `tFiredrake` task.

### 3. The BSP boot-script loader explains everything

```
$ strings vxworks.out | grep -i startup
StartUp Script: %s
Executing startup script %s ...
Done executing startup script %s
Unable to open startup script %s
No file system present. resetStartupFail exiting
/fs/etc/startup.safe
startup failed too high. Skipping startup script
Startup failed more than 10 times. Do not start ANY startup script
Startup failed more than 8 times. Starting VxWorks in bootmode
/fs/etc/startup.app
Cannot find startup.app in runmode !
startup.app failed!
```

So the BSP runs a startup-fail counter:
- 1-7 fails: keep trying
- 8 fails: drop to **bootmode** (we are here)
- 10 fails: don't start any startup script at all

Bootmode == "boot the kernel + service daemons + sit waiting".
That's a precise description of what we see.

### 4. No filesystem image available locally

The pre-session investigation listed
`/home/kasper/Vestas/decompiled/firmware_v12.04.55/factory.romfs`
(claimed 13 MB) as a "verified asset". That file does **not
exist**. What's actually present is only:

- `factory.romfs_decompiled.c` — Ghidra-decompiled C source
- `vxworks.romfs_decompiled.c` — ditto
- `startup_manager_decompiled.c` — ditto
- `ApplLibVxWorks_decompiled.c` — ditto

These are **reconstructed source code**, not binaries. We cannot
hand them to QEMU as-is.

## What this means for the roadmap

### Gate 7 (Filesystem available) is the real bottleneck

The plan's gate-7 verification command was `iosDevShow lists /ata0a/`.
In this BSP image the mount path is `/fs/`, not `/ata0a/`. Adjust
gate-7 phrasing accordingly. The required artifact is a populated
filesystem at the canonical mount point with `/fs/etc/startup.app`
and whatever it transitively pulls in.

### Gates 6, 8, 9 cascade from gate 7

- Gate 6 (banner): startup.app prints the banner. No FS → no banner.
- Gate 8 (Vestas-app boots): startup.app spawns the tasks. No FS
  → no tasks.
- Gate 9 (AP/Firecrest/Firedrake listeners): Vestas-app tasks bind
  those sockets. No app → no listeners. The generic VxWorks
  daemons (FTP/portmap/NFS) **are** bound (verified with `nc` over
  hostfwd) — that part of gate 9 is closed.

### Path forward (not for this session)

Three branches, in order of effort:

1. **Find the real `factory.romfs` binary** — likely in Vestas
   firmware archives, on a real turbine's `/fs/`, or in the V4
   firmware_v12.04.55 release tarball (which we apparently only
   have the *analysis* of, not the binary). Then mount it via QEMU
   blockdev + add a synthetic ATA/SD/Flash device that the BSP's
   FS layer accepts.

2. **Reconstruct from `*_decompiled.c`** — heroic; only feasible
   if the decompilation is high-fidelity, which it usually isn't.

3. **Boot to bootmode and FTP-push the app** — match what the
   factory does. The bootmode FTP daemon **is** running
   (verified via `nc 127.0.0.1 2121`), so we can experiment:
   ftp the startup.app file, then send the BSP a command to
   re-attempt startup. Costs writing a small client + figuring
   out the BSP's "retry startup" command.

## Patches that remain in the tree from this session

Still in `mac_newworld.c`, both useful for future sessions:

- Gate watcher for `*(0x962e2c)` and `*(0x95a5a0)` in
  `mpc5200_diag_sample()`. Cheap; leave in. Will tell us if a
  later boot path actually exercises the flag.
- 5 new stations in `g_boot_stations[]` for gate-fn area. Cheap;
  leave in. Will tell us if a later run reaches them.
- The `stl_be_phys(&address_space_memory, 0x00962e2c, 0)` force-
  zero in `mpc5200_apply_keyswitch_patches()`. Harmless; leave
  in (it'd matter if some BSP path *did* exercise the gate but
  initialised the slot to non-zero from a side-effect). Negligible
  cost.

## Honest revised progress estimate

Yesterday: ~20-25% to gate 12.
Today: ~25-30% — we closed gate 4 (RX) and half of gate 9 (VxWorks
daemons listening), and we *correctly identified the real gate-7
bottleneck*. We did not unblock task-spawn — but we now know that
problem isn't a flag-patch, it's filesystem provisioning. The
distance to gate 12 hasn't shrunk much in absolute terms; what
shrunk is uncertainty about what's in the way.
