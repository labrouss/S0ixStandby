#!/bin/bash
# s0ix-status.sh — show what S0ixStandby.kext found and measured. Read-only.
#   ./macos/s0ix-status.sh
set -u
echo "== kext"
kextstat 2>/dev/null | grep -i s0ixstandby || kmutil showloaded 2>/dev/null | grep -i s0ixstandby || echo "S0ixStandby not loaded"
echo
echo "== S0ixStandby properties"
ioreg -rd1 -c S0ixStandby -w0 | grep -E '"(DSM|LPIT|C-state meter|CPU model|PKG_CST|D3|SLP_S0 blocker|Sleep count|Last|Total|Residency|Wi-Fi|AC RTC|On AC|Battery full|EC )' || echo "(no S0ixStandby service)"
echo
echo "== Last sleep (computed)"
P=$(ioreg -rd1 -c S0ixStandby -w0)
val() { echo "$P" | awk -F' = ' -v k="\"$1\"" '$1 ~ k {gsub(/[ "]/,"",$2); print $2; exit}'; }
TSCF=$(sysctl -n machdep.tsc.frequency 2>/dev/null)
TSC=$(val "Last TSC ticks"); RES=$(val "Last residency us")
if [ -n "$TSC" ] && [ -n "$TSCF" ] && [ "$TSCF" -gt 0 ]; then
  awk -v t="$TSC" -v f="$TSCF" -v r="${RES:-0}" 'BEGIN{
    s=t/f; printf "Asleep (TSC):  %.1f min\nS0ix (SLP_S0): %.1f %% (%.1f s)\n", s/60, (s>0?100*r/1e6/s:0), r/1e6 }'
  echo "C-states (% of sleep):"
  echo "$P" | grep '"Last C-state percent"' | sed 's/.*= {//; s/}$//; s/,/  /g; s/"//g; s/^/  /'
  echo "Halt window on the boot CPU (% of halt):"
  echo "$P" | grep '"Last halt C-state percent"' | sed 's/.*= {//; s/}$//; s/,/  /g; s/"//g; s/^/  /'
else
  echo "(no C-state/TSC data yet: needs kext >= 0.1.4 and one sleep)"
fi
# The kext reports only the last Deep Idle stretch; sum the whole boot from pmset.
# kern.boottime is "{ sec = N, usec = M } date": take sec, not usec.
BT=$(sysctl -n kern.boottime 2>/dev/null | sed -nE 's/^\{ sec = ([0-9]+),.*/\1/p')
if [ -n "$BT" ] && [ "$BT" -gt 1000000000 ]; then
  BTS=$(date -r "$BT" "+%Y-%m-%d %H:%M:%S")
  pmset -g log 2>/dev/null | awk -v b="$BTS" 'substr($0,1,19) >= b {
      if ($0 ~ /Entering Sleep state/) { n++; if (match($0, /[0-9]+ secs *$/)) t += substr($0, RSTART) + 0 }
      if ($0 ~ /DarkWake from/) d++
    } END { printf "Since boot:    %d sleeps, %d dark wakes, %.1f h asleep\n", n, d, t/3600 }'
fi
echo "Live (awake, every 10 s; many IPs are normally on while awake):"
echo "$P" | grep '"Live SLP_S0 blockers"' | sed 's/.*= (//; s/)$//; s/"//g; s/^/  /' || true
echo "Likely SLP_S0 blockers (IPs still powered at the halt):"
echo "$P" | grep '"Last SLP_S0 blockers at halt"' | sed 's/.*= (//; s/)$//; s/"//g; s/^/  /' || true
echo
echo "== GPU (-s0ixgpuprobe / -s0ixgpureset)"
if echo "$P" | grep -q '"GPU registers at wake"'; then
  echo "$P" | grep -E '"GPU"|"GPU (state after sleep|reset|reset count|retained count)"' | sed 's/^ *//; s/"//g; s/^/  /'
  echo "$P" | grep -E '"GPU registers at (sleep|wake)"' | sed 's/^ *//; s/"//g; s/^/  /'
else
  echo "$P" | grep -E '"GPU"' | sed 's/^ *//; s/"//g; s/^/  /' || echo "  (off)"
fi
echo
echo "== Display power (DMC / DC states)"
echo "$P" | grep -E '"Display (DMC firmware|power now|power at sleep|power at wake|power 15 s after wake|refclk workaround)"' | sed 's/^ *//; s/"//g; s/^/  /' || echo "  (not reported: needs 0.4.3)"
echo
echo "== Deep Idle"
ioreg -rd1 -c IOPMrootDomain | grep -E 'IOPMDeepIdleSupported|IOPMSystemSleepType'
echo
echo "== Battery"
echo "$P" | grep '"Battery full state"' | sed 's/^ *//; s/"//g; s/^/  /' || echo "  (not reported: needs 0.5.2)"
if echo "$P" | grep -q '"Battery full state" = "stuck'; then
  echo "  -> the EC stopped charging just short of full; see docs/BATTERY-FULL.md (SSDT-BATFULL)"
fi
echo
echo "== S0ixStandby log (Recent log, kept by the kext; 0.5.3+)"
RL=$(echo "$P" | grep '"Recent log"' | sed 's/^[^(]*= (//; s/)$//')
if [ -n "$RL" ]; then
  RL=${RL#\"}; RL=${RL%\"}
  # one entry per line (BSD sed can't insert newlines; bash can)
  printf '%s\n' "${RL//\",\"/$'\n'}" | tail -25 | sed 's/^/  /'
else
  echo "  (not reported: needs 0.5.3)"
fi
echo
echo "== kernel log (this boot)"
# IOLog lines are in the kernel message buffer; the unified log is the fallback.
# dmesg first: it's instant, while log show --last boot can take a minute.
KL=$(dmesg 2>/dev/null | grep "S0ixStandby:")
[ -z "$KL" ] && KL=$(log show --last 30m --style compact --predicate 'eventMessage CONTAINS "S0ixStandby:"' 2>/dev/null | grep -v kernelmanager | grep "S0ixStandby:")
if [ -n "$KL" ]; then echo "$KL" | tail -20
elif [ "$(id -u)" -ne 0 ]; then echo "  (none: run with sudo for dmesg)"
else echo "  (no S0ixStandby lines in dmesg or the last 30 min; most appear only with -s0ixdbg)"; fi
