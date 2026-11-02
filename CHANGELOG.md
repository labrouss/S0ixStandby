# Changelog

## 0.1.1 (unreleased)

- Meter: read LPIT through `IOACPIPlatformExpert::getACPITableData()` (Tahoe
  does not publish an "ACPI Tables" registry property); the property lookup
  stays as a fallback.
- New properties `LPIT source` / `LPIT status` explain a disabled meter.
- `s0ix-status.sh`: kernel log through `dmesg` (run with sudo).
- CI: `kextlibs` symbol check skipped (notice) on runners without x86_64 KPI
  libraries; `actions/checkout@v5`.

## 0.1.0 (unreleased)

- First kext: binds to any `PNP0D80` LPS0 device, probes Intel / Microsoft /
  AMD LPS0 `_DSM` masks, calls display-off/entry and exit/display-on from
  `IOPlatformSleepAction` / `IOPlatformWakeAction` on Deep Idle sleeps only.
- Built-in SLP_S0 residency meter from ACPI LPIT, published in the IORegistry.
- Boot-args `-s0ixoff`, `-s0ixnodsm`, `-s0ixdbg`.
- `SSDT-DEEPIDLE` enables macOS Deep Idle (AppleACPIPlatform `LPS0`/`LXEN`).
- GitHub Actions: privacy scan, ShellCheck, iasl, kext build + symbol check,
  release on tag.
