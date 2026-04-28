# BSP park diagnosis findings

## Verdict

**Hypothesis:** vxworks.out parks at 0x207fe8 waiting for FTP push from 169.254.254.253.

**Result:** REFUTED

**Real wait condition:** The CPU is oscillating between the `windExit` reschedule spin-loop
(0x207f70–0x207fd0) and the tick-interrupt handler at vector 0x700; 0x207fe8 is not a park
address but the interrupted NIP saved in SPRG0 when the tick fires.  The kernel idles
because all application tasks are blocked on network completion, and network boot (FTP from
host 169.254.254.252, not .253) never completes because the FEC Ethernet controller is not
functional in QEMU.

---

## Evidence

### Polled address

The spin loop (0x207f70–0x207fd0) polls **kernelState** at RAM address **0x00908310**
(first word of the `.bss` section, `.bss`+0x000).  The loop body is:

```
0x207f74:  lwz  r10, [0x9084a8]   ; r10 = taskIdCurrent TCB pointer
0x207f7c:  lwz  r4,  [0x99af58]   ; r4  = null/idle task TCB pointer
0x207f80:  cmpw r10, r4           ; current task == idle task?
0x207fa4:  lwz  r6,  [0x908310]   ; r6 = kernelState
0x207fa8:  cmpwi r6, 0            ; kernelState != 0  →  fast exit
0x207fb0:  ... call inner spin 0x207d90, then b 0x207f70  ; else spin again
```

Address 0x908310 is pure RAM, not MMIO.  None of the polled addresses are in the FEC
MMIO window (0xf0003000–0xf0003FFF) or any other peripheral range.

### What 0x207fe8 actually is

0x207fe8 is the `isync` instruction on the **fast-exit path** of `windExit`, reached only
after `kernelState` becomes non-zero:

```
0x207fd4:  xor  r3, r3, r3
0x207fd8:  lis  r4, 0x91
0x207fdc:  stw  r3, -0x7864(r4)    ; clear kernel work flag
0x207fe0:  lwz  r6, 0(r1)          ; restore saved MSR
0x207fe4:  mtmsr r6
0x207fe8:  isync                   ; <<<< reported NIP
0x207fec:  addi r1, r1, 8
0x207ff0:  blr
```

QEMU live register dump (taken at t ≈ 3.5 s) confirmed:

```
NIP  = 0x00000700      (EXT interrupt vector – CPU is IN the tick handler)
SPRG0 = 0x00207fe8     (VxWorks saves pre-interrupt NIP in SPRG0)
SRR0  = 0x00000700
MSR   = 0x00001000     (EE=0, ME=1 – external interrupts disabled in handler)
r31   = 0x0099af58     (null/idle task TCB address)
```

The CPU is **not** stalled at 0x207fe8; it is looping between the `windExit` spin
(0x207f70–0x207fd0) and the external-interrupt handler at 0x700.  Each tick interrupt
finds the CPU inside `windExit`, saves NIP→SPRG0 = 0x207fe8, and dispatches the handler.
The handler (via `bl 0x207efc` at 0x1762b4) calls `windExit` again, which calls the tick
queue processor at 0x302710 (`windTickAnnounce`), sets `kernelState`=1, and eventually
returns.  The outer `windExit` spin then loops because no higher-priority task has become
ready.

### Disassembly snippet

```
; --- windExit reschedule entry (no-interrupt variant) ---
0x207f58:  mfmsr  r3
0x207f5c:  stwu   r1, -8(r1)
0x207f60:  stw    r3, 0(r1)           ; save current MSR
0x207f64:  rlwinm r4, r3, 0, 0x11, 0xf  ; clear EE (disable interrupts)
0x207f68:  mtmsr  r4
0x207f6c:  isync
; --- outer spin top ---
0x207f70:  lis    r10, 0x91
0x207f74:  lwz    r10, -0x7b58(r10)   ; r10 = taskIdCurrent  [0x9084a8]
0x207f78:  lis    r4,  0x9a
0x207f7c:  lwz    r4,  -0x50a8(r4)    ; r4  = idleTask ptr   [0x99af58]
0x207f80:  cmpw   r10, r4
0x207f84:  beq    0x207fa0            ; if current==idle skip context-save
0x207f88:  lwz    r4, 0x50(r10)       ; check TCB priority?
0x207f8c:  cmpwi  r4, 0
0x207f90:  beq    0x207ff4            ; if non-zero: switch context out
0x207f94:  lwz    r5, 0x3c(r10)
0x207f98:  cmpwi  r5, 0
0x207f9c:  bne    0x207ff4
0x207fa0:  lis    r6, 0x91
0x207fa4:  lwz    r6, -0x7cf0(r6)     ; r6 = kernelState      [0x908310]
0x207fa8:  cmpwi  r6, 0
0x207fac:  bne    0x207fd4            ; kernelState!=0 → fast exit
; --- inner spin: yield & wait for tick ---
0x207fb0:  lwz    r3, 0(r1)           ; old MSR (interrupts enabled)
0x207fb4:  stwu   r1, -0x10(r1)
0x207fb8:  mflr   r7
0x207fbc:  stw    r7, 0x14(r1)
0x207fc0:  bl     0x207d90            ; enable intr, call windTickAnnounce, disable intr
0x207fc4:  addi   r1, r1, 0x10
0x207fc8:  lwz    r7, 4(r1)
0x207fcc:  mtlr   r7
0x207fd0:  b      0x207f70            ; spin forever if still idle
; --- fast-exit path (kernelState became non-zero) ---
0x207fd4:  xor    r3, r3, r3
0x207fd8:  lis    r4, 0x91
0x207fdc:  stw    r3, -0x7864(r4)     ; clear work flag
0x207fe0:  lwz    r6, 0(r1)           ; restore MSR
0x207fe4:  mtmsr  r6
0x207fe8:  isync                      ; <<<< SPRG0 saved here on tick interrupt
0x207fec:  addi   r1, r1, 8
0x207ff0:  blr
```

