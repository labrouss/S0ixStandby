# Design: Modern Standby (S0ix) sleep for macOS on Tiger Lake

## Project requirements (2026-11-02)

- **Generic, not HP-specific.** The kext must work on any Intel (and later
  AMD) laptop with an LPS0 device. No hardcoded ACPI paths, MMIO addresses or
  device IDs. Everything is discovered at runtime: the LPS0 device by
  `_HID`/`_CID` `PNP0D80`, the `_DSM` GUIDs by their function-0 masks, the
  SLP_S0 counter from **LPIT**. Device-specific fixes belong in optional SSDTs
  or per-device notes, never in the kext.
- **All CI on GitHub Actions:** kext build (Xcode on a macOS runner,
  MacKernelSDK), SSDT compile (`iasl`), `shellcheck` on the scripts, release
  artifacts on tags. Nothing is built or released from a local machine.

## Route A component: LPS0 `_DSM` helper kext (phase N2)

Apple's Deep Idle never calls the Intel/Microsoft LPS0 `_DSM` (Q10). On the
HP that `_DSM` puts the EC to sleep (FINDINGS §4), so Deep Idle alone will
leak power. The helper adds exactly those calls and nothing else:

| | |
|---|---|
| Match | `IOACPIPlatformDevice` with `PNP0D80` (plain IOKit; **no Lilu**, no patching) |
| Probe | `_DSM(fn 0)` per GUID: Intel `c4eb40a0-…`, Microsoft `11e00d56-…`, AMD `e3f32452-…`; keep the masks |
| Sleep hook | Publish `IOPlatformSleepAction`; XNU calls it from `IOCPUSleepKernel()` **after all drivers have slept, before the CPUs halt, in thread context with interrupts on** (`iokit/Kernel/IOCPU.cpp:271`). That matches Linux's `prepare_late`. Order: display off (Intel/MS 3, AMD 4) → MS "sleep entry" 7 → entry (Intel/MS 5, AMD 2) |
| Wake hook | `IOPlatformWakeAction` (`IOCPU.cpp:363`), reverse order: exit 6 / AMD 3 → MS 8 → display on 4 / AMD 5 |
| Only when | `IOPMSystemSleepType == 7` (Deep Idle); with S3 do nothing |
| Not in | `IOPlatformQuiesceAction`: interrupts are off there and AML can sleep/stall/take the EC mutex |
| Diagnostics | IORegistry: found GUIDs and masks, last entry/exit timestamps, SLP_S0 counter delta (LPIT) per sleep, i.e. the phase 0 meter built in |
| Opt-out | `-s0ixoff` boot-arg |

**Diagnostics built in:** SLP_S0 residency per sleep (LPIT), package C-states
per sleep and per halt (MSRs, Intel model whitelist), MSR 0xE2, and the
**SLP_S0 blocker report**: the PCH IP power-gating status read inside the
halt, decoded with per-platform data (`S0ixPlatforms` in Info.plist: register
offset relative to the LPIT counter + IP names, keyed by CPU model). Adding a
platform = adding data, not code. Caveat: the snapshot is taken as the CPU
halts; an IP that gates itself a moment later is a false positive, so treat
the list as "likely" blockers and confirm by changing one thing at a time.

Function numbers come from Intel's and Microsoft's published LPS0 /
Modern Standby firmware-notification specs. Linux is only a cross-check
(GPL, see README licence note). Name: **S0ixStandby**
(bundle ID `com.labrouss.S0ixStandby`, repo `labrouss/S0ixStandby`).

Status: **proposal**. Nothing here is implemented. Items marked
**[verify]** must be checked against primary sources (Linux kernel source,
XNU source, Intel/ACPI specs) before code relies on them.

> **Update 2026-11-02 (phase 2a, [RESEARCH-2A.md](RESEARCH-2A.md)):** XNU
> already has an Intel S0 sleep path, **Deep Idle** (`acpi_idle_kernel()`),
> which AppleACPIPlatform uses when ACPI has `\_SB.LPS0` and `\_GPE.LXEN`.
> This is now **Route A** and comes first: an SSDT
> (`acpi/SSDT-DEEPIDLE.dsl`), plus at most a small helper kext for the LPS0
> `_DSM` calls. The architecture below is now **Route B**, the fallback. It
> is harder than first thought: by the time the S3 callback runs, XNU has
> already disabled the local APIC (`lapic_shutdown(true)`), so an MWAIT loop
> there cannot be woken by interrupts. Q1, Q2 and Q4 are answered in
> RESEARCH-2A; Q3 moves to experiment N1.

## Goal

Make macOS "Sleep" on the HP EliteBook 840 G8 end in **S0ix** (Modern
Standby) instead of ACPI S3, whose resume is broken in HP firmware.
Target: ~0.8 W in sleep (Linux achieves 0.76 W), wake on power button and
lid open, all apps and state preserved.

## Why this is possible at all

- macOS's normal sleep path already does most of the work Linux's s2idle
  needs: it puts drivers to sleep and quiesces the system **before** the
  final "enter S3" step.
- What differs is only the **last step**. S3 asks the chipset to cut power
  (write `SLP_TYP|SLP_EN` to the PM1 control register). S0ix instead tells
  the firmware "we're entering low-power idle" (LPS0 `_DSM`) and **idles the
  CPUs in the deepest C-state** until a wake event; the chipset then drops
  into S0ix on its own when every device is idle.
- So the kext replaces the final step and leaves everything before it alone.

## Background: how Linux does s2idle (reference implementation)

