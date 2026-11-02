# Phase 2a research: how macOS enters sleep on Intel (Q1–Q4)

Source: XNU **xnu-12377.1.9** (macOS 26 Tahoe, `apple-oss-distributions/xnu`).
Line numbers refer to that tag. AppleACPIPlatform is closed source; anything
about it is marked **[unverified]** until checked on the Tahoe binary.

## Headline

**XNU already has an S0 sleep path for Intel ("Deep Idle"),** separate from
S3. AppleACPIPlatform can call `acpi_idle_kernel()` instead of
`acpi_sleep_kernel()`. The kernel then idles the boot CPU in its deepest
C-state through XCPM and returns on wake, with the local APIC still enabled.
That is the s2idle model the design was going to build by hand.

On real Macs, AppleACPIPlatform enables this path when the ACPI tables
contain `\_SB.LPS0` and `\_GPE.LXEN` returning 1. That publishes
`IOPMDeepIdleSupported = true`. In 2017 a hackintosh (Dell XPS 13 9360,
Kaby Lake) reached `SleepType: Deep Idle` → `ACPI S0 WAKE` with only those
two methods added. It woke, but the platform stayed partly powered (power LED
on, SSD audible) [Pike R. Alpha, "Debugging sleep issues", 2017, incl.
comments].

**Consequence:** the cheapest first experiment is an SSDT, not a kernel hook
(see phase N in [PLAN.md](PLAN.md)). The custom S3-replacement hook (Route
B) gets much harder, see Q2.

## Q1: where is the final S3 step?

- `acpi_sleep_kernel(func, refcon)` (`osfmk/i386/acpi.c:182`) is called by
  AppleACPIPlatform with `func` = AppleACPIPlatform's own "enter S3" routine.
- `acpi_sleep_cpu(func, refcon)` (`osfmk/x86_64/start.s:350`) saves CPU
  state, then `call *%rsi`, i.e. `func`. On success it never returns.
  Hardware wake comes back through `acpi_wake_prot` (real-mode bootstrap).
- So the final step is the **AppleACPIPlatform callback**, not an XNU
  function. Route B would have to Lilu-route that closed function
  **[unverified: needs disassembly on Tahoe]**.

## Q2: what if the final step returns?

- `start.s:422`: *"sleep failed, no cpu context lost"* → `jmp wake_restore`,
  followed by the normal post-wake code in `acpi_sleep_kernel()` (TSC
  rebase, MTRR/PAT, microcode, LAPIC re-init, timers). **A returning
  callback is handled cleanly.**
- **But:** before calling it, `acpi_sleep_kernel()` has already:
  - put every other CPU in the "off" state (`pmCPUExitHaltToOff`, `:205`);
  - run `lapic_shutdown(true)` (`:213`, `osfmk/i386/lapic_native.c:612`).
    This masks the LVTs, software-disables the APIC **and clears
    `IA32_APIC_BASE.EN`**.
- With the LAPIC globally disabled, no external interrupt (SCI included)
  can reach the CPU. An MWAIT loop inside the S3 callback could only wake by
  polling, which prevents PC10/S0ix. Route B would have to re-enable and
  re-program the LAPIC and IOAPIC routing itself: fragile and
  version-specific.

## Q3: `_PTS` / `_WAK` around Deep Idle

Unknown. XNU does not evaluate ACPI methods; AppleACPIPlatform does. Answer it
empirically: `SSDT-DEEPIDLE` logs a `Debug` string, and the same can be done
in `_PTS`/`_WAK` (there is already `SSDT-PTSWAK`) to see which ones run on
a Deep Idle sleep. **[experiment N1]**

## Q4: CPU idle and wake in Deep Idle

`acpi_idle_kernel()` (`acpi.c:428`) already does what Q4 asked for:

1. Called with interrupts disabled; all other CPUs already offline in their
   deepest C-state.
