# Phase 2.5: mount node10 filesystem in QEMU

**Status:** placeholder — gated on Daniele's findings
**Owner:** TBD (likely Kasper)
**Branch:** TBD

---

## Why this exists

Node 10 files were dumped from a working Røye2 turbine via the Vestas
**Firedrake** protocol. We have the contents of `/ata0a/` on disk
locally. At some point QEMU has to expose those files to VxWorks so
the kernel finds `etc/startup.app` and the application binaries.

**Currently a placeholder** — the actual shape of this work depends on
Daniele's findings (`BSP_park_findings.md`):

- **If runmode boots from `/ata0a/`:** building a disk image and
  modeling an ATA controller is the *short* path — possibly shorter
  than the FTP plumbing. We may skip Phase 2 (FTP) entirely and go
  straight from current park → runmode boot.
- **If boot-mode FTP push writes TO the filesystem first:** we need
  the FS *and* the FTP plumbing, in order. The dumped Røye2 files
  give us the post-FTP filesystem state we want to reach.
- **If filesystem mount is itself what the kernel parks on:** Phase 2
  (FTP) was the wrong hypothesis. Phase 2.5 is actually Phase 2.

---

## Inputs (already available)

- Røye2 node10 dump on local disk (path TBD — check Kasper's notes)
- Firedrake-side knowledge: which paths exist, what file formats
- Vestas vault: `/bin/VMP_AP_IF/NsConfig.xml` and similar config layout
  references already known from earlier sessions

## Inputs (need from Daniele)

- Device backing `/ata0a/`: ATA controller, TrueFFS, CompactFlash,
  raw block, etc.
- Filesystem type: `dosFs` (FAT-style), `tffsDrv`, `iosDevAdd` raw
- Partition layout (MBR? bare FAT? raw at offset 0?)
- Boot-mode vs runmode trigger
- Required directory layout — what paths the BSP probes during init

---

## Likely shape of the work (sketch only — refine after Daniele)

### If `/ata0a/` is FAT on a CompactFlash via the MPC5200 ATA controller

1. Build a disk image: `mkfs.vfat`-style FAT16/32 image, populate
   with the Røye2 dump tree. Tools: `mtools` to copy in without
   mounting, or loopback-mount + `cp -a`.
2. Add an ATA controller to the QEMU `mac99` machine (or model the
   MPC5200's onboard ATA block — see manual section 17). Existing
   QEMU IDE/AHCI cores may be reusable.
3. Wire the disk image as `-drive
   file=node10.img,if=ide,format=raw`.
4. Verify VxWorks mounts it. Look for `/ata0a/` showing up in
   `iosDevShow` (run via the shell once boot completes).

### If `/ata0a/` is TrueFFS over flash

1. `tffsDrv` is non-trivial. Either:
   - Model a sufficient flash controller and let VxWorks's TrueFFS run
   - Or short-circuit `tffsDrv` calls to point at a host directory
2. Significantly more work than the FAT-on-IDE path. Hope it isn't
   this.

### If `/ata0a/` is something stranger

Document what Daniele finds, plan from there.

---

## Out of scope (until findings are in)

- Picking a concrete approach
- Building the disk image
- Modeling any controller
- Application-layer protocols (Phase 3)
- Multi-node ARCnet (Phase 4)

---

## When this unblocks

Daniele's `BSP_park_findings.md` lands → re-read this doc → decide:
1. Is Phase 2.5 actually before Phase 2? (i.e. is the park an FS issue?)
2. Or after Phase 2? (FTP succeeds → kernel mounts → app loads)
3. Or instead of Phase 2? (force runmode, skip FTP)

Then promote this from placeholder to a real plan.
