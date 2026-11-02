#!/bin/bash
# check-deepidle.sh — READ-ONLY probe for Apple "Deep Idle" (S0 sleep) support
# on this macOS install. Changes nothing. Run before and after adding
# SSDT-DEEPIDLE.aml, and send the output file.
#
#   ./check-deepidle.sh            (sudo is only used for `pmset -g log`)
#
# Answers (docs/RESEARCH-2A.md):
#   Q8  Does Tahoe's AppleACPIPlatform still contain the Deep Idle path?
#   Q9  Does the SMBIOS board-id's X86PlatformPlugin plist carry a sleep policy?
#       Is IOPMDeepIdleSupported published?

OUT="deepidle-check-$(date +%Y%m%d-%H%M%S).txt"
PAT='LPS0|LXEN|Deep ?Idle|DeepIdle|S0 WAKE|S0 Wake|S0 sleep|acpi_idle_kernel|IOPMDeepIdleSupported|IOPMSystemSleepType|sleep policy'

{
echo "== system"
sw_vers
uname -v
BOARD=$(ioreg -rd1 -c IOPlatformExpertDevice | awk -F'"' '/"board-id"/{print $4}' | tr -d '<>')
echo "model: $(sysctl -n hw.model)   board-id: $BOARD"
nvram boot-args 2>/dev/null

echo; echo "== IOPMrootDomain (want IOPMDeepIdleSupported = Yes after the SSDT)"
ioreg -rd1 -c IOPMrootDomain | grep -E 'DeepIdle|SleepType|SleepPolicy|Hibernate|Standby' || echo "(no matching keys)"

echo; echo "== ACPI methods visible in IODeviceTree"
ioreg -l -p IODeviceTree | grep -iE '"(PEPD|LPS0)"|PNP0D80' | head -5

echo; echo "== X86PlatformPlugin resource for this board-id"
XPP=/System/Library/Extensions/IOPlatformPluginFamily.kext/Contents/PlugIns/X86PlatformPlugin.kext/Contents/Resources
F="$XPP/$BOARD.plist"
if [ -f "$F" ]; then
  echo "$F"
  plutil -p "$F" | grep -nE 'Sleep|Idle|Standby|Hibernate' | head -40
  echo "-- top-level keys:"; plutil -p "$F" | grep -E '^  "' | head -40
else
  echo "missing: $F"; ls "$XPP" 2>/dev/null | head -5
fi
echo "-- other board-id plists that mention a sleep policy or deep idle:"
grep -l -aE 'IOPlatformSystemSleepPolicy|DeepIdle' "$XPP"/*.plist 2>/dev/null | xargs -n1 basename 2>/dev/null | head -40

echo; echo "== Strings in AppleACPIPlatform / kernel collections"
B=/System/Library/Extensions/AppleACPIPlatform.kext/Contents/MacOS/AppleACPIPlatform
if [ -f "$B" ]; then echo "binary present: $B"; strings -a "$B" | grep -E "$PAT" | sort | uniq -c
else echo "no standalone binary (expected on 11+): searching kernel collections"; fi
ls -la /System/Library/KernelCollections/ 2>&1
for KC in /System/Library/KernelCollections/*.kc /System/Library/Kernels/kernel; do
  [ -f "$KC" ] || continue
  echo "-- $KC ($(file -b "$KC" | cut -c1-60))"
  # grep -a on the raw file: `strings` can skip fileset Mach-O sections
  LC_ALL=C grep -a -o -E "$PAT" "$KC" 2>&1 | sort | uniq -c | sort -rn | head -40
done

echo; echo "== pmset"
pmset -g
echo "-- last sleep/wake entries"
pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Failure) ' | tail -15

echo; echo "== kernel log, last boot, ACPI/sleep lines"
log show --last boot --style compact --predicate 'sender == "AppleACPIPlatform" OR eventMessage CONTAINS "Deep Idle" OR eventMessage CONTAINS "S0IX"' 2>/dev/null | tail -60
} > "$OUT" 2>&1

R="$(dirname "$0")/../tools/redact.sh"; [ -x "$R" ] && "$R" "$OUT" >/dev/null
echo "Done: $OUT"
