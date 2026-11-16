#!/bin/sh
# fluid_compare.sh [max seconds, default 600]: renders each file in build/host/midi/ with fluidsynth (default-GM.sf2) and
# with render_midi, and prints lengths, the RMS level difference and the 10 ms envelope and onset
# correlations (envcorr). Run `make -C test/host midi-baseline` first to build and copy the files.
# Output is in build/host/fluid/. Our level uses the device's default (Headphone Low), so it reads low.
here=$(cd "$(dirname "$0")" && pwd)
out=$here/../../build/host
secs=${1:-600}
sf=${SOUNDFONT:-/usr/share/sounds/sf2/default-GM.sf2}
mkdir -p "$out/fluid"
make -s -C "$here" midi >/dev/null || exit 1
printf '%-28s %s\n' file "length ours/fluid    level    envelope             onset"
for f in "$out"/midi/*.mid; do
    n=$(basename "$f" .mid)
    "$out/render_midi" "$f" "$out/fluid/$n.ours.wav" "$secs" 2>/dev/null
    timeout 300 fluidsynth -ni -T wav -F "$out/fluid/$n.fluid.wav" -r 44100 "$sf" "$f" >/dev/null 2>&1
    printf '%-28s ' "$n"
    "$out/envcorr" "$out/fluid/$n.ours.wav" "$out/fluid/$n.fluid.wav"
done
