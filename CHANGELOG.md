# Changelog

## 0.1.12 (unreleased)

- Platform data `Non-blocking IPs`: IPs proven not to block S0ix are left out
  of the blocker report. Tiger Lake-LP: ISH (Linux s2idle 96 % with
  `intel_ish_ipc` never loaded).
- Removed `-s0ixd3net` (0.1.11): CNVi stayed powered and Wi-Fi did not
  resume.

## 0.1.11 (unreleased)

- `-s0ixd3net`: network-class PCI functions **with** a driver (e.g. CNVi Wi-Fi)
  go to D3hot in the sleep action (drivers already asleep) and back to D0 in
  the wake action (before drivers wake), as the Linux PCI core does on
  suspend. AirportItlwm stops the NIC on sleep but leaves the PCI function in
  D0. List in `D3 sleep network devices`.

## 0.1.10 (unreleased)

- `-s0ixd3`: driverless endpoints are now kept in D3hot from the boot scan on
  (re-applied before each Deep Idle), so firmware-managed blocks like ISH get
  time to power down. Memory-class devices are excluded.
- `Live SLP_S0 blockers`: the gating report refreshed every 10 s while awake.
- `DSM function filter` shown in hex.

## 0.1.9 (unreleased)

- **SLP_S0 blocker report:** reads the PCH IP power-gating status inside the
  Deep Idle halt and names the IPs still powered (`Last SLP_S0 blockers at
  halt`, raw `Last gating status at halt`, kernel log line). Infrastructure
  IPs (PMC, OPI, SBR, PSF, CSME, NPK_AON, reserved) are not reported.
  Platform data (register offset relative to the LPIT counter, IP names) lives
  in Info.plist `S0ixPlatforms`, keyed by CPU model; first entry: Tiger
  Lake-LP (`cpu-8c`). Platforms without data: report `unavailable`.

## 0.1.8 (unreleased)

- `-s0ixd3`: PCI endpoints without a driver (e.g. an Intel Integrated Sensor
  Hub, for which macOS has no driver) are put in D3hot for each Deep Idle and
  back to D0 on wake. Bridges and display devices are never touched. List in
  `D3 devices`; scanned 60 s after start and after each wake.
- Depends on IOPCIFamily.

## 0.1.7 (unreleased)

- `-s0ixpmcdump`: snapshot of the 4 KB PMC page holding the SLP_S0 counter
  (IP power-gating status, LTRs, ...) at boot and inside the Deep Idle halt
  (`PMC page at boot` / `at halt`); `macos/pmc-dump.sh` saves them as text.
  The counter mapping now covers its whole page.

## 0.1.6 (unreleased)

- Halt-window meter: `IOPlatformQuiesceAction` / `IOPlatformActiveAction` run
  on the boot CPU right around the Deep Idle halt (interrupts off, MSR reads
  only) and record boot-core CC6/CC7 plus package C-states for exactly that
  window (`Last halt C-state percent`, `Last halt TSC ticks`).

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
