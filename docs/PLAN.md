# Plan

Each phase ends with a go/no-go decision. Phases 0–1 are read-only and cheap;
they decide whether phase 2 is worth the effort.

**Status 2026-11-05:** N0–N2 done (0.2.0). WhateverXe acceleration across sleep done (0.4.0, GPU reset at wake by default, FINDINGS §9). Next: overnight run with acceleration, testers on other laptops, then CNVi (FINDINGS §7–8, ~0.2 W).

**Order since 2026-11-02:** L → N0 → N1 → 0 (meter) → 1 → N2 → 3 → 4. Route B
(2b) only if Deep Idle turns out to be unavailable on Tahoe.

| Phase | Deliverable | Exit criterion | Risk |
|---|---|---|---|
| **L. Linux baseline** | `capture-baseline.sh` archive + `s0ix-selftest-tool.sh -s` and `-r off` logs + a 30–60 min `s2idle-test.sh` run | Reference data for every later comparison | None |
| **N0. Deep Idle probe** ✅ *(new)* | `macos/check-deepidle.sh` output | Q8: Tahoe still has the Deep Idle strings; Q9: board-id policy known | None (read-only) |
| **N1. Deep Idle SSDT** ✅ 2.01 W, clean wake (with `-wex-noaccel`) *(new)* | `SSDT-DEEPIDLE.aml` on the USB EFI + `macos/sleep-power-test.sh` | `IOPMDeepIdleSupported = Yes`, `IOPMSystemSleepType = 7`, one sleep → wake with apps intact | Low: worst case is a hard power-off; the `_S3` patch stays as the fallback |
| **N2. LPS0 `_DSM` glue** ✅ 0.2.0: EC standby, PC10 99 %, ~0.7–1.0 W, overnight clean *(new)* | SSDT or small kext calling `PEPD._DSM` 3/5 on entry, 6/4 on exit (Q10) | Watts drop toward the Linux 0.76 W; SLP_S0 residency > 0 (needs the meter) | Low–medium |
| **0. Meter** | Read-only kext + `s0ixstandby-meter` CLI: package C10 and SLP_S0 residency, PCH IP gating, LTR | Counters on macOS match Linux semantics (sanity check: C10 rises at idle with the screen off) | None |
| **1. Idle survey** | Report: with the display off and the system idle on macOS, which IPs stay powered | List of blocking drivers; rough idle power vs Linux | None |
| **2a. Hook research** | ✅ XNU part done: [RESEARCH-2A.md](RESEARCH-2A.md). AppleACPIPlatform disassembly only if Route B is needed | A concrete, routable hook point and a resume strategy | None (research) |
| **2b. Sleep hook prototype** *(Route B, only if N0/N1 fail)* | S0ixStandby.kext: LPS0 `_DSM` + MWAIT loop + wake on power button | One sleep → wake cycle with apps intact; the meter shows SLP_S0 residency > 0 | Kernel: hang/panic on failure, no data risk |
| **3. Blockers** | Per-driver fixes from phase 1 (ASPM/D3 properties, kext patches, SSDTs) | ≥ 90 % S0ix residency, < 1.5 W | Varies |
| **4. Polish** | Lid wake, wake reasons in `pmset -g log`, boot-args, docs, release | Daily-driver reliable for a week | Low |

## Parallel track: hibernation fallback

Independent of the above. See [HIBERNATION-FALLBACK.md](HIBERNATION-FALLBACK.md).
Gives users a working "sleep that saves state" while S0ixStandby matures.

## Housekeeping

- **CI: GitHub Actions only.** One workflow: `tools/privacy-scan.sh` (first,
  blocking), build the kext (macOS runner, Xcode, MacKernelSDK), `iasl` the
  SSDTs, `shellcheck` the scripts, attach everything to a GitHub release on
  tag. Set up with the first code commit.
- **Privacy:** no personal or machine-identifying data in the repo (see the
  README Privacy section).
- **Generic by design:** runtime discovery only (PNP0D80, `_DSM` masks,
  LPIT). HP-specific findings stay in docs/ and optional SSDTs.

- BIOS update to 01.24.02 is recommended for other reasons: 01.22.00 fixes
  microphone-jack detection and a missing fingerprint device. After updating,
  **re-run the Linux baseline** (firmware changes can move S0ix behaviour)
  and **re-validate the EFI's ACPI patches** (`_S3`, PNLF→XNLF, `_Q86/_Q87`),
  since a new DSDT can move the bytes they search for.
- Test EFIs on the USB stick first; keep the internal EFI as the fallback.
