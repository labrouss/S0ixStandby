# Testing

## Linux (reference measurements)

Boot the Ubuntu live USB (F9 → Ventoy → Ubuntu). Run everything from a
terminal **on the laptop**, not over SSH: Wi-Fi drops during suspend.

### Quick check

```bash
cat /sys/power/mem_sleep                                   # expect: [s2idle] deep
sudo cat /sys/kernel/debug/pmc_core/slp_s0_residency_usec  # note value
systemctl suspend                                          # wait, press power
sudo cat /sys/kernel/debug/pmc_core/slp_s0_residency_usec  # must have increased
```

### Power measurement

On battery (unplug the charger):

```bash
sudo ./linux/s2idle-test.sh
```

| Average power | Verdict |
|---|---|
| < 0.8 W | Proper Modern Standby |
| 1–3 W | Partial: some device blocks S0ix |
| > 3 W | Barely better than idle |

S0ix % above ~90 % is good. The HP battery's charge counter is coarse:
use 30+ minutes for a precise number.

### Full baseline capture

```bash
sudo apt install acpica-tools     # for acpidump/iasl, if missing
sudo ./linux/capture-baseline.sh  # one suspend at the end; press power to wake
```

Produces `s2idle-baseline-<date>.tar.gz`. It skips the MSDM table (Windows
product key), but review the folder before sharing it.

Then Intel's tool (run it **after** `capture-baseline.sh`: it runs
`dmesg -C`, `powertop --auto-tune`, sets RTC alarms and changes PMC
settings, all of which reset on reboot of the live USB):

```bash
sudo apt install powertop
git clone https://github.com/intel/S0ixSelftestTool && cd S0ixSelftestTool
sudo ./s0ix-selftest-tool.sh -s        # s2idle cycle: PC10, S0ix, substates, IPs not ready, DC5/DC6, ASPM
sudo ./s0ix-selftest-tool.sh -r off    # runtime PC10 with the screen off = what macOS idle should match
```

- Run it **on the laptop**, not over SSH: over SSH `-r off` tries the
  forwarded display (`localhost:10.0`) and fails.
- `-r off` turns the screen off with `xset`: log in with "Ubuntu on Xorg"
  (gear icon on the login screen), open a terminal, run with `sudo -E` so
  `DISPLAY`/`XAUTHORITY` survive.
- `-s` needs the bundled `turbostat` to be executable:
  `chmod +x turbostat` in the tool's folder. If it still fails, test the
  suspend on its own with `sudo rtcwake -m freeze -s 15`.
- Some failures still `exit 0`: read the log, not the exit code.
- Copy `*-s0ix-output.log` and `tc.out` off the stick before rebooting.

## macOS (from phase 0 on)

Before any sleep test, disable dark wakes so results are comparable:

```bash
sudo pmset -a powernap 0 tcpkeepalive 0 proximitywake 0 standby 0 autopoweroff 0 hibernatemode 0
pmset -g                     # confirm
pmset -g log | tail -50      # sleep/wake reasons after each test
```

### Phase N0: Deep Idle probe (read-only)

```bash
./macos/check-deepidle.sh        # writes deepidle-check-<date>.txt
```

Look for: strings `LPS0`, `LXEN`, `Deep Idle`, `S0 WAKE` in the kernel
collections (Q8); the board-id plist's sleep keys and whether other board-ids
carry `IOPlatformSystemSleepPolicy` (Q9).

### Phase N1: Deep Idle SSDT (USB-stick EFI only)

1. In the HP DSDT dump (Linux `capture-baseline.sh`), confirm there is no
   existing `LPS0` method under `\_SB` and no `LXEN` under `\_GPE`.
2. Copy `acpi/SSDT-DEEPIDLE.aml` to `EFI/OC/ACPI/` and add it to
   `ACPI > Add`. **Keep the `_S3` patch.**
