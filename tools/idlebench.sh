#!/bin/sh
# Compare idle power between AMDZenPM idle modes (needs sudo for the sysctl writes).
# Alternates the configurations ROUNDS times, settles, then averages SAMPLES
# readings 1 s apart, so background-load noise averages out.
#   sudo sh tools/idlebench.sh [ROUNDS] [SAMPLES]
# Restores idle_mode / idle_threshold_us at the end. Keep the machine idle while it runs.
set -eu
ROUNDS=${1:-3}
SAMPLES=${2:-15}
OLDMODE=$(sysctl -n kern.amdzen.idle_mode)
OLDTHR=$(sysctl -n kern.amdzen.idle_threshold_us)
trap 'sysctl -q kern.amdzen.idle_mode=$OLDMODE kern.amdzen.idle_threshold_us=$OLDTHR >/dev/null' EXIT

# "label mode threshold_us"
CONFIGS="hlt 1 $OLDTHR
mwait-c1 2 $OLDTHR
cc6-200us 3 200
cc6-50us 3 50"

TMP=$(mktemp -d /private/tmp/idlebench.XXXXXX)
r=1
while [ "$r" -le "$ROUNDS" ]; do
  echo "$CONFIGS" | while read -r label mode thr; do
    sysctl -q kern.amdzen.idle_mode="$mode" kern.amdzen.idle_threshold_us="$thr" >/dev/null
    sleep 5
    i=0
    while [ "$i" -lt "$SAMPLES" ]; do
      echo "$(sysctl -n kern.amdzen.pkg_power_mw) $(sysctl -n kern.amdzen.cores_power_mw) $(sysctl -n kern.amdzen.busy_permille)" >> "$TMP/$label"
      sleep 1; i=$((i + 1))
    done
    printf 'round %s %-10s done\n' "$r" "$label"
  done
  r=$((r + 1))
done

echo
printf '%-10s %8s %8s %8s %8s %6s\n' config pkg_W median cores_W median busy
echo "$CONFIGS" | while read -r label mode thr; do
  f="$TMP/$label"
  pm=$(cut -d' ' -f1 "$f" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]/1000}')
  cm=$(cut -d' ' -f2 "$f" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]/1000}')
  awk -v l="$label" -v pm="$pm" -v cm="$cm" '{p+=$1; c+=$2; b+=$3} END{printf "%-10s %8.2f %8.2f %8.2f %8.2f %5.1f%%\n", l, p/NR/1000, pm, c/NR/1000, cm, b/NR/10}' "$f"
done
rm -rf "$TMP"
