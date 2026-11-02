# Changelog

## 0.1.5 (unreleased)

- Read MSR 0xE2 (`PKG_CST_CONFIG_CONTROL`) at start and at each sleep: package
  C-state limit, CFG lock, demotion bits (`PKG_CST_CONFIG_CONTROL`,
  `Last 0xE2 at sleep`).
- Core CC6/CC7 removed from the meter (per-core MSRs; the sleep and wake
  actions may run on different cores, which produced garbage).

## 0.1.4 (unreleased)

- **Fix:** MSRs are only read on a whitelist of Intel client models (Haswell-ULT
  to Meteor Lake); 0.1.2/0.1.3 read MSR 0x632 on any family-6 CPU, which
  panics CPUs without it.
- C-state meter: PC2/3/6/7/8/9/10 and core CC6/CC7 share of each sleep
  (`Last C-state percent`); `-s0ixnocst` disables it. Replaces the PC10-only
  properties.

## 0.1.3 (unreleased)

- `s0ixfn=<mask>` boot-arg: only run the listed `_DSM` functions (bisecting
  wake problems), shown as `DSM function filter`. `-s0ixdbg` logs each call.

## 0.1.2 (unreleased)

- Package C10 meter (Intel MSR 0x632 vs TSC, GenuineIntel family 6 with LPIT
  only; `-s0ixnopc10` disables it): `Last PC10 percent`, `Last TSC ticks`.
  `s0ix-status.sh` computes sleep length, PC10 % and S0ix % from them.
- `Residency counter address` now shows the LPIT physical address (the
  map's address was IOMMU-translated with VT-d on); new `Residency counter at
  start` / `at wake` show the raw counter.
- `sleep-power-test.sh`: survives SSH drops (ignores HUP), writes
  `sleep-power-<date>.log`, warns on runs under 10 min, prints the kext's
  residency.
- `s0ix-status.sh`: kernel log through the unified log (IOLog does not reach
  `dmesg` on Tahoe).

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
