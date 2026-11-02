#!/bin/bash
# pmc-dump.sh — save the PMC register page snapshots taken by S0ixStandby
# (boot-arg -s0ixpmcdump): at boot and at the last Deep Idle halt.
#   ./macos/pmc-dump.sh      -> pmc-<date>.txt (offset: little-endian dwords)
# Hardware registers only: no personal data.
set -u
OUT="pmc-$(date +%Y%m%d-%H%M%S).txt"
TMP=$(mktemp -d)
ioreg -rd1 -c S0ixStandby -a > "$TMP/s.plist" || { echo "S0ixStandby not loaded"; exit 1; }
{
  echo "PMC page address: $(plutil -extract '0.PMC page address' raw "$TMP/s.plist" 2>/dev/null)"
  echo "Sleep count: $(plutil -extract '0.Sleep count' raw "$TMP/s.plist" 2>/dev/null)"
  for k in "PMC page at boot" "PMC page at halt"; do
    echo; echo "== $k"
    if plutil -extract "0.$k" raw "$TMP/s.plist" > "$TMP/b64" 2>/dev/null; then
      base64 -D -i "$TMP/b64" -o "$TMP/bin" && xxd -e -g4 -c16 "$TMP/bin"
    else
      echo "(not present: boot with -s0ixpmcdump; the halt page needs one Deep Idle sleep)"
    fi
  done
} > "$OUT"
rm -rf "$TMP"
echo "Saved: $OUT"
