#!/bin/bash
# s0ix-status.sh — show what S0ixStandby.kext found and measured. Read-only.
#   ./macos/s0ix-status.sh
set -u
echo "== kext"
kextstat 2>/dev/null | grep -i s0ixstandby || kmutil showloaded 2>/dev/null | grep -i s0ixstandby || echo "S0ixStandby not loaded"
echo
echo "== S0ixStandby properties"
ioreg -rd1 -c S0ixStandby -w0 | grep -E '"(DSM|LPIT|Sleep count|Last|Total|Residency)' || echo "(no S0ixStandby service)"
echo
echo "== Deep Idle"
ioreg -rd1 -c IOPMrootDomain | grep -E 'IOPMDeepIdleSupported|IOPMSystemSleepType'
echo
echo "== kernel log (this boot; dmesg needs sudo)"
dmesg 2>/dev/null | grep "S0ixStandby:" | tail -20
log show --last boot --style compact --predicate 'eventMessage CONTAINS "S0ixStandby:"' 2>/dev/null | grep "S0ixStandby:" | tail -20
