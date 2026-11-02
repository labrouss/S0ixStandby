# Changelog

## 0.1.0 (unreleased)

- First kext: binds to any `PNP0D80` LPS0 device, probes Intel / Microsoft /
  AMD LPS0 `_DSM` masks, calls display-off/entry and exit/display-on from
  `IOPlatformSleepAction` / `IOPlatformWakeAction` on Deep Idle sleeps only.
- Built-in SLP_S0 residency meter from ACPI LPIT, published in the IORegistry.
- Boot-args `-s0ixoff`, `-s0ixnodsm`, `-s0ixdbg`.
- `SSDT-DEEPIDLE` enables macOS Deep Idle (AppleACPIPlatform `LPS0`/`LXEN`).
- GitHub Actions: privacy scan, ShellCheck, iasl, kext build + symbol check,
  release on tag.