2. Cancels the pending timer deadline (`setPop(0)`), briefly re-enabling
   interrupts to drain a pending LAPIC timer/PMC interrupt.
3. `IOCPURunPlatformQuiesceActions()`, then calls back `func(refcon)` so
   AppleACPIPlatform can do its last step **[unverified: what it does there]**.
4. `pmCPUHalt(PM_HALT_SLEEP)` → `pmDispatch->pmCPUHalt` (XCPM): deepest
   C-state. On the boot CPU it **returns when an interrupt arrives**.
5. Rebases time (`rtc_sleep_wakeup`), `IOCPURunPlatformActiveActions()`, and
   returns. The LAPIC stays enabled throughout, so the SCI can wake it.

## Sleep type selection

- `IOPMrootDomain::evaluateSystemSleepPolicyEarly()` publishes
  `IOPMSystemSleepType` (`IOPMrootDomain.cpp:5673`). `7` =
  `kIOPMSleepTypeDeepIdle` (`IOPMPrivate.h:896`); `2` = normal sleep (S3).
- On x86 the policy comes from a platform handler or the board's
  `IOPlatformSystemSleepPolicy` table. In the 2017 thread, removing
  `IOPlatformSystemSleepPolicy` from the X86PlatformPlugin board plist logged
  *"Failed to find sleep policy for this platform! Disabling deep sleep."*
  and went back to S3. So the **SMBIOS board-id may matter**
  (MacBookPro16,2 = `Mac-5F9802EFE386AA28`) **[check with
  macos/check-deepidle.sh]**.

## New open questions

| # | Question | How to answer |
|---|---|---|
| Q8 | Does Tahoe's AppleACPIPlatform still contain the Deep Idle path (the evidence is from Sierra)? | `macos/check-deepidle.sh`: strings `LPS0`, `LXEN`, `Deep Idle`, `S0 WAKE` in the kernel collections |
| Q9 | Does the board-id's sleep policy allow Deep Idle? | Same script: X86PlatformPlugin plist for `Mac-5F9802EFE386AA28`; `IOPMDeepIdleSupported` / `IOPMSystemSleepType` |
| Q10 | Does Apple call the Intel/Microsoft LPS0 `_DSM` (screen off / entry)? Probably not: Apple uses its own `LPS0` method. Without it, HP's EC and some IPs may stay powered (likely the XPS "LED on, SSD whining" case) | Experiment N1 + phase 0 meter. Fix options: SSDT glue (call `PEPD._DSM` 3/5 and 6/4 from whatever Apple evaluates) or a **small** kext calling them from a late sleep/early wake callback; no Lilu routing needed |
| Q11 | What wakes Deep Idle on this HP: power button, lid (EC `_Qxx` → GPE), keyboard? `SSDT-GPRW` zeroes `_PRW` for GPE 0x6D/0x69/0x0D | **Partly answered:** the power button is the fixed-feature button, and Linux wakes on `ACPI fixed event` through the SCI. Lid/keyboard: experiment N1 |

**Q10, from the baseline DSDT:** HP's PEPD `_DSM` (Intel GUID only, fn 0–6)
does real work: fn 3/4 put the EC into and out of its sleep mode, fn 5/6
toggle `GUAM` and TCSS. Apple won't call these, so Route A needs the generic
helper kext (DESIGN.md, phase N2). The hook point is `IOPlatformSleepAction`
/ `IOPlatformWakeAction` (`iokit/Kernel/IOCPU.cpp:271/363`,
`IOPlatformActions.cpp:203/211`): after drivers sleep, before the CPUs halt,
interrupts enabled. Unlike `IOPlatformQuiesceAction`, AML is safe there.

## Effect on the design

| | Route A: native Deep Idle (new) | Route B: replace S3 callback (original) |
|---|---|---|
| Kernel code | None at first; maybe a small `_DSM` helper kext later | Lilu route of a closed AppleACPIPlatform function |
| CPU idle / wake | XNU + XCPM, as on real Macs | Own MWAIT loop; LAPIC already disabled (Q2) |
| Risk | Low: SSDT on the USB EFI | High: hangs/panics, per-macOS-version patches |
| First test | Hours | Weeks |

