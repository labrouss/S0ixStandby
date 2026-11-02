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
