#!/bin/bash
# redact.sh — strip personal identifiers from collected logs, in place.
#   tools/redact.sh <file|dir>...
# Removes: MAC addresses, IPv4/IPv6 addresses, e-mail addresses, USB/PCIe/disk
# serial numbers, hardware UUIDs, SMBIOS serial/MLB/ROM values, hostnames in
# shell prompts and /Users|/home paths. Only upper-case UUIDs are removed:
# lower-case ACPI _DSM GUIDs are public spec values. ACPI .dsl dumps are left
# as-is (MSDM is never dumped). Review the result before sharing anyway.
set -u
red() {
  LC_ALL=C sed -E -i.bak \
    -e 's/([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}/<MAC>/g' \
    -e 's/\b([0-9]{1,3}\.){3}[0-9]{1,3}\b/<IP>/g' \
    -e 's/\b([0-9A-Fa-f]{1,4}:){5,7}[0-9A-Fa-f]{1,4}\b/<IPV6>/g' \
    -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/<EMAIL>/g' \
    -e 's/(Serial ?Number|SerialNumber|serial|Serial|SN|Device Serial Number|USB Serial)([ :=]+)[^ ,;)]+/\1\2<SERIAL>/g' \
    -e 's/(IOPlatformSerialNumber|IOPlatformUUID|SystemSerialNumber|SystemUUID|MLB|ROM|board-serial-number)("? *[=:] *)[^,}]+/\1\2<REDACTED>/g' \
    -e 's/\b[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\b/<UUID>/g' \
    -e 's#/(Users|home)/[^/ ]+#/\1/<USER>#g' \
    -e 's/^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+ /<USER>@<HOST> /' \
    "$1" && rm -f "$1.bak"
}
for t in "$@"; do
  if [ -d "$t" ]; then
    find "$t" -type f \( -name '*.txt' -o -name '*.log' -o -name '*.panic' -o -name '*.out' \) -print0 |
      while IFS= read -r -d '' f; do red "$f"; done
  else red "$t"; fi
done
echo "Redacted: $*"
