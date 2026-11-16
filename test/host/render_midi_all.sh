#!/bin/sh
# render_midi_all.sh <render_midi> <mididir> <outdir>: renders every .mid in mididir to <outdir>/<name>.wav
# and prints each file's peak, clip count and RMS (SECONDS caps the length, default 60).
set -e
render=$1; src=$2; dir=$3
mkdir -p "$dir"
rm -f "$dir"/*.wav "$dir"/log.txt
for f in "$src"/*.mid; do
    "$render" "$f" "$dir/$(basename "$f" .mid).wav" "${SECONDS:-60}" 2>>"$dir/log.txt" || echo "FAILED: $f" >>"$dir/log.txt"
done
cat "$dir/log.txt"
