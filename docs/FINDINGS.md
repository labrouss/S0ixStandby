# Findings

Everything measured so far on the target machine. Numbers here are facts;
interpretation lives in [DESIGN.md](DESIGN.md).

## Machine

| | |
|---|---|
| Laptop | HP EliteBook 840 G8 (Tiger Lake i7, Iris Xe `8086:9a49`), HP platform ID **T37** |
| BIOS | T37 Ver. 01.19.00 (11/19/2024). Latest published: **01.24.02** |
| Battery | ~3.45 Ah @ 12.4 V ≈ **43 Wh** (estimated from `charge_now` at 99 %) |
| macOS | Tahoe 26.7.1 (also Sequoia, Ventura), OpenCore 1.0.8, SMBIOS `MacBookPro16,2` |
| EFI repo | `labrouss/EliteBook840G8` — see its `docs/SLEEP.md` |

## 1. ACPI S3 resume is broken in firmware

- Entering S3 works; **resume hangs** (black screen, needs a hard power-off).
- The **same hang happens on Linux** with `mem_sleep=deep`, so it is not a
  macOS/kext problem. `pm_trace` was used during that investigation.
- HP's BIOS release notes from 01.10.00 to 01.24.02 never mention S3, but
  **01.12.00 says "Improves modern standby"**: Windows uses Modern Standby on
  this machine and the S3 path is legacy/untested.
- Current macOS workaround: `ACPI > Patch` turns `_S3` SLP_TYP 5 → 7
  (Find `5F53335F1207040A05`, Replace `5F53335F1207040A07`), so sleep is a
  clean power-off.

## 2. Hibernation (S4-style, macOS `hibernatemode 25`) — first attempt failed

- Caused **BIOS "Real-Time Clock power loss (005)"** CMOS resets, and the
  restore hung.
- The **normal-shutdown** 005 is fixed: AppleRTC was writing into HP's
  CMOS. RTCMemoryFixup 1.0.7 with `rtcfx_exclude=80-AB` plus OpenCore
  `DisableRtcChecksum` stop it.
- The **restore** 005 is **not** fixed: per the EFI repo's `docs/SLEEP.md`,
  boot.efi writes the hibernation key into RTC 0x80–0xAB during restore,
  before macOS (and RTCMemoryFixup) runs. That attempt already used
  `HibernateMode` Auto and NVRAM. See HIBERNATION-FALLBACK.md for the next
  try (`AppleRtcRam` + `rtc-blacklist`).
- The hung restore has not been retried.

## 3. Modern Standby (s2idle / S0ix) works perfectly on Linux

Ubuntu 26.04.1 live USB, kernel 7.0.0-30-generic, BIOS 01.19.00.

- `/sys/power/mem_sleep` = `[s2idle] deep`: **s2idle is the firmware default**.
- Test 1 (short): SLP_S0 residency 0 → 19,255,992 µs. The machine **resumed
  cleanly** (live session intact; only the SSH session dropped with Wi-Fi).
- Test 2 (`linux/s2idle-test.sh`, on battery, 2026-11-02):

| Metric | Result |
|---|---|
| Elapsed | 10.8 min (645 s) |
| Battery used | 11.0 mAh = 0.14 Wh |
| **Average power** | **0.76 W** (≈ 0.6–0.9 W given the coarse counter) |
| **S0ix residency** | **634.4 s of 645 s = 98 %** |
| Projected standby | ≈ 1.8 %/hour, ≈ 2 days from full |

**Conclusion:** hardware and firmware fully support S0ix with a clean wake.
The only missing piece for real sleep on macOS is software.

## 4. Baseline capture (2026-11-02, `s2idle-baseline-20261102-093936`)

