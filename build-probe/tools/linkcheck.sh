#!/bin/sh
# Offline link check for a kext (no loading): every undefined symbol of the
# kext binary must be resolved by kmutil's library search over the installed
# Boot/System KCs. Prints unresolved symbols and exits 1 if any.
# Usage: tools/linkcheck.sh /abs/path/To.kext   (use /private/tmp, not /tmp)
set -eu
K="$1"
EXE=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$K/Contents/Info.plist")
TMP=$(mktemp -d /private/tmp/linkcheck.XXXXXX)
nm -u "$K/Contents/MacOS/$EXE" | sort -u > "$TMP/imports"
kmutil libraries -p "$K" --onedef-symbols --multdef-symbols 2>/dev/null \
  | awk '/ in \/System\//{print $1}' | sort -u > "$TMP/resolved"
echo "imports: $(wc -l < "$TMP/imports")  resolved: $(wc -l < "$TMP/resolved")"
comm -23 "$TMP/imports" "$TMP/resolved" > "$TMP/unresolved"
if [ -s "$TMP/unresolved" ]; then
  echo "UNRESOLVED:"; sed 's/^/  /' "$TMP/unresolved"; rm -rf "$TMP"; exit 1
fi
echo "all symbols resolved"; rm -rf "$TMP"
