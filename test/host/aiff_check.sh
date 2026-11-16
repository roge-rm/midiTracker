#!/bin/bash
# aiff_check.sh <render_wav> <workdir>
# Makes a WAV and the same audio as AIFF (and AIFF-C) with ffmpeg in workdir
# (emptied first), plays both through render_wav, and checks the AIFF gives
# the same frames as the WAV, from the start and after a seek. Then
# hand-made files (aiff_make.py): SSND before COMM with an offset, an odd
# chunk and junk past the frame count, 12-bit, and big-endian AIFF-C.
set -u
RENDER=$1
DIR=$2
rm -rf "$DIR"
mkdir -p "$DIR"
FAILS=0
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
SIG="0.3*sin(2*PI*440*t)+0.15*sin(2*PI*(200+400*t)*t)+0.05*(random(0)-0.5)|0.25*sin(2*PI*660*t+1)+0.2*sin(2*PI*3000*t)+0.05*(random(1)-0.5)"

# one <name> <rate> <channels> <wav codec> <aiff codec> <ext>
one() {
    local n=$1 rate=$2 ch=$3 wc=$4 ac=$5 ext=$6
    ff -f lavfi -i "aevalsrc='${SIG}':s=$rate:d=3" -ac "$ch" -c:a "$wc" "$DIR/$n.wav"
    ff -i "$DIR/$n.wav" -c:a "$ac" "$DIR/$n.$ext"
    local magic
    magic=$(head -c 12 "$DIR/$n.$ext" | tail -c 4)
    "$RENDER" "$DIR/$n.wav" "$DIR/$n.wav.raw" 2>/dev/null || bad "$n.wav didn't play"
    "$RENDER" "$DIR/$n.$ext" "$DIR/$n.$ext.raw" 2>"$DIR/$n.log" || { bad "$n.$ext didn't play: $(cat "$DIR/$n.log")"; return; }
    if cmp -s "$DIR/$n.wav.raw" "$DIR/$n.$ext.raw" && [ -s "$DIR/$n.wav.raw" ]; then
        echo "ok   $n.$ext ($magic, $(stat -c %s "$DIR/$n.$ext.raw") bytes) = wav"
    else
        bad "$n.$ext differs from the wav"
    fi
    "$RENDER" "$DIR/$n.wav" "$DIR/$n.wav.s.raw" 1700 2>/dev/null
    "$RENDER" "$DIR/$n.$ext" "$DIR/$n.$ext.s.raw" 1700 2>/dev/null
    cmp -s "$DIR/$n.wav.s.raw" "$DIR/$n.$ext.s.raw" && [ -s "$DIR/$n.wav.s.raw" ] || bad "$n.$ext differs after a seek"
    rm -f "$DIR/$n".*.raw "$DIR/$n.wav.raw" "$DIR/$n.log"
}
one a8m 44100 1 pcm_u8 pcm_s8 aiff
one a8s 48000 2 pcm_u8 pcm_s8 aif
one a16m 44100 1 pcm_s16le pcm_s16be aiff
one a16s 44100 2 pcm_s16le pcm_s16be aif
one a16s48 48000 2 pcm_s16le pcm_s16be aiff
one a24s 48000 2 pcm_s24le pcm_s24be aiff
one a24m 44100 1 pcm_s24le pcm_s24be aif
one a32s 44100 2 pcm_s32le pcm_s32be aiff
one c16s 44100 2 pcm_s16le pcm_s16le aifc
one c16m 48000 1 pcm_s16le pcm_s16le aifc
one cf32 48000 2 pcm_f32le pcm_f32be aifc

echo "== odd layouts"
python3 "$(dirname "$0")/aiff_make.py" "$DIR" || bad "aiff_make.py"
for n in x_layout.aiff x_12bit.aiff x_aifc.aifc; do
    b=${n%.*}
    "$RENDER" "$DIR/$b.wav" "$DIR/$b.w.raw" 2>/dev/null
    "$RENDER" "$DIR/$n" "$DIR/$b.a.raw" 2>"$DIR/$b.log" || { bad "$n didn't play: $(cat "$DIR/$b.log")"; continue; }
    cmp -s "$DIR/$b.w.raw" "$DIR/$b.a.raw" && [ -s "$DIR/$b.w.raw" ] || bad "$n differs from the wav"
    "$RENDER" "$DIR/$b.wav" "$DIR/$b.w.raw" 400 2>/dev/null
    "$RENDER" "$DIR/$n" "$DIR/$b.a.raw" 400 2>/dev/null
    cmp -s "$DIR/$b.w.raw" "$DIR/$b.a.raw" && [ -s "$DIR/$b.w.raw" ] || bad "$n differs after a seek"
    echo "ok   $n ($(stat -c %s "$DIR/$b.a.raw") bytes after the seek)"
done
head -c 12 "$DIR/x_aifc.aifc" | tail -c 4 | grep -q AIFC || bad "x_aifc isn't AIFC"

echo "== the header says AIFC with sowt, fl32"
grep -q sowt "$DIR/c16s.aifc" || bad "c16s.aifc isn't sowt"
grep -q -a -E "fl32|FL32" "$DIR/cf32.aifc" || bad "cf32.aifc isn't fl32"

echo "== junk and a short file"
echo "FORM    AIFF" > "$DIR/junk.aif"
if "$RENDER" "$DIR/junk.aif" /dev/null 2>/dev/null; then bad "junk.aif played"; fi
head -c 30 "$DIR/a16s.aif" > "$DIR/cut.aif"
if "$RENDER" "$DIR/cut.aif" /dev/null 2>/dev/null; then bad "cut.aif played"; fi

rm -rf "$DIR"
if [ "$FAILS" = 0 ]; then echo "aiff-check: all passed"; else echo "aiff-check: $FAILS failed"; exit 1; fi
