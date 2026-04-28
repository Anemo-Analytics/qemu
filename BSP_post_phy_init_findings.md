# BSP post-PHY-init failure trace

Continuation investigation: after the PHY init fixes
(`ed449f1b6e`, `1bc3110083`), the BSP enables FEC then enters
`m5200FecRestart` retry cycle. This doc identifies the actual
restart trigger.

## TL;DR

**The blocker is a hardware key-switch sentinel: `CT296 KeySwitch`.**

`m5200FecStart` checks the cached keyswitch state at `0x12d390`:

```
0x12d388: bl   0x112630      ; read [0x452140] (cached keyswitch state)
0x12d38c: cmpwi r3, 0
0x12d390: beq  cr7, 0x12d7e8 ; if 0 (LOCAL) → "simulate PHY init error" → retry
```

The keyswitch is **file-based simulation** — `FpgaCT296SimKeyGet`
reads `/fs/fpga_ct296_key.txt`. We have no filesystem at this stage,
so fopen fails, the cache normalizes to a value that the BSP treats
as "LOCAL" → restart loop.

## Function map (verified)

| Address | Function |
|---|---|
| `0x12d00c` | `m5200FecStart` (entry: `stwu r1,-0x60(r1)`) |
| `0x12f12c` | `m5200FecPhyInit` (called via fp `[0x452b30]`) |
| `0x112630` | `CT296DioGetKeySwitch` cache reader (returns `[0x452140]`) |
| `0x1125c4` | KeySwitch cache updater |
| `0x1124fc` | Reads `/fs/fpga_ct296_key.txt` via fopen/fscanf |
| `0x12adec` | `m5200FecCT296KeyGet` wrapper (returns 2 if raw < 0) |
| `0x12ae34` | `m5200FecRestart` |
| `0x12b8b4` | `m5200FecStop` |
| `0x12c184` | `m5200FecSdmaTaskInit` (called via fp from EndLoad, NOT from Start) |

## Architectural insight: when does TCR get written?

**`m5200FecSdmaTaskInit` is called from `m5200FecEndLoad`, not from
`m5200FecStart`.** That changes our mental model:

- `EndLoad` → allocates BestComm tasks (TaskCreate populates
  `[0x8cfc00]`, `[0x8cfc04]`) → calls `TaskSetup_FEC_TX/RX` which
  writes TCR at `MBAR+0x1220/0x1222`
- `Start` → enables FEC (ECR=2), schedules netJob `m5200FecMuxTxRestart`

We see BSS pointers populated but **no TCR writes**, which suggests
`TaskSetup_FEC_TX/RX` never ran or returned before the TCR write.
Most likely: `TaskCreate` allocated the structs but `TaskSetup`
either wasn't reached or failed (looking for log strings
`"failed to setup TX task"`/`"failed to setup RX task"` at
`0x38b7c0`/`0x38b7f0`).

This is a secondary issue. Once we get past the keyswitch gate, we'll
see whether TaskSetup actually runs.

## CT296 KeySwitch internals

| Address | Meaning |
|---|---|
| `[0x452140]` (.data, file `0x3521c0`) | Cached key state (0=LOCAL, 1/2=non-LOCAL, -1=invalid/uninit) |
| `[0x452144]` (.data, file `0x3521c4`) | Cache valid flag (1=valid, 0=stale) |
| `[0x452148]` (.data, file `0x3521c8`) | Last update tick (-1 = never) |
| `[0x452b30]` | PhyInit fp = `0x12f12c` |

This BSP build uses `FpgaCT296SimKeyGet` — file-based simulation, no
real GPIO. Initial cache value `0xFFFFFFFF` (-1). Normalization (per
agent): `0/1/2` kept, `>2` → `-1`.

`m5200FecCT296KeyGet` (`0x12adec`) converts `-1` to `2` (non-LOCAL).
But the check at `0x12d390` calls `0x112630` directly, returning the
raw cached value, not the normalized one.

## Fix options

### Option A — binary patch (chosen, see commit)

NOP out the conditional branch at `0x12d390`:

- Address: VMA `0x12d390`
- Original: `0x419e0458` (`beq cr7, 0x12d7e8`)
- Patched:  `0x60000000` (`nop`)

This makes the keyswitch check unconditionally pass. Implemented as a
load-time patch in `mac_newworld.c` so the original binary stays
unmodified.

**Caveat:** there may be other keyswitch checks elsewhere — e.g. in
`m5200FecRestart` at `0x12ae60` (`409e0040`). Watch for those after
the first patch.

### Option B — provide `/fs/fpga_ct296_key.txt` (deferred)

The proper fix is to give the BSP a real filesystem with a key file
containing `1` or `2`. This is gate 7 work (filesystem mount).
Document for later.

### Option C — seed BSS values (alternative to A)

Patch the .data section:

- File offset `0x3521c0` (= `[0x452140]`): `0xFFFFFFFF` → `0x00000002`
- File offset `0x3521c4` (= `[0x452144]`): `0x00000001` → `0x00000000`

Slightly more invasive than A; A is simpler and more localized.

## Other things to watch for after this patch

Per agent recommendations:

1. **ECR.RESET self-clear timing** — our model self-clears RESET
   (bit 0) immediately on write. The BSP's reset poll loop at
   `0x12d654→0x12d0cc` reads ECR back and tests bit 0; if it's still
   set, infinite "Reset error" log. Confirm we self-clear.

2. **Bus error path** — `m5200FecInt: bus error, restart chip` log
   string at `0x38b1c0`. EIR has bus-error bits that, if set,
   trigger restart. Make sure our FEC doesn't spuriously assert
   bus error.

3. **MII watchdog from netTask context** — periodic re-reads of PHY
   from netTask. PHY values must be stable when FEC is enabled.

## Sources

- `/tmp/vxworks_romfs/vxworks.out` — VxWorks ELF
- Background agent disassembly session (full report in conversation
  history)
- `BSP_phy_init_findings.md` — prerequisite findings
