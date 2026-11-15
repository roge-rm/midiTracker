#!/bin/sh
# openmpt_compare.sh <list> <outdir> [seconds]: renders each file with the
# harness and with openmpt123 (linear interpolation, no ramping) and prints
# corr's numbers for each. Needs make -C test/host first.
list=$1; dir=$2; secs=${3:-60}
bin=$(dirname "$0")/../../build/host
mkdir -p "$dir"
while IFS= read -r f; do
    [ -z "$f" ] && continue
    case "$f" in \#*) continue ;; esac
    name=$(basename "$f" | tr -c 'A-Za-z0-9.\n-' '_')
    "$bin/render" "$f" "$secs" "$dir/$name.ours.raw" 2>/dev/null || { echo "FAILED  $name"; continue; }
    openmpt123 --batch --stdout --samplerate 44100 --no-float --filter 2 --ramping 0 --end-time "$secs" -q "$f" > "$dir/$name.ref.raw" 2>/dev/null
    printf '%-60s %s\n' "$name" "$("$bin/corr" "$dir/$name.ours.raw" "$dir/$name.ref.raw")"
    rm -f "$dir/$name.ours.raw" "$dir/$name.ref.raw"
done < "$list"