3. Boot macOS from the stick. Run the dark-wake `pmset` line above, then
   `ioreg -rd1 -c IOPMrootDomain | grep -E 'DeepIdle|SleepType'`:
   want `IOPMDeepIdleSupported = Yes`.
4. Re-run `check-deepidle.sh` (the kernel log section should show the
   `S0IX:` debug lines if ACPI debug output reaches the log).
5. Save your work, then on battery: `sudo ./macos/sleep-power-test.sh`. After
   1–10 min, try to wake: **power button**, then **lid**, then **a key**.
6. Outcomes:

| What happens | Meaning | Next |
|---|---|---|
| Laptop powers off, `pmset` log shows Normal Sleep / EFI failure | macOS still chose S3 (type 2) | Q9: board-id/sleep policy |
| Stays on, wakes, type 7, `S0` in the log | **Route A works** | Measure W; phase N2 for the `_DSM` |
| Stays on, never wakes | Deep Idle entered, wake source missing (Q11) | Hold power 10 s; send the log from the next boot |
| Panic / reboot | Note the panic text | Send `/Library/Logs/DiagnosticReports/*.panic` |

### Phase N2: S0ixStandby.kext (USB-stick EFI)

Keep `SSDT-DEEPIDLE.aml` and `-wex-noaccel` (until WhateverXe survives sleep).
**Tiger Lake: spoof Ice Lake** (`Cpuid1Data` `E5060700…`, base64 `5QYHAAAAAAAAAAAAAAAAAA==`), not Coffee Lake: with Coffee Lake XCPM stops at C6 (FINDINGS §5).
XCPM's own view of each Deep Idle: `sysctl machdep.xcpm.deep_idle_last_stats`.

**Step 1 — meter only.** Add the kext and boot-args `-s0ixnodsm -s0ixdbg`.
```bash
./macos/s0ix-status.sh        # want: DSM Intel mask 0x7f, LPIT counter address, freq 8197
sudo ./macos/sleep-power-test.sh
./macos/s0ix-status.sh        # want: Last sleep type 7, "Last residency us" > 0 ?
```
`Last residency us` answers whether the platform reached S0ix **without**
the `_DSM` calls (0 = never, which would explain the 2 W).

**Step 2 — with `_DSM`.** Remove `-s0ixnodsm`, keep `-s0ixdbg`.
- During sleep the power LED should **breathe** (EC got `_DSM` fn 3).
- Repeat `sleep-power-test.sh` (30+ min for a precise number) and
  `s0ix-status.sh`; compare W and residency with step 1 and with Linux.
- If it does not wake or panics: boot with `-s0ixoff`, collect the panic file
  and `log show --last 30m --predicate 'eventMessage CONTAINS "S0ixStandby"'`.

## Wake failure protocol

For every sleep test, especially long ones (30 min+), note the answers below.
After the wake, or after the forced restart that followed a failed wake, run
`sudo bash macos/wake-report.sh` (it finds the last sleep in the pmset log;
or pass the sleep time) and keep the `wake-report-*.txt` with the notes.
Never commit it.

Before sleep:
- Boot entry / boot-args (`-wex-noaccel`? `-s0ixgpuprobe`?), kext versions
- AC or battery, lid open/closed, Wi-Fi on/off, Bluetooth devices connected,
  USB devices plugged in
- How it goes to sleep: Apple menu, power button, lid, idle (`pmset -g`
  `sleep` minutes), and the time

While asleep: power LED (breathing / solid / off).

On wake, before touching anything else:
1. What woke it (power button, key, lid) and the time
2. Screen: backlight on? picture? lock screen? frozen or responsive?
3. If nothing appears: press Caps Lock (LED toggles = system alive), try
   `ping`/`ssh` from another device, **wait 3 minutes** before forcing off
4. Keyboard backlight, fan, LED state
5. If forced off: time of the forced restart

## Results log

Add one row per run.

