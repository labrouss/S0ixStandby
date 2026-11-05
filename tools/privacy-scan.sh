#!/bin/bash
# privacy-scan.sh — fail if tracked files contain personal identifiers.
# Run locally before committing and in CI (.github/workflows).
#   tools/privacy-scan.sh [path]   (default: git ls-files, else all files)
set -u
cd "${1:-.}" || exit 2
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then FILES=$(git ls-files)
else FILES=$(find . -type f -not -path './.git/*'); fi

PAT='([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}'                     # MAC
PAT+='|\b(10|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]+\.[0-9]+'  # private IPv4
PAT+='|[A-Za-z0-9._%+-]+@(gmail|yahoo|outlook|hotmail|icloud|me)\.[a-z]+'  # personal e-mail
PAT+='|IOPlatformSerialNumber|IOPlatformUUID|SystemSerialNumber|board-serial-number'
PAT+='|<key>(MLB|ROM|SystemUUID)</key>'                      # OpenCore PlatformInfo
PAT+='|/Users/[A-Za-z0-9._-]+/|/home/[A-Za-z0-9._-]+/'
PAT+='|SerialNumber[ :=]+[A-Za-z0-9]{6,}'

FAIL=0
for f in $FILES; do
  f=${f#./}
  case "$f" in tools/privacy-scan.sh|tools/redact.sh|*.aml|*.png|*.jpg) continue;; esac
  if hits=$(LC_ALL=C grep -nIE "$PAT" "$f" 2>/dev/null); then
    echo "::error file=$f::possible personal data"; echo "$hits" | sed "s|^|$f:|"; FAIL=1
  fi
done
# Files that must never be committed
for f in $FILES; do
  f=${f#./}
  case "$f" in
    *config.plist|*.tar.gz|*.panic|*wake-report-*|*sleep-power-*.log|*deepidle-check-*|*s2idle-baseline-*|*s0ix-output.log|*.shutdownStall|*msdm*|*MSDM*)
      echo "::error file=$f::data file must not be committed"; FAIL=1;;
  esac
done
[ $FAIL = 0 ] && echo "privacy-scan: clean"
exit $FAIL
