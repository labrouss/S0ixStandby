# Changelog

## 0.5.4 (unreleased): wake sources; lid wake via the EC (`-s0ixecwake`)

- New read-only report of what woke each Deep Idle sleep: `Last wake source`
  lists PM1 status (PWRBTN, RTC) and the GPE0 status bits latched right after
  the halt (e.g. `GPE 0x6e (EC)`). `EC GPE at last halt` shows whether macOS
  left the embedded controller's GPE enabled; `EC wake` shows the EC GPE and
  its enable register (FADT GPE0 block + EC `_GPE`).
- `-s0ixecwake` (experimental, opt-in): on laptops where the lid is an EC event
  (`_Qxx` → `Notify(LID)`) and the lid device has no `_PRW`, macOS leaves the
  EC's GPE off during sleep and only the power button wakes. With this flag the
  kext sets the EC GPE enable bit at the Deep Idle quiesce action (if macOS left
  it off); macOS then handles the pending
  EC event (lid open) as usual. Other EC events (battery, AC, thermal) may wake
  too: watch `EC wakes`. Off by default. The EC (`PNP0C09`) is looked up in
  the ACPI plane, not only among registered services, and retried every minute
  for 10 min; `s0ixecgpe=0xNN` sets its GPE by hand. The enable bit is not
  cleared again at wake (a first build did, which could leave the EC's runtime
  GPE off for the session and hung the 840 G8 a minute after a wake).
  `EC GPE now` reports every minute whether the EC GPE is enabled while awake.

## 0.5.3 (unreleased): own log ring ("Recent log")

- On macOS 26 the kext's IOLog output (and other third-party kexts') reaches
  neither `dmesg` nor the unified log: with `-s0ixdbg` not a single
  `S0ixStandby:` line was found, only the kernel's own
  `IOPlatformSleepAction -> S0ixStandby`. The kext now also keeps its last
  40 log lines (seconds since boot + text) in a ring buffer and publishes them
  as the `Recent log` property: after each wake, at start, and every minute.
  The sleep/wake actions only write into the buffer (no allocation, no
  registry access). IOLog is still called as before.
- `s0ix-status.sh`: new `== S0ixStandby log` section from `Recent log`; works
  without `sudo` and on any macOS. Like all properties, it's lost on a forced
  power-off.

## 0.5.2 (unreleased): battery "not full" on AC detected; SSDT-BATFULL

- **Cause of the AC freeze found:** on AC at 100 % the EC stops charging a few
  mAh short of full (HP 840 G8: 3620/3622). SMCBatteryManager then reports
  `IsCharging No`, `FullyCharged No` and no reason, and powerd
  (`GaugingMitigationActions`) and Optimized Charging (`com.apple.obc`)
  dark-wake the machine every ~5 min. Those cycles eventually hang the TGL
  framebuffer (`GFX0()` setPowerState) at sleep entry. The underlying hang
  is the framebuffer's re-sleep after an AC dark wake (WhateverXe).
  FINDINGS §12.1.
- New `acpi/SSDT-BATFULL.dsl` (optional, per machine): under macOS, when
  `_BST` says neither charging nor discharging and remaining is within 3 % of
  last full, it returns remaining = last full, so `FullyCharged = Yes`. Needs
  an OpenCore `_BST` → `XBST` rename; `tools/find-bst.py DSDT.aml` prints it.
  `docs/BATTERY-FULL.md` explains how to adapt it. On the 840 G8, AC sleeps
  at 100 % went from all freezing to 4/4 clean together with
  `-s0ixacnortc`, with no gauging wakes.
- Kext: new read-only property `Battery full state` (`ok: …`, or `stuck: on
  AC, not charging, cur/max, FullyCharged No …`), checked every minute from
  `IOPMPowerSource`; logged when it becomes or stops being `stuck`.
- `s0ix-status.sh`: `== Battery` section with a hint to BATTERY-FULL.md;
  `Since boot: N sleeps, N dark wakes, N h asleep` from `pmset -g log` (the
  kext only reports the last Deep Idle stretch); the kernel-log section reads
  `dmesg` first (instant; `log show --last boot` took ~1 min) and falls back to
  the last 30 min of the unified log (it was empty before).