| Date | OS | BIOS | Test | Duration | Avg W | S0ix % | Notes |
|---|---|---|---|---|---|---|---|
| 2026-11-02 | Ubuntu 26.04.1 (7.0.0-30) | 01.19.00 | s2idle quick | — | — | — | residency 19.26 s, clean wake |
| 2026-11-02 | Ubuntu 26.04.1 (7.0.0-30) | 01.19.00 | s2idle-test.sh | 10.8 min | 0.76 | 98 | 11 mAh used |
| 2026-11-02 | Ubuntu 26.04.1 (7.0.0-30) | 01.19.00 | capture-baseline.sh | 30 s | — | — | +29.16 s S0i3.0, wake = ACPI fixed event (power button) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N1: SSDT-DEEPIDLE loaded | — | — | — | IOPMDeepIdleSupported = Yes |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N1: first Deep Idle sleep | ~4.3 min | — | — | Deep Idle entered (LED solid), woke, **panic 1.2 s later in WhateverXe** (context-ID-mismatch) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N1b: Deep Idle, `-wex-noaccel` | 6.5 min | **2.01** | ? | **clean wake on power button**, type 7, 19 mAh |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 step 1: kext 0.1.0 loads (`-s0ixnodsm`) | — | — | — | Intel `_DSM` mask 0x7F found; meter off (LPIT not found via registry property, fixed in 0.1.1) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 step 1: kext 0.1.1, `-s0ixnodsm`, ~16 min Deep Idle | ~16 min | ~0.7–1.2 (rough, from charge between runs) | ? | clean wake; script output lost to SSH drop. A 1.4 min run read 7.95 W: too short, discarded |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 step 1: kext 0.1.1 status after 2 sleeps (~17.5 min) | ~17.5 min | — | **0** | `Last/Total residency us = 0`: no S0ix without `_DSM` (or counter unreadable; 0.1.2 adds raw counter + PC10 to tell) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 step 1: kext 0.1.1, `-s0ixnodsm` | 9.4 min | 2.27 | **0** | LED solid (expected: `_DSM` off); confirms ~2 W baseline, the earlier 0.7–1.2 W estimate was wrong |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 step 2: kext 0.1.2, `_DSM` on | — | — | ? | **LED breathing** (EC got fn 3), **no wake** on power button or keys: hard reset |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 test A: 0.1.3, `s0ixfn=0x60` (fn 5/6) | 1.1 min | — | 0 | **wakes cleanly**, LED solid; `_DSM` entry/exit ran; **PC10 = 0 %** over 64.5 s TSC |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 (earlier): 0.1.2, all fn | — | — | ? | LED breathing, **no wake**: `Failure during sleep 0x1F` (wake never reached the CPU) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 test A: 0.1.4, `s0ixfn=0x60` | 3.4 min (TSC) | 2.17 | 0 | **PC6 95 %**, PC3 4 %, PC7–PC10 0 %: package capped at PC6 |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 test A: 0.1.5, `s0ixfn=0x60` | 2.9 min (TSC) | 2.34 | 0 | PC6 95 %, PC3 4 % again; **MSR 0xE2 = 0x74008008: limit C10**, CFG lock on, same at sleep: not a package limit |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2 test A: 0.1.5, `s0ixfn=0x60`, **CPUID spoof Ice Lake** | 3.4 min (TSC) | 1.57 | 0 | **PC10 99 %**, XCPM CC7 99 % / C10 99 %; wakes. SLP_S0 still 0 |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2: 0.1.6, **all `_DSM` fn**, Ice Lake spoof | 2.9 min (TSC) | 3.37 (too short) | 0 | **LED breathing and clean wake**; boot core CC7 99 %, PC10 99 %; SLP_S0 still 0 |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2: 0.1.7 `-s0ixpmcdump`, all fn, Ice Lake | ~3 min | — | 0 | PMC page readable; at the halt only **ISH (IP #17)** is powered vs Linux; xHCI gated |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2: 0.1.9 `-s0ixd3` (D3hot just before sleep), all fn, Ice Lake | 2.8 min (TSC) | 1.46 (short) | 0 | blocker report: **ISH, CNVI** still powered at the halt; ISH in D3hot but not gated; 6 driverless devices D3, clean wake |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | N2: 0.1.10 `-s0ixd3` (D3hot from boot) | 2.8 min (TSC) | — | 0 | ISH stays on (Live and halt) despite minutes in D3hot; CNVI on; SPI/eSPI transient |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | same, **Wi-Fi + BT off** | 1.4 min (TSC) | — | 0 | CNVI still on (Live and halt): radios off in macOS does not power CNVi down; eSPI not listed (transient confirmed) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | same, **AirportItlwm + IntelBTUnified disabled** | 1.7 min (TSC) | — | 0 | Wi-Fi 00:14.3 driverless and in D3hot, **CNVI still on** (Live and halt): CNVi, like ISH, needs more than D3hot |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | 0.1.10, all fn, `-s0ixd3`, radios on, Ice Lake | ~1 h | — | — | **hang**: LED solid, hard reset; `Failure during wake 0x…32` = **DarkWakeExit** (dark wake → full wake never finished) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | same, `pmset relative wake 180` | 3 min | — | 0 | **RTC wake works** (`Wake from Deep Idle … RTC/Alarm`) |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | same, tcpkeepalive on (no dark wake occurred) | **21.1 min** | **0.96** | 0 | **first reliable figure**; PC10 99 %, EC asleep, blockers ISH + CNVI; clean wake |
| 2026-11-02 | macOS 26.7.1 (25G241) | 01.19.00 | 0.1.11 `-s0ixd3net` (Wi-Fi function D3hot during sleep) | 2.6 min (TSC) | — | 0 | CNVI still on at the halt; **Wi-Fi did not come back after wake** → option dropped |
| 2026-11-02 | Ubuntu 26.04.1 (7.0.0-30) | 01.19.00 | PMC `0xFE001B1C` read via /dev/mem | — | — | — | `0x0140801e`, **XSQD = 1 as on macOS** → not the blocker |
| 2026-11-02 | Ubuntu 26.04.1 (7.0.0-30) | 01.19.00 | s2idle-test.sh, `module_blacklist=intel_ish_ipc` | 3.2 min | 1.48 (short) | **96** | **ISH is not a blocker** |
| 2026-11-02/03 | macOS 26.7.1 (25G241) | 01.19.00 | **Overnight**, 0.1.12, all fn, `-s0ixd3`, radios on, Ice Lake | **11 h 27 min** | **~0.7–1.0** (100 % → 80 %) | 0 | **26 RTC/Maintenance dark wakes, all back to sleep**; clean power-button wake in the morning; 46.5 min / 64 mAh first segment = 1.01 W |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.2.0, **WhateverXe 0.7.0 acceleration on** (no `-wex-noaccel`) | ~1 min | — | — | before sleep healthy; **panic 6 s after wake** `r113 ElementSwitch-second-item-null`, black screen, no ping (FINDINGS §9) |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.0 `-s0ixgpuprobe`, `-wex-noaccel` | 0.1 min (TSC) | — | 0 | GPU readable at sleep (idle, canary written); **at wake D0 but PCI memory decode off (`cmd 0x400`)**: probe skipped → 0.3.1 checks BAR0 and enables decode for the probe |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.1 `-s0ixgpuprobe`, `-wex-noaccel` | 3.3 min (TSC) | — | 0 | **GPU engine state lost** (canary in GPR0 gone); PCI config kept (BAR0 same, D0), only `cmd` 407 → 400. Without acceleration the GPU wakes blank; with acceleration still to measure (0.3.2 NVRAM record) |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.2, WhateverXe acceleration on, `-s0ixgpuprobe` | short | — | — | panic again `ElementSwitch-second-item-null` (from the GPU hard interrupt); no NVRAM record: 0.3.2's commit key was stored as a junk variable instead of committing |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.2 `-s0ixgpuprobe`, `-wex-noaccel` | 0.9 min | 3.15 (short) | 0 | `s0ix-gpu-last`: lost power (canary gone), as with 0.3.1; NVRAM write works |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.2/0.3.3 `-s0ixgpuprobe`, `-wex-noaccel`, **idle sleep on battery** | 29 min | — | 0 | **USB stick plugged in**; power-button wake completed (full wake, lock screen drawing, Wi-Fi reconnected, GPU record saved), then **unusable, no panic**; log ends 14 s after wake (may be the last saved batch); forced restart after 2.5 min. Cause open |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | **same setup as the 14:28 freeze**: 0.3.3 `-s0ixgpuprobe`, `-wex-noaccel`, idle sleep on battery | 27 min (+ dark wake XDCI 3 s after sleep, 45 s) | — | 0 | **USB stick out**; **clean power-button wake**; GPU lost power (canary); NVRAM record not committed (`commit refused`) → 0.3.4. The freeze is intermittent |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.4 `-s0ixgpuprobe`, **WhateverXe acceleration on**, Apple-menu sleep, USB stick out | ~4 min | — | — | panic again after wake; **NVRAM record survived**: `retained`, sleep HWS_PGA=0x40004000 MODE=8 → wake HWS_PGA=0x40004000 MODE=8. **With acceleration the GPU keeps its state across Deep Idle** (unlike `-wex-noaccel`) → test `-s0ixgpureset` |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.4 **`-s0ixgpureset`, WhateverXe acceleration on**, Apple-menu sleep, USB stick out | ~1 min | — | — | **first accelerated wake from Deep Idle**: GPU `retained` → full GT reset in 72 us (RCS+BCS ready); Metal 2 after wake; WhateverXe CSB: all engines re-initialised (resets 1→2), 0 errors/retries/fallbacks, events flowing; `GFX_MODE` read 0 at WhateverXe's wake init (= after the reset, before Apple's restart) |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.4 `-s0ixgpureset`, acceleration on: 3.6 min, then 1 min, then 5 min sleeps + GPU load | 3 wakes | — | 0 | **3/3 clean**: `retained` → reset 54–94 us each wake; Metal 2; WhateverXe render engine 2498 → 30615 → 32225 events, 0 errors/retries/fallbacks; no panic |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.3.4 `-s0ixgpureset`, acceleration on, Apple-menu sleep, **USB stick in** | **40 min** | — | 0 | clean power-button wake; `retained` → reset 46 us (4th this boot); Safari video playing, wallpaper 180 frames/6 s; no dark wake in this window → 0.4.0 makes the reset the default |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | **0.4.0 defaults** (no GPU boot-arg), acceleration on, `-s0ixd3`, idle sleep on battery, USB stick in | **4 h 8 min** | **~0.5** (36 % → 31 %, coarse) | 0 | **5 RTC/maintenance dark wakes + power-button wake: 6/6 GPU resets, all clean**, no panic; Wi-Fi back after wake; blocker CNVI |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.1 `-s0ixwifipm`, acceleration on, Apple-menu sleep, on AC | — | — | — | **sleep entry stalled** (LED solid, no panic): `Failure during sleep: GFX0() : Some drivers failed to handle setPowerState`; log lost with the forced power-off. Same GFX0 failure seen once before without `-s0ixwifipm` |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.1 `-s0ixwifipm` (repeat) | 4.0 min | — | 0 | clean; **Wi-Fi function D3 at the sleep and wake actions** (PMCSR 0x000b, NoSoftReset), **Wi-Fi back by itself**; GPU reset 59 us; **CNVI still powered**, gating identical to without the option → Bluetooth suspected |
| 2026-11-05 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.1 `-s0ixwifipm` + **Bluetooth off** | 7.8 min | — | 0 | Wi-Fi D3/D3; **CNVI still powered**, plus HDA_PGD0 once |
| 2026-11-05 | Ubuntu 26.04.1 | 01.19.00 | s2idle-test.sh, `module_blacklist=btusb,btintel` | 1.1 min | — | **85** | Bluetooth never initialised: S0ix anyway |
| 2026-11-05 | Ubuntu 26.04.1 | 01.19.00 | s2idle-test.sh, `module_blacklist=iwlwifi` | 1.3 min | — | **89** | Wi-Fi never initialised: S0ix anyway |
| 2026-11-06 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.1 `-s0ixpmcdump`: A Bluetooth off, B Wi-Fi off, C no `-s0ixwifipm`, D `-s0ixnodsm`, D1 `s0ixfn=0x18` | 2 min each | — | 0 | pre-halt LPM status bit-identical in A, B, C, D1; D: woke but display stayed dark, second sleep stuck (forced off). Later understood: the pre-halt snapshot cannot show the decisive bits (FINDINGS §10) |
| 2026-11-06 | Ubuntu 26.04.1 | 01.19.00 | `/dev/mem` PMC page awake and after s2idle + `substate_status_registers` / `substate_live_status_registers` | — | — | — | latched LPM status at `PWRM+0x1C3C`, live at `+0x1C5C` |
| 2026-11-06 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.3 display probe, acceleration on | 0.5 min | — | 0 | at the sleep/wake action: DC9, power wells 0, `SOUTH_CHICKEN1` 0x901; awake: DC5/DC6 never allowed, Apple DMC loaded (not Linux's 2.12) |
| 2026-11-06 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.4 `-s0ixrefclk` `-s0ixpmcdump` `-s0ixwifipm` | 4.0 min | — | 0 | refclk bit set for the sleep (0x981), PMC display block 9/FF/FF/80 at the halt like Linux, SLP_S0 still 0, PC10 99 % |
| 2026-11-06 | macOS 26.7.1 (25G241) | 01.19.00 | 0.4.4 + `s0ixfn=0x18` | short | — | 0 | first try: no breathing LED, display did not come back (forced off); second try same settings: breathing LED, clean wake. 0x1A00/0x1D80/0x1424/0x1434 change as with all functions |
| 2026-11-09/10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1 `-s0ixacnortc`, AC, **charging from 87 %** | 1 h 57 min | — | 0 | no dark wakes (RTC blocked), clean wake; battery full at wake. Freeze ~3 min later while awake |
| 2026-11-10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1 `-s0ixacnortc`, **AC at 100 %**, user sleep 3 min after boot | — | — | — | **GFX0 hang at sleep entry** (LED steady); S0ixStandby's sleep action never ran |
| 2026-11-10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1, **battery**, overnight | **8 h 56 min** | **~0.4** (100 % → 93 %) | 0 | 17 RTC/Maintenance dark wakes, all clean; PC10 99 % |
| 2026-11-10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1 `-s0ixacnortc`, AC, charging from 83 % | 1 h 28 min | — | 0 | clean; the 90 s `pmset relative wake` was blocked (as designed) |
| 2026-11-10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1 `-s0ixacnortc`, **AC at 100 %**, sleep 5 min after a full wake | — | — | — | **GFX0 hang at sleep entry** |
| 2026-11-10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1 `-s0ixacnortc`, **AC at 100 % + SSDT-BATFULL** (`FullyCharged = Yes`) | 20 s, 20 min, 42 min, **98 min** | — | 0 | **4/4 clean**, incl. a quick re-sleep after a wake; no `GaugingMitigationActions`, OBC wake ~23 h out |
| 2026-11-10 | macOS 26.7.1 (25G241) | 01.19.00 | 0.5.1, **AC at 100 % + SSDT-BATFULL, no `-s0ixacnortc`** | ~2 h | — | 0 | 5 dark wakes (3× OBC, calendar, CSPN), **45 s each** (battery: 0 s); no gauging; **GFX0 hang on the re-sleep after the 5th** → `-s0ixacnortc` still needed on AC |
