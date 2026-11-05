#!/bin/bash
# wake-report.sh — collect everything about the last sleep/wake in one file.
# Run after a wake, or after the forced restart that followed a failed wake:
#   sudo bash macos/wake-report.sh              # last sleep found in pmset log
#   sudo bash macos/wake-report.sh "2026-11-05 13:59"   # or give the sleep time
# Writes ./wake-report-<date>.txt (personal identifiers redacted). Never
# commit it; attach it to the test notes instead.

[ "$(id -u)" = 0 ] || { echo "Run with sudo."; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="wake-report-$(date +%Y%m%d-%H%M%S).txt"

# Start of the window: the given time, else the last user/idle "Entering Sleep"
# line (maintenance re-sleeps after dark wakes are skipped).
# pmset separates its columns with tabs or spaces.
START=${1:-$(pmset -g log | grep -E "^[0-9]{4}-[0-9]{2}-[0-9]{2} .*[[:space:]]Sleep[[:space:]]+Entering Sleep" | grep -v "Maintenance Sleep" | tail -1 | cut -c1-19)}
[ -n "$START" ] || START=$(date -v-1H '+%Y-%m-%d %H:%M:%S')
# If this boot began after the sleep (panic or forced restart), the wake to
# look at is in the previous boot: end the log window at this boot.
BOOT=$(date -r "$(sysctl -n kern.boottime | sed -E 's/^\{ sec = ([0-9]+).*/\1/')" '+%Y-%m-%d %H:%M:%S')
END=()
[[ "$BOOT" > "$START" ]] && END=(--end "$BOOT")

{
echo "== wake-report $(date)   window from: $START${END[1]:+ to $BOOT (previous boot)}"
echo "uptime: $(uptime)"
echo
echo "== Versions"
sw_vers | tr '\n' ' '; echo
kmutil showloaded 2>/dev/null | grep -E -i "s0ixstandby|whateverxe|lilu|itlwm|IntelBT|TGL" | awk '{print $6, $7}'
echo "boot-args: $(nvram boot-args 2>/dev/null | cut -f2-)"
echo
echo "== Boots, sleeps and wakes since the window start (pmset)"
pmset -g log | awk -v s="$START" '/^[0-9][0-9][0-9][0-9]-/ && substr($0,1,19) >= s' \
  | grep -E "[[:space:]](Sleep|Wake|DarkWake|Failure|Wake Requests|Kernel Client Acks)[[:space:]]" | tail -40
echo
echo "== Panic reports since the window start"
found=0
for f in /Library/Logs/DiagnosticReports/*.panic; do
  [ -e "$f" ] || continue
  m=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$f")
  [[ "$m" > "$START" ]] || continue
  found=1
  echo "-- $f ($m)"
  sed -n 's/.*"macOSPanicString":"//p' "$f" | cut -c1-1500 | sed 's/\\n/\n/g' | head -40
done
[ $found = 1 ] || echo "(none)"
echo
echo "== NVRAM records"
nvram -p 2>/dev/null | grep -E "^s0ix-|SYNCNOW" || echo "(none)"
echo
echo "== S0ixStandby (current boot)"
ioreg -rd1 -c S0ixStandby -w0 | grep -E '"(Sleep count|Last sleep type|Last SLP_S0 blockers at halt|Last C-state percent|GPU[^"]*|Wi-Fi[^"]*)"' || echo "(not loaded)"
echo
echo "== Last 60 log lines of the window (any process)"
echo "(after a forced power-off the newest entries may be missing: the log is written in batches)"
log show --start "$START" "${END[@]}" --style compact 2>/dev/null | tail -60
echo
echo "== Kernel, graphics, Wi-Fi and display lines in the window (last 80)"
log show --start "$START" "${END[@]}" --style compact --predicate \
  'process == "kernel" OR process == "WindowServer" OR process == "loginwindow" OR eventMessage CONTAINS[c] "itlwm" OR eventMessage CONTAINS[c] "S0ixStandby" OR eventMessage CONTAINS[c] "WhateverXe" OR eventMessage CONTAINS[c] "backlight"' \
  2>/dev/null | grep -v -E "Sandbox|AMFI" | tail -80
} > "$OUT" 2>&1

[ -x "$HERE/../tools/redact.sh" ] || chmod +x "$HERE/../tools/redact.sh" 2>/dev/null
bash "$HERE/../tools/redact.sh" "$OUT" >/dev/null 2>&1 && rm -f "$OUT.bak"
echo "Saved: $OUT ($(wc -l < "$OUT") lines). Review, then attach it."