- Display probe: new `Display power 15 s after wake` (DC_STATE_EN, power
  wells, DC counters read again once the framebuffer should have restored the
  display; `Display power now` is taken 0.5 s after the wake action). Tells a
  display engine that was never re-enabled (power wells still 0, DC9 still
  set) apart from one that is up but not scanning out.
- `-s0ixacnortc` is **recommended on AC** on the 840 G8: with SSDT-BATFULL
  alone, the re-sleep after an ordinary AC dark wake (45 s long; 0 s on
  battery) still hung GFX0. With both, AC sleeps at 100 % were clean. It
  also blocks timed wakes (`pmset relative wake`, `pmset schedule`) on AC.

## 0.5.1 (unreleased): no RTC dark wakes on AC (experiment)

- `-s0ixacnortc` (experimental, off by default): while on AC, a Deep Idle
  sleep can't be woken by the RTC alarm, so macOS's every-5-minute
  maintenance dark wakes on AC (battery management) don't happen; the power
  button, lid and other wake sources are unaffected. At the Deep Idle sleep
  action the RTC alarm interrupt enable (CMOS register B, AIE) and the ACPI
  fixed event `RTC_EN` (PM1_EN bit 10; PM1a event block from the FADT) are
  cleared, `RTC_EN` once more at the quiesce action, and both are restored
  first thing at the wake action (a stale alarm flag is dropped). AC state
  comes from `IOPMPowerSource` `ExternalConnected`, cached every 10 s.
  Battery sleep is unchanged. Works around the AC freeze in FINDINGS §12.
  Reported: `AC RTC wake block`, `AC RTC wake blocks`, `On AC (last check)`.
- Fixed: three stray releases in `start()` (of `liveTimer`, `wifiTimer` and
  `wifiDev`) that belonged in `free()`; `wifiTimer` could be released while
  still on the work loop when the iGPU was not yet published at start.
  `free()` now releases `wifiTimer` and `wifiDev`.

## 0.5.0 (2026-11-09)

Consolidates 0.4.3–0.4.4 and adds platform guards from the first outside
test reports. Nothing changes on Tiger Lake with the default boot-args.

- **AMD guard:** on an AMD (or Hygon) CPU the kext publishes `Deep Idle =
  unsupported on AMD …`, logs a warning and registers no sleep/wake actions
  and no `_DSM` calls. XNU's Deep Idle halt (`pmCPUHalt(PM_HALT_SLEEP)`)
  needs a CPU power-management halt handler that only Intel has (XCPM /
  AppleIntelCPUPowerManagement); without it the CPU is stopped with
  interrupts off and cannot wake. Reported on a Ryzen 5 5500U, with and
  without `-s0ixnodsm`.
- **iGPU reset default on Tiger Lake only** (CPU model 0x8C/0x8D, where it
  was developed with WhateverXe). Other Gen11/12 CPUs (Ice Lake, Alder/Raptor
  Lake, e.g. with NootedGreen) need `-s0ixgpureset` to enable it.
- `macos/lpm-status.sh` refuses to run on CPUs other than Tiger Lake-LP
  (its offsets and bit names are TGL-specific).
- README: platform support table, how to find a driver that blocks sleep
  entry (`io=0x800`: XNU panics on a `setPowerState` timeout and names the
  driver), where `-s0ixdbg` output goes, known limitations updated (SLP_S0
  investigation, sleep-entry stall on AC).

## 0.4.4 (unreleased): PCH display refclk workaround (experiment)

- `-s0ixrefclk` (experimental, off by default): at a Deep Idle sleep
  action, if Apple's framebuffer has already powered the display core down
  (`DC_STATE_EN` DC9, power wells 1/2 off), sets `SOUTH_CHICKEN1` bit 7
  (`SBCLK_RUN_REFCLK_DIS`), the "Tweaked Wa_14010685332" i915 applies after
  DC9 on suspend (cnp/icp/jsp/mcc/tgp/adp PCH); cleared at the wake action
  before any driver wakes. Linux reaches S0ix only with i915 loaded, and the
  0.4.3 probe showed this bit as the one i915 suspend step macOS skips
  (FINDINGS §11). Reported: `Display refclk workaround`.

