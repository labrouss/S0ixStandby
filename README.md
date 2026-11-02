# S0ixStandby

*(formerly TGLStandby, the working title)*

Real sleep for macOS on laptops whose ACPI S3 is broken or missing,
by entering **Modern Standby (S0ix)** instead of S3.

**Status: research / design.** No kext yet. First target machine: HP
EliteBook 840 G8 (BIOS T37 01.19.00), macOS Tahoe 26.7.1, OpenCore 1.0.8.
The kext is meant to be **generic** (any laptop with an LPS0 device). **CI
runs on GitHub Actions only.**

**2026-11-02:** XNU already has an Intel S0 sleep path, "Deep Idle", which
AppleACPIPlatform enables via `\_SB.LPS0`/`\_GPE.LXEN`. The plan is now:
SSDT first (`acpi/SSDT-DEEPIDLE.dsl`), then a small generic kext that adds
the LPS0 `_DSM` calls Apple doesn't make. See
[docs/RESEARCH-2A.md](docs/RESEARCH-2A.md).

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

## Related

- EFI: `labrouss/EliteBook840G8` (`docs/SLEEP.md` describes today's
  sleep = power-off setup and the RTC 005 fix)
- Reference implementations: Linux `drivers/acpi/x86/s2idle.c`,
  `drivers/platform/x86/intel/pmc/`; FreeBSD's ongoing S0ix work.

## Licence

To be decided (BSD-3 suggested, matching TGLDmic). Code derived from Linux
would be GPL-2.0 and must stay out of a BSD kext: use Linux only as
documentation of the hardware behaviour, and Intel/ACPI specs as the source.

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
