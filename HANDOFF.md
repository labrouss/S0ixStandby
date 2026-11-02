# Handoff prompt

Paste the block below as the first message of a new chat, and attach this
project as a zip (or the `docs/` files plus `linux/`).

```
New project: S0ixStandby — real sleep for macOS on my HP EliteBook 840 G8 hackintosh via Modern Standby (S0ix)
instead of ACPI S3. All docs are attached (README, docs/FINDINGS, DESIGN, PLAN, TESTING, HIBERNATION-FALLBACK,
linux/ scripts) — read them first.

Machine: HP EliteBook 840 G8, Tiger Lake i7, HP platform T37, BIOS 01.19.00 (latest 01.24.02).
macOS Tahoe 26.7.1 (also Sequoia/Ventura), OpenCore 1.0.8, SMBIOS MacBookPro16,2, Intel Mac (no Homebrew).
Kexts: Lilu, RTCMemoryFixup, VirtualSMC, USBMap, AMFIPass, AppleALC, TGLDmic, BrightnessKeys, CpuTscSync,
ECEnabler, VoodooI2C(+HID/Synaptics), SMCBatteryManager, VoodooPS2, AirportItlwm, IntelBTUnified, WhateverXe.
EFI repo: labrouss/EliteBook840G8. Sleep today = clean power-off (_S3 SLP_TYP patched 5->7).

Key facts:
- S3 resume hangs in HP firmware (same on Linux). Not fixable from the OS.
- Linux s2idle on this laptop: 0.76 W, 98% S0ix residency over 10.8 min, clean wake. s2idle is the firmware default.
- Hibernation attempt failed earlier (005 CMOS resets — now fixed by RTCMemoryFixup rtcfx_exclude=80-AB +
  DisableRtcChecksum — and a hung restore, not retried yet).

Requirements: the kext must be GENERIC (any laptop with a PNP0D80 LPS0 device; runtime discovery via _DSM masks
and LPIT, no hardcoded paths/addresses), and ALL CI runs on GitHub Actions (kext build, iasl, shellcheck, releases).

Key research (docs/RESEARCH-2A.md): XNU (xnu-12377, Tahoe) already has an Intel S0 sleep path, "Deep Idle"
(acpi_idle_kernel), enabled by AppleACPIPlatform when ACPI has \_SB.LPS0 and \_GPE.LXEN returning 1. Apple never
calls the LPS0 _DSM, and HP's _DSM puts the EC to sleep, so a small generic IOKit kext (no Lilu) calls _DSM
3/(MS 7)/5 from IOPlatformSleepAction and 6/(MS 8)/4 from IOPlatformWakeAction when IOPMSystemSleepType == 7.
The original Lilu S3-replacement hook is only the fallback (XNU disables the LAPIC before the S3 callback).

Plan: L Linux baseline (done; selftest tool still to run locally) -> N0 macos/check-deepidle.sh (read-only) ->
N1 SSDT-DEEPIDLE on the USB EFI -> phase 0 meter -> N2 helper kext + GitHub CI -> blockers -> polish.
Hibernation fallback runs in parallel (restore 005 NOT fixed yet: boot.efi writes RTC; try AppleRtcRam).

Next step: <say what you want to do first, e.g. "run the Linux baseline" or "start phase 0">.
```