| Item | Value | Why it matters |
|---|---|---|
| FADT | `Low Power S0 Idle = 1`, `Control Method Power Button = 0` | S0ix declared; power button is the **fixed-feature** button (PM1 status, SCI) |
| Wake on Linux | `ACPI: PM: ACPI fixed event wakeup` via SCI (IRQ 9) | Power-button wake needs only the SCI: answers part of Q11 |
| Substates | Only **S0i3.0** used (S0i2.0 = 0); +29.16 s in the capture suspend | Target state is S0i3 |
| LPIT | Native C-state entry `FFH 0x60` (MWAIT C10), residency counter **MMIO `0xFE00193C`** (32-bit), **8197 Hz** (≈ 122 µs/tick); min. residency 30 ms, latency 3 ms | The phase 0 meter reads this generically from LPIT: no hardcoded PWRMBASE |
| LPS0 device | `\_SB.PEPD` (`INT33A1`, `_CID PNP0D80`) in SSDT14 `MS-SSDT` | — |
| `_DSM` GUIDs | Intel `c4eb40a0-…` mask `0x7F` (fn 0–6); `57a6512e-…` (fn 1, L0IX buffer). **No Microsoft `11e00d56-…`** | — |
| fn 3 / 4 (display off / on) | `ECNT(1)` / `ECNT(0)`: EC `SLPT = 6` / `0`, GPIO `0x090B000E`, `ECUP` | **EC sleep signalling: Deep Idle without `_DSM` leaves the EC awake** |
| fn 5 / 6 (entry / exit) | `GUAM(1)` / `GUAM(0)`, `ECNT(3/2)`, `DPOF` for TCSS xHCI | Platform "user absent" mode + TCSS |
| Name conflicts | No `LPS0`, `LXEN`, `_TTS` anywhere in DSDT/SSDTs | `SSDT-DEEPIDLE` is safe to add |
| ACPI root | Host bridge is **`\_SB.PC00`**, not `PCI0` | Any new SSDT must use `PC00` paths |

The capture script now also saves `substate_requirements` and
`substate_live_status_registers` (missed in this run). Re-run it after the
BIOS update anyway.

`s0ix-selftest-tool.sh` did not run usefully: `-s` could not run its
bundled `turbostat`, and `-r off` was started over SSH (`localhost:10.0`,
X forwarding). See TESTING.md.

## 5. macOS Deep Idle on this HP (2026-11-02)

| Item | Result |
|---|---|
| `SSDT-DEEPIDLE` | macOS sleeps with type 7 (Deep Idle) and wakes on the power button (`PWRB`) |
| **CPUID spoof** | With `Cpuid1Data` = Coffee Lake (`0x906EA`) XCPM never goes below **core C6 / package PC6** in Deep Idle (`machdep.xcpm.deep_idle_last_stats`: CC7 0 %, C6 95 %). With **Ice Lake (`0x706E5`)**: **CC7 99 %, package C10 99 %** |
| Why | XCPM picks idle states from the table of the CPU model it is told. Tiger Lake reports `CPUID.5:EDX` = `0x11121020` (no MWAIT C3 hint, fewer sub-states), identical to Ice Lake, unlike Coffee Lake. **Tiger Lake hackintoshes must spoof Ice Lake for deep idle** |
| MSR 0xE2 | `0x74008008`: package limit C10, CFG lock on (not a limit) |
| LPS0 `_DSM` fn 5/6 | Safe: wakes normally |
| LPS0 `_DSM` fn 3/4 | Puts the EC into its standby mode (power LED breathes); with the Coffee Lake spoof the wake never reached the CPU (`Failure during sleep 0x1F`) |
| SLP_S0 residency | 0 so far, even at PC10 99 % (fn 3/4 not yet tested with the Ice Lake spoof) |
| WhateverXe | Acceleration panics on any wake (`context-ID-mismatch`); `-wex-noaccel` avoids it. Upstream: sleep unsupported |

## Still to capture

- A 30–60 min run of `s2idle-test.sh` for a tighter power number.
- `s0ix-selftest-tool.sh -s` and `-r off` from the laptop's own X session.