## 0.4.3 (unreleased): display power probe

- Read-only display power report for Intel Gen11/12 iGPUs, whenever the GPU
  is mapped (default): `Display DMC firmware` (whether the display
  microcontroller firmware is loaded: first program word, SSP base, HTP),
  `Display power now` (60 s after setup and after each wake),
  `Display power at sleep` / `at wake` (`DC_STATE_EN`, DC3→DC5, DC5→DC6 and
  DC3CO counters, power well control, `SOUTH_CHICKEN1`). Same registers as Linux's
  `i915_dmc_info`. Linux bisection showed that without i915 the SLP_S0
  counter stays at 0, as on macOS (FINDINGS §11). Nothing is written.
- `macos/s0ix-status.sh`: new Display section.

## 0.4.2 (unreleased)

- `-s0ixpmcdump`: a third PMC page snapshot right after the Deep Idle halt
  (`PMC page after halt`, taken in `IOPlatformActiveAction`). The PMC's
  latched LPM status (latched at package C10 entry) is only meaningful
  there.
- `macos/lpm-status.sh` rewritten: latched registers (`PWRM+0x1C3C`) after
  the halt vs Linux after s2idle, live registers (`PWRM+0x1C5C`) awake and
  before the halt vs Linux awake. Register locations confirmed with Linux
  `/dev/mem` dumps.

## 0.4.1 (unreleased): Wi-Fi to D3 for sleep (experiment)

- `-s0ixwifipm` (experimental): 30 s after boot, calls
  `IOPCIDevice::enablePCIPowerManagement(D3)` for the Wi-Fi PCI function
  (class 0x0280, with a driver), the call Apple's own drivers make.
  IOPCIFamily then puts the function into D3 during sleep, runs its ACPI
  `_PS3`, and restores its config space before the driver wakes, which
  AirportItlwm does not ask for (it leaves CNVi Wi-Fi in D0). Unlike the
  removed `-s0ixd3net` (0.1.11), nothing writes PMCSR behind IOPCIFamily.
  Reported: `Wi-Fi PCI PM` (result of the call) and `Wi-Fi PCI power state`
  (PMCSR at the sleep and wake actions). Tested: Wi-Fi in D3 for the whole
  sleep, back by itself after wake; CNVI unchanged.
- `macos/lpm-status.sh`: decodes the PMC low-power-mode requirement status
  registers (Tiger Lake-LP: `PWRM+0x1C5C`) from a `-s0ixpmcdump` snapshot at
  the halt and compares them bit by bit, by name, with Linux at S0ix entry.
- `wake-report.sh`: a start time without seconds works (`log show` returned
  nothing).

## 0.4.0 (2026-11-05): accelerated graphics across sleep

- The GPU reset at a Deep Idle wake is now **the default** on Intel
  Gen11/Gen12 iGPUs (Ice Lake, Tiger Lake, Rocket Lake, Alder Lake, Raptor
  Lake). It acts only when the GPU kept its state (an accelerator such as
  WhateverXe was running), and only after Deep Idle (S3 wakes are left
  alone). `-s0ixnogpureset` turns it off; `-s0ixgpureset` is still accepted.
- Without an active accelerator at sleep (e.g. `-wex-noaccel`) the GPU is not
  touched at wake at all.
- `-s0ixgpuprobe` is now a debugging option only: it also probes an idle GPU
  (canary) and keeps the NVRAM record. No NVRAM writes by default.
- Verified with WhateverXe 0.7.0: 4/4 accelerated wakes (1–40 min), then a
  4 h 8 min idle sleep on battery with 5 RTC/maintenance dark wakes: 6/6 GPU
  resets, all clean, no panic, ~0.5 W (36 % → 31 %).
- Includes 0.3.0–0.3.4 below: the GPU probe and reset, `macos/wake-report.sh`
  (one redacted report per sleep/wake) and the wake failure protocol in
  docs/TESTING.md.

Known limitations: SLP_S0 (real S0ix) not reached yet: CNVi (Intel
Wi-Fi/BT) stays powered, sleep draws ~1 W. One unexplained freeze after a
30 min sleep (without acceleration), not reproduced in 3 identical runs.

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