### Parked task

The idle/null task body is executing at 0x17615c ff., which calls `windExit` (0x1762b4:
`bl 0x207efc`).  r31 = 0x0099af58 = the null-task TCB.

Task name: **null task / idle task** (VxWorks internal `tIdleTask` / null task at 0x99af58)  
Blocked on: **windExit reschedule spin** — no ready-to-run task found; all application
tasks are pended on semaphores or message queues waiting for network completion.  
TCB status: task is "running" in the scheduler's view (it IS the current task); the
underlying reason no other task runs is FEC not delivering any packets.

### Protocol (if network wait)

The production boot strings embedded in the binary confirm standard VxWorks FTP boot:

- Protocol: **Standard anonymous FTP** (VxWorks `bootpd`/`ftpLib` client)
- FTP host (server): **169.254.254.252** (field `h=` in bootline — NOT .253)
- Target Ethernet IP: 169.254.254.254 (field `e=`)
- Username: `anonymous`
- Password: `test@cotas.dk`
- File: `ct6003/vxworks` (or `ct360/vxworks`, `ct296/vxworks`, `ct3603/vxworks`,
  `ct440/vxworks`, `def/vxworks` depending on board variant detected at runtime)
- Boot flags: `f=0x08` (BSP-specific; likely suppresses autoboot timeout)
- Cold-start fallback: `tffs=0,0(0,0)` — load from TFFS flash, no network required

The hypothesis named host **169.254.254.253** — this address does not appear anywhere in
the binary.  The actual FTP server is **.252**.

---

## Implications for Kasper's track

The QEMU emulation stalls in an infinite `windExit` idle spin because the FEC Ethernet
controller never becomes operational: without a working FEC model the FTP client in
`tBoot` or the equivalent boot task cannot reach host 169.254.254.252, so the firmware
image is never downloaded and no application tasks ever become ready.  The tick interrupt
fires correctly (QEMU log shows "tick #1 / #2 / #3, asserting EXT"), the MSR EE path is
functioning, and `windExit` itself is structurally sound — the spin is the intended idle
behaviour.  The fix path therefore runs through completing the MPC5200 FEC emulation (or
providing a `-netdev` tap bridge at 169.254.254.252 that answers anonymous FTP on TCP/21
with the correct image file) rather than anything in the interrupt or timer subsystem.
Once the FTP transfer completes, `tBoot` will set the relevant semaphore, task priorities
will unblock, `windExit` will find a ready task, and the CPU will stop spinning.  The
cold-start TFFS fallback (`tffs=0,0(0,0)`) is a viable alternative if a TFFS flash image
can be constructed and mounted as a QEMU block device, bypassing the network entirely.

---

## Open questions

1. **Which board variant is selected at runtime?**  The BSP picks among ct6003, ct360,
   ct296, ct3603, ct440, or def based on a hardware identifier read at boot; under QEMU
   it will likely fall through to "def/vxworks" or fault before that point.
2. **TFFS cold-start path viability:** The fallback bootline `tffs=0,0(0,0)` does not
   specify a host address, suggesting it loads from on-board flash.  Whether QEMU can
   provide a suitable TFFS/NOR-flash image for the cold-start path has not been tested.
3. **Exact task blocked on FTP:** The LR chain was not fully unwound beyond the null-task
   body; the specific application-level task (e.g., `tBoot`, `tRootTask`) that
   initiated the FTP transfer and is now pending on its completion semaphore was not
   identified because no symbol table is present in the ELF.
4. **f=0x08 boot flag semantics:** The exact meaning of this BSP-specific flag was not
   confirmed; it could affect autoboot timeout or ethernet link-wait behaviour.