Route A first; Route B only if Q8 says the path is gone from Tahoe.

## Sources

- XNU xnu-12377.1.9: `osfmk/i386/acpi.c`, `osfmk/x86_64/start.s`,
  `osfmk/i386/lapic_native.c`, `osfmk/i386/pmCPU.c`,
  `iokit/Kernel/IOPMrootDomain.cpp`, `iokit/IOKit/pwr_mgt/IOPMPrivate.h`
  — <https://github.com/apple-oss-distributions/xnu>
- Pike R. Alpha, "Debugging sleep issues…" (2017-01-12) and comments
  (LPS0/LXEN, `Scope (\_SB)` fix, XPS 9360 Deep Idle log, sleep-policy
  message) — <https://pikeralpha.wordpress.com/2017/01/12/debugging-sleep-issues/>
- Linux `drivers/acpi/x86/s2idle.c`: LPS0 `_DSM` GUIDs and function numbers
  (Intel `c4eb40a0-…` 1/3/4/5/6, Microsoft `11e00d56-…` 3/4/5/6/7/8). This
  confirms the values in DESIGN.md.

## N0 result (2026-11-02, `deepidle-check-20261102-114859`, macOS 26.7.1 25G241)

| Check | Result |
|---|---|
| Kernel | xnu-12377.161.15, `_acpi_idle_kernel` present: Deep Idle exists on the kernel side |
| `IOPMSystemSleepPolicyHandler` | **Yes**: a platform policy handler is installed |
| Board plist `Mac-5F9802EFE386AA28` | Has `IOPlatformSystemSleepPolicy` (`SLPT` v2, 328 bytes) and `UnifiedSleepSliderPref = true`; ~40 board-ids carry a policy |
| `IOPMDeepIdleSupported` | Absent: expected without the SSDT (confirmed again 11:53). `Supported Features` lists `DeepSleep` and `UnifiedSleepSliderPref`: `DeepSleep` is the standby/hibernate feature, **not** Deep Idle |
| PEPD | Present, `compatible = PNP0D80` |
| AppleACPIPlatform strings (Q8) | **Inconclusive**: `strings` returned nothing from either kernel collection (not even generic keys), so the method failed, not the search. Script now uses `grep -a` on the raw files |
| Side note | Boot-time ACPI errors in `EC0._REG`, `EC0.ECRI`, `EC0._Q09` (`AE_BAD_PARAMETER` from the EmbeddedControl handler). Harmless today, but N2's `_DSM` 3/4 write `EC0.SLPT` through the same handler: check before N2 |

Verdict: nothing blocks N1. The SSDT test itself answers Q8: if
`IOPMDeepIdleSupported` appears after boot, AppleACPIPlatform still reads
`LPS0`/`LXEN`.

## N1 step 1 (2026-11-02 12:03): Q8 answered, YES

With `SSDT-DEEPIDLE.aml` loaded from the USB EFI, macOS 26.7.1 publishes
`"IOPMDeepIdleSupported" = Yes`. **Tahoe's AppleACPIPlatform still reads
`\_SB.LPS0` / `\_GPE.LXEN`.** Next: one sleep, to see whether the policy picks
type 7 and whether it wakes.

## N1 step 2 (2026-11-02 12:05–12:10): first Deep Idle sleep → wake → GPU panic

