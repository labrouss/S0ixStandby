# S0ixStandby

*(formerly TGLStandby, the working title)*

Real sleep for macOS on laptops whose ACPI S3 is broken or missing: macOS
sleeps in **Deep Idle** (the S0 sleep built into XNU) with the Modern Standby
(S0ix) calls PC firmware expects, instead of S3.

[![build](https://github.com/labrouss/S0ixStandby/actions/workflows/build.yml/badge.svg)](https://github.com/labrouss/S0ixStandby/actions/workflows/build.yml)

**Status: pre-release 0.5.0.** Generic: any Intel laptop with an ACPI LPS0
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
| Real S0ix (SLP_S0) | Not asserted, although package C10 is reached and every PMC requirement Linux needs is met at the halt (display, CNVi, ISH, Thunderbolt all ruled out, [FINDINGS §11](docs/FINDINGS.md)). Power is already in Linux's S0ix range, so the gain left is a few tenths of a watt |
| Sleep-entry stall (`GFX0(): Some drivers failed to handle setPowerState`) | Rare per sleep, but on **AC power** macOS's battery-charging management wakes the machine every ~5 min all night, and one of those cycles eventually hangs (LED solid). Inside Apple's TGL graphics driver / WhateverXe; reported upstream. Sleep on battery is much less exposed |
| Rare dark display after wake | Seen twice, not reproducible with the same settings; please report with `macos/wake-report.sh` |
| Wi-Fi after wake | AirportItlwm reconnects after every wake (it stops the firmware for sleep) |

## How it works

**New here? Read [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)**: one page with
a sleep/wake sequence diagram.

| Piece | Role |
|---|---|
| `acpi/SSDT-DEEPIDLE` | Defines `\_SB.LPS0` and `\_GPE.LXEN` (Darwin only). AppleACPIPlatform then publishes `IOPMDeepIdleSupported`, and macOS sleeps with type 7 (Deep Idle, XNU `acpi_idle_kernel()`) instead of ACPI S3 |
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
| Intel Tiger Lake (tested), other Intel with LPS0 | supported; the wake-time iGPU reset is on by default on Tiger Lake only |
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

**Logs:** `-s0ixdbg` output goes to the unified log
(`log show --last 1h --predicate 'eventMessage CONTAINS "S0ixStandby"'`).
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
| `macos/` | `s0ix-status.sh`, `wake-report.sh`, `lpm-status.sh`, `sleep-power-test.sh`, `check-deepidle.sh`, `pmc-dump.sh` |
| `linux/` | `s2idle-test.sh`, `capture-baseline.sh` (reference measurements under Linux) |
| `S0ixStandby/` | Kext source |
| `tools/` | `privacy-scan.sh` (CI), `redact.sh` (used by the capture scripts) |

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
