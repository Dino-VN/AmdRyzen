#!/bin/sh
# Offline link check for a kext (no loading): every undefined symbol of the
# kext binary must be resolved by kmutil's library search over the installed
# Boot/System KCs. Prints unresolved symbols and exits 1 if any.
#
# Extra arguments name library kexts that are not in the KCs (VirtualSMC, Lilu,
# the freshly built AMDZenPM...): a .kext bundle, or a directory searched for
# *.kext one level deep. `kmutil libraries --repository` does not search such
# bundles for symbol definitions (it only reports KC symbols), so their exports are
# read with `nm -gU` instead. Like the kernel linker, only libraries the kext names
# in OSBundleLibraries count, and each such library must accept the requested
# version (OSBundleCompatibleVersion <= requested <= CFBundleVersion).
#
# Usage: tools/linkcheck.sh /abs/path/To.kext [LIB.kext | DIR ...]
#        (use /private/tmp, not /tmp)
set -eu
K="$1"; shift
EXE=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$K/Contents/Info.plist")
SELF=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$K/Contents/Info.plist")
TMP=$(mktemp -d /private/tmp/linkcheck.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
nm -u "$K/Contents/MacOS/$EXE" | sort -u > "$TMP/imports"
# "sym in /System/...kc: bundle.id (ver)". Like the kernel linker, a KC symbol only
# counts when its bundle is one of the kext's OSBundleLibraries.
/usr/libexec/PlistBuddy -c 'Print :OSBundleLibraries' "$K/Contents/Info.plist" 2>/dev/null \
  | awk -F' = ' 'NF == 2 { gsub(/^[ \t]+/, "", $1); print $1 }' > "$TMP/declared"
kmutil libraries -p "$K" --onedef-symbols --multdef-symbols 2>/dev/null \
  | awk '/ in \/System\//{ id = $0; sub(/^.*\.kc: /, "", id); sub(/ .*$/, "", id); print $1, id }' \
  | sort -u > "$TMP/kcsyms"
awk 'NR == FNR { d[$1] = 1; next } ($2 in d) { print $1 }' "$TMP/declared" "$TMP/kcsyms" | sort -u > "$TMP/resolved"
awk 'NR == FNR { d[$1] = 1; next } !($2 in d) { print $1, $2 }' "$TMP/declared" "$TMP/kcsyms" > "$TMP/undeclared"
if [ -s "$TMP/undeclared" ]; then
  echo "symbols found only in bundles missing from OSBundleLibraries:"; sed 's/^/  /' "$TMP/undeclared"
fi
echo "imports: $(wc -l < "$TMP/imports")  resolved: $(wc -l < "$TMP/resolved")"
comm -23 "$TMP/imports" "$TMP/resolved" > "$TMP/unresolved"

if [ $# -gt 0 ]; then
  # Requested libraries: "id version" per line.
  /usr/libexec/PlistBuddy -c 'Print :OSBundleLibraries' "$K/Contents/Info.plist" 2>/dev/null \
    | awk -F' = ' 'NF == 2 { gsub(/^[ \t]+/, "", $1); print $1, $2 }' > "$TMP/wanted"
  # vercmp A B -> prints -1/0/1 (numeric, dot-separated, missing parts = 0)
  vercmp() {
    awk -v a="$1" -v b="$2" 'BEGIN {
      na = split(a, x, "."); nb = split(b, y, "."); n = na > nb ? na : nb; r = 0
      for (i = 1; i <= n && r == 0; i++) { p = x[i] + 0; q = y[i] + 0; r = p < q ? -1 : (p > q ? 1 : 0) }
      print r }'
  }
  : > "$TMP/libs"
  for arg in "$@"; do
    case "$arg" in
      *.kext) echo "$arg" >> "$TMP/libs" ;;
      *) for b in "$arg"/*.kext; do [ -d "$b" ] && echo "$b" >> "$TMP/libs"; done ;;
    esac
  done
  : > "$TMP/libresolved"
  FAIL=0
  while read -r B; do
    [ -f "$B/Contents/Info.plist" ] || continue
    ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$B/Contents/Info.plist" 2>/dev/null || true)
    [ -n "$ID" ] && [ "$ID" != "$SELF" ] || continue
    WANT=$(awk -v id="$ID" '$1 == id { print $2 }' "$TMP/wanted")
    [ -n "$WANT" ] || continue                      # not a declared dependency: invisible to the linker
    echo "$ID" >> "$TMP/found"
    LEXE=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$B/Contents/Info.plist" 2>/dev/null || true)
    [ -n "$LEXE" ] && [ -f "$B/Contents/MacOS/$LEXE" ] || continue
    CUR=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$B/Contents/Info.plist")
    COMPAT=$(/usr/libexec/PlistBuddy -c 'Print :OSBundleCompatibleVersion' "$B/Contents/Info.plist" 2>/dev/null || echo "")
    if [ -z "$COMPAT" ]; then
      echo "LIBRARY $ID has no OSBundleCompatibleVersion (cannot be linked against)"; FAIL=1; continue
    fi
    if [ "$(vercmp "$WANT" "$COMPAT")" = "-1" ] || [ "$(vercmp "$WANT" "$CUR")" = "1" ]; then
      echo "LIBRARY $ID: requested $WANT, available $CUR (compatible back to $COMPAT)"; FAIL=1; continue
    fi
    nm -gUj "$B/Contents/MacOS/$LEXE" | sort -u > "$TMP/exports"
    comm -12 "$TMP/unresolved" "$TMP/exports" > "$TMP/hit"
    if [ -s "$TMP/hit" ]; then
      echo "  $(wc -l < "$TMP/hit" | tr -d ' ') from $ID $CUR ($B)"
      cat "$TMP/hit" >> "$TMP/libresolved"
    fi
  done < "$TMP/libs"
  # Every declared non-Apple library must have been found somewhere.
  while read -r ID V; do
    case "$ID" in com.apple.*) continue ;; esac
    if ! grep -qx "$ID" "$TMP/found" 2>/dev/null; then
      echo "LIBRARY $ID $V is declared but was not found in: $*"; FAIL=1
    fi
  done < "$TMP/wanted"
  sort -u "$TMP/libresolved" -o "$TMP/libresolved"
  comm -23 "$TMP/unresolved" "$TMP/libresolved" > "$TMP/still"
  mv "$TMP/still" "$TMP/unresolved"
  [ "$FAIL" = 0 ] || { [ -s "$TMP/unresolved" ] && { echo "UNRESOLVED:"; sed 's/^/  /' "$TMP/unresolved"; }; exit 1; }
fi

if [ -s "$TMP/unresolved" ]; then
  echo "UNRESOLVED:"; sed 's/^/  /' "$TMP/unresolved"; exit 1
fi
echo "all symbols resolved"