Linux `drivers/acpi/x86/s2idle.c` and `kernel/power/suspend.c` **[verify]**:

1. Freeze tasks, suspend devices (D3hot/D3cold, ASPM L1.2), enable wake GPEs.
2. Call the **LPS0 device `_DSM`** (Low Power S0 Idle):
   - Intel GUID `c4eb40a0-6cd2-11e2-bcfd-0800200c9a66`: function 1 = device
     constraints, 3 = screen off, 5 = LPS0 entry; on wake 6 = exit,
     4 = screen on.
   - Microsoft GUID `11e00d56-ce64-47ce-837b-1f898f9aa461`: 3/4 display
     off/on, 5/6 entry/exit, 7/8 Modern Standby entry/exit.
   - **[verify]** GUIDs, function numbers and the order Linux uses.
3. Idle all CPUs (deepest MWAIT C-state; the package reaches PC10, the PCH
   then reaches SLP_S0).
4. On any interrupt: if it is a wake source (power button, lid/EC GPE, ...)
   resume; otherwise go back to idle.
5. Exit: LPS0 exit + screen on, resume devices, thaw tasks.

Linux s2idle does **not** run `_PTS`/`_WAK` for S3 **[verify]**; macOS
does, before its final step. See open question Q3.

## Proposed architecture

```
IOPMrootDomain decides to sleep
  └─ drivers' setPowerState(sleep)  ← unchanged; this is what gets devices idle
      └─ _PTS(3)                    ← unchanged (see Q3)
          └─ final "enter S3" step  ← KEXT ROUTES THIS (Lilu)
               ├─ LPS0 _DSM: screen off, entry (Intel + MS GUIDs)
               ├─ arm wake sources: SCI (power button, lid/EC GPEs)
               ├─ stop the local timer that would wake us
               ├─ loop: MWAIT deepest C-state → check PM1/GPE status
               │         └─ not a wake event → clear, go back to MWAIT
               ├─ LPS0 _DSM: exit, screen on
               └─ return as if hardware woke from S3
  └─ normal macOS wake path (_WAK, drivers wake, apps resume)
```

Name: **S0ixStandby.kext** (working title). Lilu plugin, Tiger Lake only at
first, opt-in boot-arg (e.g. `-s0ixstandby`), with a `-s0ixoff`
escape hatch.

### Built-in safety net

Keep the existing `_S3` → soft-off ACPI patch. If the kext is not loaded or
declines (unknown CPU, missing LPS0 device), macOS falls through to the
patched S3 and the machine simply powers off cleanly, which is today's
behaviour. A kext failure costs nothing worse than what users already have.

## Measuring on macOS (phase 0 meter)

macOS has no `pmc_core` driver, so we need our own read-only meter:

| Counter | Source | Meaning |
|---|---|---|
| Package C10 residency | MSR `0x632` (`MSR_PKG_C10_RESIDENCY`) **[verify]** | CPU package reached PC10 (prerequisite for S0ix) |
| SLP_S0 residency | PMC/PWRM MMIO register, the one Linux `intel_pmc_core` reads for Tiger Lake **[verify offset in `drivers/platform/x86/intel/pmc/tgl.c` and how Linux finds PWRMBASE]** | Whole platform was in S0ix |
| PCH IP power-gating status, LTR | Same PMC block **[verify]** | Which IP (device) blocks S0ix |

Delivered as a tiny kext exposing values in the IORegistry plus a CLI
(`s0ixstandby-meter`) that samples them. Read-only: no risk.

## Open questions / risks

| # | Question | Why it matters |
|---|---|---|
| Q1 | Exactly which function performs the final S3 entry on Tahoe (XNU `acpi_sleep_kernel()` callback vs AppleACPIPlatform's ACPICA `AcpiEnterSleepState`)? Is it routable with Lilu on 26.x? | Determines the hook |
| Q2 | When the final step **returns** instead of losing power, does XNU take the normal wake path cleanly? (CPU state already saved; APs already offline) | If not, the kext must emulate a resume |
| Q3 | macOS has already run `_PTS(3)`. Does HP's `_PTS` prepare the EC/platform for S3 in a way that conflicts with S0ix? Is `_WAK` safe after S0ix? | May need an SSDT to make `_PTS/_WAK` S0ix-aware under Darwin (there is already an `SSDT-PTSWAK` in the EFI) |
| Q4 | Can we MWAIT with interrupts masked but still wake on SCI (MWAIT ECX[0] "interrupt break") **[verify]**, and stop the LAPIC/TSC-deadline timer safely? | Wake reliability + low power |
| Q5 | Which macOS drivers leave their device out of its low-power state? Candidates: CNVi Wi-Fi (AirportItlwm), Bluetooth (IntelBTUnified), VoodooI2C, NVMe (APST/L1.2), Thunderbolt 4/TCSS, iGPU display (WhateverXe, DC6), HDA (AppleALC + TGLDmic), xHCI | Each blocker means watts instead of milliwatts; phase 1 answers this |
| Q6 | Dark wakes (Power Nap, network maintenance) | Should be disabled initially: `pmset powernap 0 tcpkeepalive 0 proximitywake 0` |
| Q7 | Does the Intel ME/CSME expect anything for S0ix on this platform? | Linux needed nothing special, so probably not |

## Non-goals (for now)

- Other platforms (Alder Lake+, AMD): the design should not prevent them,
  but only Tiger Lake/T37 is tested.
- Connected Standby (network activity while asleep).
- Replacing hibernation: hibernation remains a separate, complementary
  feature (see [HIBERNATION-FALLBACK.md](HIBERNATION-FALLBACK.md)).
