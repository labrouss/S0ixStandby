# S0ixStandby

*(formerly TGLStandby, the working title)*

Real sleep for macOS on laptops whose ACPI S3 is broken or missing: macOS
sleeps in **Deep Idle** (the S0 sleep built into XNU) with the Modern Standby
(S0ix) calls PC firmware expects, instead of S3.

[![build](https://github.com/labrouss/S0ixStandby/actions/workflows/build.yml/badge.svg)](https://github.com/labrouss/S0ixStandby/actions/workflows/build.yml)

**Status: pre-release 0.5.13.** Generic: any Intel laptop with an ACPI LPS0
device (`PNP0D80`); **not AMD** (see [Platform support](#platform-support)). Built and released by GitHub Actions only. First test machine:
HP EliteBook 840 G8 (Tiger Lake), macOS Tahoe 26.7.1, OpenCore 1.0.8.
**Testers with other laptops wanted**, see [Testing](#testing-wanted).

## What works

| | Result on the test machine |
|---|---|
| Sleep | Deep Idle (`IOPMSystemSleepType` 7) from the Apple menu, the power button or idle sleep; session kept (lid: not yet tested) |
| Power | **~0.5–1.0 W** asleep (Linux s2idle on the same machine: 0.76 W); package C10 99 %; EC in standby (breathing power LED) |
| Wake | Power button, RTC alarms; maintenance dark wakes go back to sleep (11.5 h night, 26 dark wakes, clean; 4 h with acceleration, 5 dark wakes, clean) |
| **Graphics acceleration** *(0.4.0)* | **WhateverXe acceleration survives sleep** (Tiger Lake Iris Xe, Metal after wake). The iGPU keeps its state in Deep Idle while Apple's driver restarts it as if it had been powered off; S0ixStandby resets the GPU at wake first. No boot-arg needed |
| Reports | Per sleep: SLP_S0 residency, C-states, the PCH blocks still powered (likely SLP_S0 blockers), GPU state |

## Known limitations

| | |
|---|---|
| Real S0ix (SLP_S0) | Not asserted by default, although package C10 is reached. Likely cause found on Alder Lake-P: AppleHPET leaves the stopped HPET's comparators equal to its counter, which stalls the PMC before S0i2.0 ([FINDINGS §14](docs/FINDINGS.md)). Try `-s0ixhpetfix`; not yet confirmed on Tiger Lake. Power is already in Linux's S0ix range, so the gain left is a few tenths of a watt |
| **Freeze at sleep (TGL framebuffer)** (`GFX0(): Some drivers failed to handle setPowerState`) | Apple's TGL framebuffer (WhateverXe) can hang on the **re-sleep after a long dark wake** (LED steady), also with S0ixStandby disabled ([FINDINGS §12](docs/FINDINGS.md)). Long means ~45 s: every dark wake on AC, and lid/EC dark wakes on battery. RTC dark wakes on battery last 0 s and have never hung. At 100 % on AC it gets much worse: the EC stops charging just short of full and macOS dark-wakes every ~4 min. **On AC use both** `-s0ixacnortc` and, if `s0ix-status.sh` shows `Battery full state = stuck`, [SSDT-BATFULL](docs/BATTERY-FULL.md): the hang became rare (about 1 in 13 AC sleeps), not gone. Not fixed by WhateverXe 0.8.3's `-wex-s0-csb-resync` on TGL. Reproducer (~20 min): lid closed on AC while charging with SSDT-LIDWAKE. Reported to WhateverXe |
| Silent deadlock at sleep (WhateverXe, TGL-H35 report) | Fixed on that machine by a local WhateverXe patch that loads Intel's DMC image (`tgl_dmc_ver2_12`) and re-uploads it at wake ([#3](https://github.com/labrouss/S0ixStandby/issues/3), [WhateverXe #157](https://github.com/b00t0x/WhateverXe/issues/157)). `s0ix-status.sh` shows which DMC image is loaded. Not known to be related to the `GFX0()` hang above |
| External display across Deep Idle (WhateverXe, TGL) | With an HDMI display connected the panel stays dark after wake; the full modeset that would light it hangs the GPU (accelerator side, [WhateverXe #157](https://github.com/b00t0x/WhateverXe/issues/157), [#126](https://github.com/b00t0x/WhateverXe/issues/126)). **Unplug external displays before sleep**; plugging in after the wake works |
| GPU hangs with hardware video (WhateverXe, TGL) | YouTube/Photo Booth decode hangs the GPU every ~10 s (`GPURestartSignaled`, CCS ring). Removing `AppleIntelTGLGraphicsVADriver.bundle` stops it (software decode, Metal stays). Not related to S0ixStandby |
| Lid wake (HP 840 G8) | Only the power button wakes: the lid is an EC event, the lid device has no `_PRW`, and macOS arms only wake GPEs that come from a `_PRW` (#7). [`acpi/SSDT-LIDWAKE`](acpi/SSDT-LIDWAKE.dsl) adds one and the lid then wakes the machine, but **don't use it on the 840 G8**: some lid/EC wakes become 45 s dark wakes (also on battery), and the re-sleep after one hangs `GFX0()` (on AC after 31 s with the lid closed; on battery overnight, when the lid was opened). `-s0ixecwake` alone doesn't help: its bit is cleared before the halt |
| Rare dark display after wake | Seen twice, not reproducible with the same settings; please report with `macos/wake-report.sh` (use the issue form) |
| Wi-Fi after wake | AirportItlwm reconnects after every wake (it stops the firmware for sleep) |

## How it works

**New here? Read [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)**: one page with
a sleep/wake sequence diagram.

| Piece | Role |
|---|---|
| `acpi/SSDT-DEEPIDLE` | Defines `\_SB.LPS0` and `\_GPE.LXEN` (Darwin only). AppleACPIPlatform then publishes `IOPMDeepIdleSupported`, and macOS sleeps with type 7 (Deep Idle, XNU `acpi_idle_kernel()`) instead of ACPI S3 |
| `acpi/SSDT-LIDWAKE` | Experimental, per machine: gives the lid device a `_PRW` on the EC's GPE so opening the lid wakes from Deep Idle. **Not for the HP 840 G8** (lid/EC dark wakes lead to the `GFX0()` hang, on AC and battery); for machines whose graphics driver survives dark-wake re-sleeps |
| `acpi/SSDT-BATFULL` | Optional, per machine: reports the battery as full when the EC stops charging just short of full on AC, which otherwise makes macOS dark-wake every ~5 min ([BATTERY-FULL](docs/BATTERY-FULL.md)). Needs a `_BST` → `XBST` rename (`tools/find-bst.py`) |
| `S0ixStandby.kext` | Plain IOKit (no Lilu). Matches `PNP0D80`; calls the LPS0 `_DSM` (Intel / Microsoft / AMD) display-off + entry before the CPUs halt and exit + display-on after wake; resets an Intel Gen11/12 iGPU at wake if it kept its state; measures every sleep (SLP_S0 counter from ACPI LPIT, C-state MSRs, PCH power gating) |

## Install (USB-stick EFI first)

1. `SSDT-DEEPIDLE.aml` → `EFI/OC/ACPI/` and `ACPI > Add`.
2. `S0ixStandby.kext` (Release, from the
   [releases](https://github.com/labrouss/S0ixStandby/releases)) →
   `EFI/OC/Kexts/` and `Kernel > Add` (no dependencies, `MinKernel` empty).
3. `sudo pmset -a hibernatemode 0 standby 0 powernap 0 tcpkeepalive 0`.
4. Reboot, then check: `ioreg -rd1 -c IOPMrootDomain | grep DeepIdle` must
   show `IOPMDeepIdleSupported = Yes`; `macos/s0ix-status.sh` shows what the
   kext found and measured.
5. If `s0ix-status.sh` shows `Battery full state = stuck …` on AC, add
   SSDT-BATFULL ([BATTERY-FULL](docs/BATTERY-FULL.md)).

Keep any existing S3 workaround (e.g. a `_S3` → soft-off patch): if macOS
picks S3 instead of Deep Idle, you get the old behaviour.

**Tiger Lake:** spoof the CPUID as **Ice Lake** (`Cpuid1Data`
`E5060700 00000000 00000000 00000000`), not Coffee Lake. With Coffee Lake,
XCPM stops at core C6 in Deep Idle; with Ice Lake it reaches C10
([docs/FINDINGS.md](docs/FINDINGS.md) §5). Check with
`sysctl machdep.xcpm.deep_idle_last_stats`.

**WhateverXe users:** acceleration works across sleep from 0.4.0 on; remove
`-wex-noaccel`.

### Platform support

| Platform | Status |
|---|---|
| Intel Tiger Lake (tested), other Intel with LPS0 | supported; the wake-time iGPU reset is on by default on Tiger Lake only. Reports: i7-1185G7 (HP EliteBook 840 G8), i5-11300H / TGL-H35 (MECHREVO Forza 6, internal display, [#3](https://github.com/labrouss/S0ixStandby/issues/3)) |
| Intel Kaby Lake R, Apple graphics (report) | Lenovo Yoga C930-13IKB, i7-8550U, UHD 620 ([#6](https://github.com/labrouss/S0ixStandby/issues/6)), macOS 15.8.3: Deep Idle sleep and wake by power button, keyboard and lid work with no extra flags; hourly dark wakes clean. Battery drain is high (~4.4 %/h) because the package stays in **PC3** for the whole sleep (`Last C-state percent` PC3 = 100 %); the blocker isn't known yet. Lid and keyboard wake arrive as power-button events. The GT reset is off (Gen11/12 only) |
| Intel Alder Lake-P with WhateverXe (report) | Lenovo ThinkBook 14+ 2022, i5-12500H, 8086:46A6 ([#4](https://github.com/labrouss/S0ixStandby/issues/4)): Deep Idle sleep and wake with Metal work using WhateverXe 0.8.3's `-wex-s0-csb-resync` (WhateverXe #117) and **without** `-s0ixgpureset`. Lid wake needed a `_PRW` for `LID0` on the EC GPE (see SSDT-LIDWAKE) |
| Alder / Raptor Lake package stuck at PC2/PC3 | One known cause: the Intel `_DSM` fn 5 sets the Type-C IOM's DPOF bit, which on some ADL/RPL machines holds the package at PC2/PC3 until a reboot ([FINDINGS §13](docs/FINDINGS.md), #7). Try `s0ixfn=0x198` from a fresh boot, or rename the TCSS SSDT's `DPOF` field. Fixed it on a ThinkPad X1 Yoga Gen 7 (ADL-P), **not** on a ThinkPad E16 Gen 3 (RPL-H), where the PMC counters point to a PCH-side device active during the halt (FINDINGS §13.1). To see which side blocks on your machine, use the PMC telemetry diff in [TESTING.md](docs/TESTING.md#finding-what-blocks-pc10-pmc-telemetry-diff). Tiger Lake isn't affected |
| Intel Ice Lake / Alder Lake / Raptor Lake iGPU | sleep path generic; the iGPU reset stays off unless `-s0ixgpureset` (untested there, e.g. with NootedGreen) |
| **AMD** | **not supported.** XNU's Deep Idle halt (`pmCPUHalt(PM_HALT_SLEEP)`, xnu `osfmk/i386/pmCPU.c`) needs a CPU power-management halt handler (Intel XCPM / AppleIntelCPUPowerManagement); without one it stops the CPU with interrupts off and it never wakes, with or without the `_DSM` calls. Don't install SSDT-DEEPIDLE on AMD. From 0.5.0 the kext detects AMD and stays out of the sleep path |

### Boot-args (all optional)

| Boot-arg | Effect |
|---|---|
| `-s0ixd3` | keep PCI devices that have no macOS driver in D3hot (recommended) |
| `-s0ixdbg` | verbose log |
| `-s0ixnogpureset` | never reset the iGPU at wake |
| `-s0ixgpureset` | reset the iGPU at Deep Idle wake on Gen11/12 CPUs other than Tiger Lake too (off there by default, untested) |
| `-s0ixwifipm` | experimental: Wi-Fi PCI function to D3 for sleep through IOPCIFamily (CNVi blocker work) |
| `-s0ixhpetfix` | experimental: clear HPET comparators that equal the stopped HPET counter. AppleHPET leaves them all at 0, and that standing match keeps the PMC from entering S0i2.0, so **SLP_S0 stays 0 at PC10 99 %**. With it, the X1 Yoga Gen 7 (ADL-P) counted real SLP_S0 for 99 %+ of its sleeps ([#11](https://github.com/labrouss/S0ixStandby/pull/11), brandonedley; FINDINGS §14). Without the boot-arg the HPET is only read: `HPET comparator fix` in `s0ix-status.sh` shows whether the match is there |
| `-s0ixhdapm` | experimental: during Deep Idle, set the Intel HDA (00:1f.3) clock-gating bits in PCI `CGCTL` (0x48), which AppleHDAController leaves at 0, and restore them at wake. Saved ~0.4–0.6 W of sleep power on the X1 Yoga Gen 7 with audio working ([#10](https://github.com/labrouss/S0ixStandby/pull/10), brandonedley). `s0ixhdacg=0xNNNNNNNN` sets the mask (default `0x003F01FF`) |
| `-s0ixecwake` | experimental: keep the embedded controller's GPE armed during Deep Idle, so EC events can wake the machine: on many laptops (HP 840 G8 included) **opening the lid** is an EC event (`_Qxx` → `Notify(LID)`), and the lid device has no `_PRW`, so macOS leaves that GPE off and only the power button wakes. Other EC events (battery, AC, thermal) may then cause extra dark wakes: check `EC wakes` and `Last wake source` in `s0ix-status.sh`. `s0ixecgpe=0xNN` sets the EC's GPE number if it isn't found (HP 840 G8: `0x6e`). **HP 840 G8: no lid wake with it** (the lid's EC event is latched during standby but only delivered once the power button wakes the machine); kept for other laptops |
| `s0ixtrace=0xNN` | diagnostic: write a stage code to battery-backed CMOS byte `0xNN` at each sleep/wake step, so after a hang and forced power-off the next boot reports the last step reached (`Sleep trace` in `s0ix-status.sh`). Extended CMOS bank (`0x80`–`0xFF`) only, and only a byte that reads `0x00` (`CMOS bytes reading 0x00` lists them); writing a standard-bank byte caused a BIOS checksum error on the 840 G8. `s0ixtraceset=0xVV` restores a byte. [SLEEP-TRACE](docs/SLEEP-TRACE.md) |
| `-s0ixacnortc` | experimental: **while on AC**, Deep Idle can't be woken by the RTC alarm (no maintenance dark wakes; **recommended on AC** on the 840 G8: avoids the dark-wake re-sleeps that can hang the TGL framebuffer, FINDINGS §12). Power button and lid still wake; timed wakes (`pmset schedule`, `relative wake`) don't fire while plugged in |
| `-s0ixrefclk` | experimental: at a Deep Idle sleep, with the display core already down (DC9), set the PCH display reference clock workaround i915 applies on suspend (`SOUTH_CHICKEN1` bit 7); cleared at wake |
| `-s0ixgpuprobe` | debugging: also probe an idle iGPU and keep the last result in NVRAM (`nvram s0ix-gpu-last`) |
| `-s0ixnodsm` | measure only, never call `_DSM` |
| `s0ixfn=<mask>` | only call the listed `_DSM` functions (debugging) |
| `-s0ixpmcdump` | snapshot the PMC register page at boot and at each halt |
| `-s0ixnocst` | don't read CPU C-state MSRs |
| `-s0ixoff` | don't load |

## Testing wanted

Most useful: **other laptops** with an Intel CPU whose S3 sleep doesn't work
on macOS, and long sleeps. (AMD cannot work, see [Platform support](#platform-support).)

1. Install as above, sleep for 30+ minutes, wake.
2. `sudo bash macos/wake-report.sh`: one report per sleep/wake (panics,
   power log, NVRAM, what the kext measured); identifiers are redacted.
   `sudo bash macos/sleep-power-test.sh` measures watts on battery.
3. Open an issue with the report, the laptop model, CPU and macOS version.
   [docs/TESTING.md](docs/TESTING.md) has the checklist (what to note before
   sleep and on wake).

**Sleep hangs before Deep Idle** (screen off, fan on, `Failure during sleep:
… Some drivers failed to handle setPowerState` after the reboot): that is a
driver failing to power down, before S0ixStandby's part runs; it usually
happens with a fake `_S3` too. To find it, boot once with `io=0x800 keepsyms=1`:
XNU then panics when a driver's `setPowerState` times out, and the panic
report (which survives the forced reboot) names the driver. No panic at all
means a CPU froze outright (often an NVMe/PCIe hang). Also check
`PKG_CST_CONFIG_CONTROL` in `macos/s0ix-status.sh`: a limit below C10 (e.g.
`C0/C1`, set by the firmware's C-state option) prevents any real standby.

**Logs:** the kext keeps its last 40 lines in the `Recent log` property
(`s0ix-status.sh` shows them; `-s0ixdbg` adds the per-sleep detail). It also
calls IOLog, but on macOS 26 that output didn't reach `dmesg` or the unified log.
After a forced power-off the newest entries are usually lost (the log is
written in batches); the kext's ioreg properties reset with the reboot.

## Build

Done by GitHub Actions (artifacts on every push, releases on `v*` tags).
Locally on macOS with Xcode:
```bash
git clone --depth 1 https://github.com/acidanthera/MacKernelSDK
make          # build/Release/S0ixStandby.kext
make DEBUG=1  # build/Debug/S0ixStandby.kext
```

## Why

- S3 resume hangs in HP's firmware, on Linux as well as macOS. The usual
  macOS workaround turns sleep into a clean power-off.
- The same laptop does **perfect Modern Standby on Linux: 0.76 W, 98 % of
  the time in S0ix, clean wake** (see [docs/FINDINGS.md](docs/FINDINGS.md)).
- macOS on Intel does have an S0 sleep path ("Deep Idle", `acpi_idle_kernel`).
  It needs an ACPI opt-in, plus the LPS0 `_DSM` calls PC firmware expects.

## Documents

| File | Contents |
|---|---|
| [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) | **Start here:** what the SSDT and the kext do, sleep/wake sequence, reports, boot-args |
| [docs/FINDINGS.md](docs/FINDINGS.md) | Everything measured so far (S3 failure, Linux S0ix, Deep Idle, blockers, the WhateverXe wake fix §9) |
| [docs/TESTING.md](docs/TESTING.md) | Test procedures, wake failure protocol, results log |
| [docs/DESIGN.md](docs/DESIGN.md) | Architecture, hook point, the meter, open questions |
| [docs/PLAN.md](docs/PLAN.md) | Phases with go/no-go criteria |
| [docs/RESEARCH-2A.md](docs/RESEARCH-2A.md) | XNU sleep internals, Deep Idle |
| [docs/HIBERNATION-FALLBACK.md](docs/HIBERNATION-FALLBACK.md) | Parallel track: hibernate-to-disk as a fallback |
| [docs/UPSTREAM-WHATEVERXE.md](docs/UPSTREAM-WHATEVERXE.md) | Report for WhateverXe: why acceleration failed after Deep Idle |
| [CHANGELOG.md](CHANGELOG.md) | Versions |
| `macos/` | `s0ix-status.sh`, `wake-report.sh`, `lpm-status.sh`, `sleep-power-test.sh`, `check-deepidle.sh`, `pmc-dump.sh`, `pmc-sleep-diff.sh` (PMC counters across one sleep) |
| `linux/` | `s2idle-test.sh`, `capture-baseline.sh` (reference measurements under Linux) |
| `S0ixStandby/` | Kext source |
| `tools/` | `privacy-scan.sh` (CI), `redact.sh` (used by the capture scripts), `find-bst.py` (OpenCore `_BST` rename for SSDT-BATFULL), `pmtel.c` (IOReport counter reader, used by `pmc-sleep-diff.sh`) |

## Related

- [WhateverXe](https://github.com/b00t0x/WhateverXe): Intel Xe graphics for
  macOS (acceleration across sleep needs S0ixStandby 0.4.0+ for now).
- Reference implementations: Linux `drivers/acpi/x86/s2idle.c`,
  `drivers/platform/x86/intel/pmc/`, `drivers/gpu/drm/i915/gt/intel_reset.c`.

## Licence

BSD-3-Clause ([LICENSE](LICENSE)). Code derived from Linux would be
GPL-2.0 and must stay out: Linux is used only as documentation of the
hardware behaviour, and the Intel/Microsoft/AMD and ACPI specs as the
source.

## Privacy

Collected data never goes into this repository.

- `.gitignore` excludes baseline archives, reports, panic files, selftest
  logs, `config.plist` and `EFI/`.
- The capture and report scripts run `tools/redact.sh` on their output:
  MAC/IP addresses, e-mails, serial numbers, hardware UUIDs, SMBIOS
  serial/MLB/ROM, user paths and prompts. The ACPI dump always skips MSDM
  (Windows product key).
- `tools/privacy-scan.sh` fails on personal identifiers or forbidden files.
  CI runs it on every push and pull request; run it locally before committing.
- Docs record measurements, not identities: no hostnames, IPs, serials or
  paths.
- Commit with your GitHub `…@users.noreply.github.com` address (GitHub →
  Settings → Emails → "Keep my email addresses private").
