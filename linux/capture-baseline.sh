#!/bin/bash
# capture-baseline.sh — collect the Linux "known good" Modern Standby picture
# for the HP EliteBook 840 G8, so the macOS side has something to compare to.
#
# READ-ONLY: it only reads sysfs/debugfs, logs and ACPI tables. It does one
# s2idle suspend at the end (press the power button to wake) so the
# per-device constraint and substate data reflect a real suspend.
#
# Run as root from the Ubuntu live USB:
#   sudo ./capture-baseline.sh
# Produces: s2idle-baseline-<date>.tar.gz in the current directory.
#
# PRIVACY: the ACPI dump skips MSDM (Windows product key). Before archiving,
# tools/redact.sh strips MAC/IP addresses, serial numbers, UUIDs and user
# paths from the text files (dmesg can contain all of these). Review the folder
# before sharing it anyway, and never commit it (see .gitignore).

set -u
[ "$(id -u)" = 0 ] || { echo "Run as root (sudo)."; exit 1; }

OUT="s2idle-baseline-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT/pmc_core" "$OUT/acpi"
P=/sys/kernel/debug/pmc_core

echo "== system" | tee "$OUT/system.txt"
{ uname -a; cat /etc/os-release 2>/dev/null | head -3
  cat /sys/class/dmi/id/{sys_vendor,product_name,bios_version,bios_date} 2>/dev/null
  echo "mem_sleep: $(cat /sys/power/mem_sleep)"
  grep -m1 "model name" /proc/cpuinfo; } >> "$OUT/system.txt"

snap() {  # $1 = label
  for f in slp_s0_residency_usec substate_residencies substate_status_registers \
           substate_requirements substate_live_status_registers \
           pch_ip_power_gating_status ltr_show package_cstate_show die_c6_us_show \
           latency_tolerance_ignore; do
    [ -r "$P/$f" ] && cat "$P/$f" > "$OUT/pmc_core/$1-$f.txt" 2>/dev/null
  done
  cat /sys/power/suspend_stats/* > "$OUT/suspend_stats-$1.txt" 2>/dev/null
}

echo "== pmc_core (before)"; ls "$P" > "$OUT/pmc_core/available.txt" 2>/dev/null; snap before

echo "== PCI devices, power states and ASPM"
lspci -nn > "$OUT/lspci-nn.txt"
lspci -vvv 2>/dev/null | grep -E "^[0-9a-f]{2}:|LnkCtl:|L1SubCtl1|Status: D[0-3]" > "$OUT/lspci-pm.txt"
for d in /sys/bus/pci/devices/*; do
  echo "$(basename $d) $(cat $d/power_state 2>/dev/null) runtime=$(cat $d/power/runtime_status 2>/dev/null) d3cold=$(cat $d/d3cold_allowed 2>/dev/null)"
done > "$OUT/pci-power.txt"

echo "== ACPI tables (excluding MSDM)"
if command -v acpidump >/dev/null; then
  ( cd "$OUT/acpi" && acpidump -b >/dev/null 2>&1; rm -f msdm*.dat )
  command -v iasl >/dev/null && ( cd "$OUT/acpi" && iasl -d *.dat >/dev/null 2>&1 )
else
  echo "acpidump missing: sudo apt install acpica-tools" | tee "$OUT/acpi/README.txt"
fi
grep -l -i "c4eb40a0\|11e00d56\|PEPD\|INT33A1\|LPI" "$OUT"/acpi/*.dsl 2>/dev/null > "$OUT/acpi/lps0-candidates.txt"

echo "== one s2idle suspend (press power to wake)"
echo 1 > /sys/power/pm_debug_messages 2>/dev/null
echo s2idle > /sys/power/mem_sleep
sleep 2; echo mem > /sys/power/state; sleep 5
snap after

echo "== kernel log"
dmesg > "$OUT/dmesg.txt"
grep -iE "lps0|s2idle|PM: suspend|PM: resume|wakeup|pmc_core|ACPI: EC" "$OUT/dmesg.txt" > "$OUT/dmesg-pm.txt"

R="$(dirname "$0")/../tools/redact.sh"
[ -x "$R" ] && "$R" "$OUT" || echo "WARNING: tools/redact.sh not found, archive is NOT redacted"
tar czf "$OUT.tar.gz" "$OUT" && echo "Done: $OUT.tar.gz"
