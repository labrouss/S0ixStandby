#!/bin/bash
# s2idle-test.sh — measure Modern Standby (s2idle / S0ix) quality on Linux.
#
# Suspends once via s2idle, then reports elapsed time, battery used, average
# power and the share of time the platform spent in S0ix (SLP_S0 residency).
#
# Run as root, ON BATTERY, from a terminal on the laptop itself (not SSH —
# Wi-Fi drops during suspend and cuts the session):
#   sudo ./s2idle-test.sh
#
# The HP battery reports charge (uAh) + voltage (uV), not energy, so energy is
# computed as charge x average voltage. The charge counter is coarse: runs
# shorter than ~10 minutes may show ~0 mAh used.

B=/sys/class/power_supply/BAT0
RES=/sys/kernel/debug/pmc_core/slp_s0_residency_usec

[ "$(id -u)" = 0 ] || { echo "Run as root (sudo)."; exit 1; }
[ -r "$RES" ] || { echo "Missing $RES (intel_pmc_core not loaded or debugfs not mounted)."; exit 1; }
[ "$(cat $B/status)" = "Discharging" ] || { echo "Unplug the charger first (status: $(cat $B/status))"; exit 1; }

echo s2idle > /sys/power/mem_sleep
c1=$(cat $B/charge_now); v1=$(cat $B/voltage_now); t1=$(date +%s); hw1=$(cat $RES)
echo "Start: $(date)  charge=${c1} uAh  voltage=${v1} uV  capacity=$(cat $B/capacity)%"
echo "Suspending — leave it ~10 minutes (30+ for a precise number), then press the power button."
sleep 2; echo mem > /sys/power/state          # returns after wake
sleep 5                                        # let the battery reading settle
c2=$(cat $B/charge_now); v2=$(cat $B/voltage_now); t2=$(date +%s); hw2=$(cat $RES)

awk -v c1=$c1 -v c2=$c2 -v v1=$v1 -v v2=$v2 -v t1=$t1 -v t2=$t2 -v h1=$hw1 -v h2=$hw2 'BEGIN{
  dt=t2-t1; dmah=(c1-c2)/1000; vavg=(v1+v2)/2/1e6
  wh=dmah/1000*vavg; w=(dt>0)?wh/(dt/3600):0
  printf "Elapsed: %.1f min\nUsed:    %.1f mAh (%.2f Wh)\nAverage: %.2f W\nS0ix:    %.1f s of %d s (%.0f%%)\n",
         dt/60, dmah, wh, w, (h2-h1)/1e6, dt, 100*(h2-h1)/1e6/dt }'
echo "End:   $(date)  capacity=$(cat $B/capacity)%"
