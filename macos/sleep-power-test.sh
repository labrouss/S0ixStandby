#!/bin/bash
# sleep-power-test.sh — macOS counterpart of linux/s2idle-test.sh.
# Sleeps once, then reports elapsed time, battery used, average power and the
# sleep type macOS actually used (Deep Idle vs Normal Sleep / S3).
#
# Run ON BATTERY from Terminal on the laptop:
#   sudo ./sleep-power-test.sh
# Leave it asleep 10+ minutes (30+ for a precise number), then wake it.
# Needs SMCBatteryManager (AppleSmartBattery in ioreg).

[ "$(id -u)" = 0 ] || { echo "Run with sudo."; exit 1; }

# Keep running when the SSH session drops (Wi-Fi goes down during sleep) and
# keep a copy of the output: results land in ./sleep-power-<date>.log
trap '' HUP
LOG="sleep-power-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1
pmset -g batt | grep -q "discharging" || { echo "Unplug the charger first."; pmset -g batt; exit 1; }

bat() { ioreg -rn AppleSmartBattery | awk -F' = ' -v k="\"$1\"" '$1 ~ k {gsub(/ /,"",$2); print $2; exit}'; }
c1=$(bat AppleRawCurrentCapacity); v1=$(bat Voltage); t1=$(date +%s)
echo "Start: $(date)  charge=${c1} mAh  voltage=${v1} mV  type before: $(ioreg -rd1 -c IOPMrootDomain | awk -F' = ' '/IOPMSystemSleepType/{print $2}')"
echo "Sleeping now. Wake it after 10+ minutes."
sleep 2
pmset sleepnow >/dev/null
sleep 20                                   # the process is frozen while asleep
c2=$(bat AppleRawCurrentCapacity); v2=$(bat Voltage); t2=$(date +%s)

awk -v c1="$c1" -v c2="$c2" -v v1="$v1" -v v2="$v2" -v t1="$t1" -v t2="$t2" 'BEGIN{
  dt=t2-t1-22; if (dt<1) dt=1; dmah=c1-c2; vavg=(v1+v2)/2/1000
  wh=dmah/1000*vavg; w=wh/(dt/3600)
  printf "Elapsed asleep: %.1f min\nUsed:    %d mAh (%.2f Wh)\nAverage: %.2f W\n", dt/60, dmah, wh, w
  if (dt < 600) print "WARNING: under 10 min: the battery counter is too coarse, treat W as unreliable" }'
echo "Sleep type reported: $(ioreg -rd1 -c IOPMrootDomain | awk -F' = ' '/IOPMSystemSleepType/{print $2}')  (7 = Deep Idle, 2 = Normal/S3)"
echo "-- pmset log:"
pmset -g log | grep -E ' (Sleep|Wake|DarkWake|Failure) ' | tail -4
echo "-- AppleACPIPlatform:"
log show --last 30m --style compact --predicate 'sender == "AppleACPIPlatform"' 2>/dev/null | grep -iE 'sleep|wake|S0|S3' | tail -10
echo "-- S0ixStandby (if loaded):"
ioreg -rd1 -c S0ixStandby -w0 | grep -E '"(Last|Total|Sleep count)' || echo "(not loaded)"
echo "Saved: $LOG"
