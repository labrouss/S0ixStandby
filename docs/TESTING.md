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
