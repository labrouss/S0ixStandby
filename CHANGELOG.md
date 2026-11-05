# Changelog

## 0.4.0 (unreleased): GPU reset on by default

- The GPU reset at a Deep Idle wake is now **the default** on Intel
  Gen11/Gen12 iGPUs (Ice Lake, Tiger Lake, Rocket Lake, Alder Lake, Raptor
  Lake). It acts only when the GPU kept its state (an accelerator such as
  WhateverXe was running), and only after Deep Idle (S3 wakes are left
  alone). `-s0ixnogpureset` turns it off; `-s0ixgpureset` is still accepted.
- Without an active accelerator at sleep (e.g. `-wex-noaccel`) the GPU is not
  touched at wake at all.
- `-s0ixgpuprobe` is now a debugging option only: it also probes an idle GPU
  (canary) and keeps the NVRAM record. No NVRAM writes by default.
- Verified: 4/4 accelerated wakes (1–40 min) clean with WhateverXe 0.7.0.

## 0.3.4 (unreleased)

**Accelerated graphics survive Deep Idle** with `-s0ixgpureset` (WhateverXe
0.7.0 on Tiger Lake, first time): with acceleration the iGPU keeps its
state through Deep Idle (`retained`), and Apple's S3-style wake path
restarted it on top of that, ending in WhateverXe's
`ElementSwitch-second-item-null` panic. The GT reset at the wake action
(54–94 us) fixes it; 3/3 short sleeps clean.

- NVRAM record: macOS 26 ignores the sync-now property key as well (0.3.3
  reported `stored, commit refused`); the record is now committed with
  `IODTNVRAM::sync()`, as Lilu does.
- `wake-report.sh`: finds the sleep start in pmset logs that use tabs,
  skips maintenance re-sleeps, lists only lines from the window; after a
  panic/forced restart the log window ends at this boot (previous boot's
  wake); panic text extraction fixed (`grep: maximum repetition`).

## 0.3.3 (unreleased)

- `macos/wake-report.sh`: one redacted report per sleep/wake (pmset log,
  panics, NVRAM records, S0ixStandby/GPU state, the log around the wake),
  also after a forced restart. TESTING.md: wake failure protocol.

- Fix: the NVRAM commit used the private `IONVRAM-FORCESYNCNOW-PROPERTY`
  key, which macOS 26 stores as a plain variable (nothing committed, junk
  variable left). Now the public `IONVRAM-SYNCNOW-PROPERTY` key; a leftover
  of either key is removed at boot, and `GPU NVRAM record` reports whether
  the record was stored and committed.

## 0.3.2 (unreleased)

- GPU probe result is also written to NVRAM 0.5 s after each wake
  (`nvram s0ix-gpu-last`), committed immediately, so it survives a graphics
  panic a few seconds later. Only with `-s0ixgpuprobe`/`-s0ixgpureset`.

## 0.3.1 (unreleased)

- GPU probe: at the wake action the iGPU is in D0 but its PCI memory decode
  is still off (`cmd 0x400`; seen on the EliteBook). If BAR0 still matches the
  sleep action, decode is enabled just for the probe/reset and the original
  command register restored; a changed BAR0 is reported as `lost power (PCI
  config space reset)`. Reports now include `cmd`, `pmcsr` and BAR0.
- `s0ix-status.sh`: fixed `grep: empty (sub)expression` in the GPU section.

## 0.3.0 (unreleased): GPU state across Deep Idle

- `-s0ixgpuprobe` (Intel Gen11/Gen12 iGPU: Ice Lake, Tiger Lake, Rocket
  Lake, Alder Lake, Raptor Lake): reads a few GPU engine registers in the
  sleep action and again in the wake action, before any driver wakes, and
  publishes whether the GPU kept its state across the sleep (`GPU state after
  sleep`). Without graphics acceleration a canary is written to a render
  engine general purpose register. Read-only otherwise.
- `-s0ixgpureset` (experimental): probe + a full GPU engine reset (GDRST) at
  wake when the GPU kept its state, so the graphics driver finds the blank GPU
  its S3-style wake path expects (as Linux does on resume). Display untouched.
- `s0ix-status.sh` shows the GPU section.
- Cleanup: removed a stray (harmless) timer release in the `-s0ixd3` setup.

## 0.2.0 (2026-11-03): first pre-release

First usable release: real sleep (macOS Deep Idle) on a Tiger Lake laptop at
**~0.7–1.0 W**, session kept, wake on power button and RTC, 26 maintenance
dark wakes overnight without a failure (HP EliteBook 840 G8, macOS 26.7.1).

- Fix: per-sleep figures are snapshotted at wake. A dark wake that goes back
  to sleep within the same second no longer mixes two cycles in the published
  properties; total residency accumulates every cycle.
- Includes everything below (0.1.0–0.1.12).

Known limitations: SLP_S0 (real S0ix) not reached yet, CNVi (Intel Wi-Fi/BT)
stays powered (driver-side); WhateverXe graphics acceleration panics on wake
(`-wex-noaccel` needed); see README.

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
