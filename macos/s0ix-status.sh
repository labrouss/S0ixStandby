#!/bin/bash
# s0ix-status.sh — show what S0ixStandby.kext found and measured. Read-only.
#   ./macos/s0ix-status.sh
set -u
echo "== kext"
kextstat 2>/dev/null | grep -i s0ixstandby || kmutil showloaded 2>/dev/null | grep -i s0ixstandby || echo "S0ixStandby not loaded"
echo
echo "== S0ixStandby properties"
ioreg -rd1 -c S0ixStandby -w0 | grep -E '"(DSM|LPIT|C-state meter|CPU model|PKG_CST|D3|SLP_S0 blocker|Sleep count|Last|Total|Residency)' || echo "(no S0ixStandby service)"
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
echo "Live (awake, every 10 s; many IPs are normally on while awake):"
echo "$P" | grep '"Live SLP_S0 blockers"' | sed 's/.*= (//; s/)$//; s/"//g; s/^/  /' || true
echo "Likely SLP_S0 blockers (IPs still powered at the halt):"
echo "$P" | grep '"Last SLP_S0 blockers at halt"' | sed 's/.*= (//; s/)$//; s/"//g; s/^/  /' || true
echo
echo "== Deep Idle"
ioreg -rd1 -c IOPMrootDomain | grep -E 'IOPMDeepIdleSupported|IOPMSystemSleepType'
echo
echo "== kernel log (this boot)"
log show --last boot --style compact --predicate 'sender == "S0ixStandby" OR eventMessage CONTAINS "S0ixStandby:"' 2>/dev/null | grep -v kernelmanager | tail -20
