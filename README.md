# S0ixStandby

*(formerly TGLStandby, the working title)*

Real sleep for macOS on laptops whose ACPI S3 is broken or missing,
by entering **Modern Standby (S0ix)** instead of S3.

[![build](https://github.com/labrouss/S0ixStandby/actions/workflows/build.yml/badge.svg)](https://github.com/labrouss/S0ixStandby/actions/workflows/build.yml)

**Status: pre-release (0.2.0).** Generic: any laptop with an ACPI LPS0
device (`PNP0D80`). CI runs on GitHub Actions only. First test machine: HP
EliteBook 840 G8 (Tiger Lake), macOS Tahoe 26.7.1, OpenCore 1.0.8.

**What works (0.2.0):** macOS Tahoe sleeps in **Deep Idle** (an S0 sleep
built into XNU) at **~0.7–1.0 W** (Linux s2idle on the same machine: 0.76 W),
package C10 99 %, EC in standby; wakes on the power button and RTC alarms; an
11.5 h night with 26 maintenance dark wakes ran without a failure. Real S0ix is still blocked, most likely by CNVi (Intel Wi-Fi/BT), whose
macOS drivers don't power it down for sleep, and one long sleep hung on a dark wake → full wake
transition. See [docs/FINDINGS.md](docs/FINDINGS.md) §5–6.

## How it works

**New here? Read [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)**: one page with
a sleep/wake sequence diagram.

| Piece | Role |
|---|---|
| `acpi/SSDT-DEEPIDLE` | Defines `\_SB.LPS0` and `\_GPE.LXEN` (Darwin only). AppleACPIPlatform then publishes `IOPMDeepIdleSupported`, and macOS sleeps with type 7 (Deep Idle, XNU `acpi_idle_kernel()`) instead of ACPI S3 |
| `S0ixStandby.kext` | Plain IOKit (no Lilu). Matches `PNP0D80`, probes Intel / Microsoft / AMD LPS0 `_DSM` masks, calls display-off + entry before the CPUs halt (`IOPlatformSleepAction`) and exit + display-on after wake (`IOPlatformWakeAction`), Deep Idle sleeps only. Reads the SLP_S0 counter described by ACPI LPIT and publishes residency per sleep |

### Known limitations (0.2.0)

| | |
|---|---|
| Real S0ix (SLP_S0) | Not reached yet: CNVi (Intel Wi-Fi/BT) stays powered; fix belongs in the Wi-Fi/BT drivers |
| Graphics | WhateverXe acceleration panics on any wake; boot with `-wex-noaccel` (no acceleration). Under investigation: `-s0ixgpuprobe` / `-s0ixgpureset` (0.3.0), see docs/FINDINGS.md §9 |
| Dark wake → full wake | One hang seen (`0x32`), not reproduced since; avoid pressing power during a dark wake |

### Recommended configuration (tested)

`SSDT-DEEPIDLE.aml`, Release kext, Ice Lake CPUID spoof on Tiger Lake,
`hibernatemode 0`, `standby 0`, `powernap 0`, `tcpkeepalive 0`. Optional
boot-args: `-s0ixd3` (driverless PCI devices to D3hot), `-s0ixdbg` while
testing. Keep the `_S3` → soft-off patch as the fallback.

### Install (USB-stick EFI first)

1. `SSDT-DEEPIDLE.aml` → `EFI/OC/ACPI/` and `ACPI > Add`.
2. `S0ixStandby.kext` → `EFI/OC/Kexts/` and `Kernel > Add` (no dependencies;
   `MinKernel` empty).
3. Optional boot-args: `-s0ixnodsm` (meter only, no `_DSM` calls),
   `-s0ixdbg` (verbose log), `-s0ixoff` (don't load).
4. `macos/s0ix-status.sh` shows what the kext found and measured.

**Tiger Lake:** spoof the CPUID as **Ice Lake** (`Cpuid1Data` `E5060700 00000000 00000000 00000000`), not Coffee Lake. With Coffee Lake, XCPM stops at core C6 in Deep Idle; with Ice Lake it reaches C10 ([docs/FINDINGS.md](docs/FINDINGS.md) §5). Check with `sysctl machdep.xcpm.deep_idle_last_stats`.

Keep any existing S3 workaround (e.g. a `_S3` → soft-off patch): if macOS
picks S3 instead of Deep Idle, you get the old behaviour.

### Build

Done by GitHub Actions (artifacts on every push, releases on `v*` tags).
Locally on macOS with Xcode:
```bash
git clone --depth 1 https://github.com/acidanthera/MacKernelSDK
make          # build/Release/S0ixStandby.kext
make DEBUG=1  # build/Debug/S0ixStandby.kext
```

## Why

- S3 resume hangs in HP's firmware, on Linux as well as macOS. The current
  macOS workaround turns sleep into a clean power-off.
- The same laptop does **perfect Modern Standby on Linux: 0.76 W, 98 % of
  the time in S0ix, clean wake** (see [docs/FINDINGS.md](docs/FINDINGS.md)).
- macOS on Intel does have an S0 sleep path ("Deep Idle", `acpi_idle_kernel`)
  that real Macs use. It needs an ACPI opt-in, plus the LPS0 `_DSM` calls that
  PC firmware expects. The original plan (a Lilu hook replacing the final S3
  step) is now the fallback.

## Documents

| File | Contents |
|---|---|
| [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) | **Start here:** what the SSDT and the kext do, sleep/wake sequence, reports, boot-args |
| [docs/FINDINGS.md](docs/FINDINGS.md) | Everything measured so far (S3 failure, hibernation attempt, Linux S0ix results) |
| [docs/DESIGN.md](docs/DESIGN.md) | Architecture, hook point, the meter, open questions Q1–Q7 |
| [docs/PLAN.md](docs/PLAN.md) | Phases with go/no-go criteria |
| [docs/TESTING.md](docs/TESTING.md) | Linux and macOS test procedures, results log |
| [docs/RESEARCH-2A.md](docs/RESEARCH-2A.md) | Phase 2a: XNU sleep internals, Deep Idle, Q1–Q4 answered, Q8–Q11 |
| [docs/HIBERNATION-FALLBACK.md](docs/HIBERNATION-FALLBACK.md) | Parallel track: hibernate-to-disk as the everyday fallback |
| [HANDOFF.md](HANDOFF.md) | Prompt to start a new chat on this project |
| `linux/s2idle-test.sh` | One-shot s2idle power + S0ix residency measurement |
| `linux/capture-baseline.sh` | Read-only capture of the Linux "known good" state |
| `acpi/SSDT-DEEPIDLE.dsl` (+ `.aml`) | Experiment N1: advertise Deep Idle to AppleACPIPlatform (Darwin only) |
| `macos/check-deepidle.sh` | Read-only probe: Deep Idle strings in Tahoe, board-id sleep policy, ioreg keys |
| `macos/sleep-power-test.sh` | macOS counterpart of `s2idle-test.sh`: W per sleep + sleep type used |
| `macos/s0ix-status.sh` | What the kext found (DSM masks, LPIT counter) and measured (residency per sleep) |
| `S0ixStandby/` | Kext source |
| `tools/` | `privacy-scan.sh` (CI), `redact.sh` (used by the capture scripts) |

## Related

- EFI: `labrouss/EliteBook840G8` (`docs/SLEEP.md` describes today's
  sleep = power-off setup and the RTC 005 fix)
- Reference implementations: Linux `drivers/acpi/x86/s2idle.c`,
  `drivers/platform/x86/intel/pmc/`; FreeBSD's ongoing S0ix work.

## Licence

BSD-3-Clause ([LICENSE](LICENSE)). Code derived from Linux would be
GPL-2.0 and must stay out: Linux is used only as documentation of the
hardware behaviour, and the Intel/Microsoft/AMD and ACPI specs as the
source.

## Privacy

Collected data never goes into this repository.

- `.gitignore` excludes baseline archives, check outputs, panic reports,
  selftest logs, `config.plist` and `EFI/`.
- `linux/capture-baseline.sh` and `macos/check-deepidle.sh` run
  `tools/redact.sh` on their output: MAC/IP addresses, e-mails, serial
  numbers, hardware UUIDs, SMBIOS serial/MLB/ROM, user paths and prompts.
  The ACPI dump always skips MSDM (Windows product key).
- `tools/privacy-scan.sh` fails on personal identifiers or forbidden files.
  CI runs it on every push and pull request; run it locally before committing.
- Docs record measurements, not identities: no hostnames, IPs, serials or
  paths.
- Commit with your GitHub `…@users.noreply.github.com` address (GitHub →
  Settings → Emails → "Keep my email addresses private").