| Evidence | Reading |
|---|---|
| Power LED stayed **solid** (Linux: breathing) | Deep Idle entered (no power-off); EC never told: no LPS0 `_DSM` fn 3 (expected, Q10) |
| Panic "Last Sleep" TSC delta 781,260,214,508 ticks | ≈ **260 s asleep** at 3.0 GHz (12:05:18 → ~12:09:38) |
| Wake → panic | **1.16 s** after wake, CPU 5, in `IOWorkLoop` → `AppleIntelTGLGraphics::handleSoftwareContextSwitchInterrupt` → **WhateverXe 0.6.1** `wrapTGLCSBProcess`: `context-ID-mismatch`, `lastHWS = ffffffff…` |
| `pmset`: `Failure during wake … 0x…28` | Trace point `0x28` = `kIOPMTracePointWakeCapabilityClients`: platform, CPUs and drivers woke; died at graphics power-on |
| Unified log 12:05:15–12:09:59 | Empty: lost in the hard power-off |

**Reading:** the platform side works: Deep Idle entry, ~4 min idle, a real wake
event, CPUs and drivers back. The iGPU came back with its context / hardware
status page gone (`0xFFFFFFFF` reads), and WhateverXe deliberately panics on
the context-ID mismatch. On S3 the GPU driver does a full re-init; on Deep
Idle (type 7) it apparently doesn't.

This is a **Tiger Lake iGPU / WhateverXe issue, not a generic one**: it belongs
in a WhateverXe report or an optional per-GPU workaround, not in the generic
kext.

**Wake source: the power button** (user confirmed: charger not connected, and
the display briefly lit dimly on the press). **Q11 answered for the power
button:** the fixed-feature event wakes Deep Idle with no help. Not yet clear:
whether *every* press wakes. The user reported a press without effect at
~12:08; the wake happened at ~12:09:38. Check this in N1b. Lid and keyboard
are still untested.

Next: **N1b**, the same test with WhateverXe's acceleration off. Removing
WhateverXe entirely stops the laptop booting (user, 12:27). WhateverXe has
`-wex-noaccel` (`kern_xe.cpp:3864/3874`): it keeps the display path and drops
the TGL accelerator, which is where the panic happened (`wrapTGLHostCSBIDPanic`
→ `tglHostCSBFatal("context-ID-mismatch")`, `kern_xe.cpp:15596`). Upstream
README: *"full system sleep/wake remain unverified"*; CHANGELOG: *"System
sleep/wake remains unsupported"*. So this is a **known WhateverXe limitation
for any system sleep**, not something specific to Deep Idle.

## N1b (2026-11-02 12:41–12:48): first clean Deep Idle sleep/wake on the HP ✅

Setup: USB EFI + `SSDT-DEEPIDLE.aml` + boot-arg `-wex-noaccel` (WhateverXe display
only, no TGL acceleration). On battery, `macos/sleep-power-test.sh`.

| Item | Result |
|---|---|
| Sleep type | **7 (Deep Idle)** |
| AppleACPIPlatform | `ACPI SLEEP` → `ACPI S0 WAKE`, `Wake reason: PWRB (User)` |
| pmset | `Wake from Deep Idle [CDNVA] : due to PWRB/User` |
| Asleep | 6.5 min |
| Used | 19 mAh = 0.22 Wh (coarse counter) |
| **Average** | **2.01 W** (Linux s2idle: 0.76 W) |
| Slow wake acks | AppleIntelFramebuffer ~1 s, ApplePS2Controller 0.7 s, IOUSBMassStorage 0.7 s, AppleIntelMEClientController 1 s |

Log timestamps across the sleep look 35 ms apart because XNU rebases time on
Deep Idle wake (`deep_idle_rebase`), so wall-clock time spent asleep doesn't
show up in the log.

**Conclusion: Route A works on Tahoe on non-Apple hardware.** Remaining gap
to Linux: ~1.25 W. Expected contributors: (1) no LPS0 `_DSM`, so the EC stays
awake (solid LED instead of breathing), no `GUAM`, no TCSS `DPOF`; (2) unknown
whether the PCH reaches SLP_S0 at all. The kext's built-in meter (LPIT counter)
answers (2).

Daily use additionally needs WhateverXe acceleration to survive sleep: an
upstream issue (both S3 and Deep Idle; README: sleep "unverified").
