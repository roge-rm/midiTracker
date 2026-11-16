#!/bin/bash
# mp3_seek_check.sh <render_mp3> <tail_check> <workdir>
# Makes LAME-tagged 44.1 kHz MP3s with ffmpeg in workdir (emptied first),
# seeks each to several points with render_mp3, plays to the end, and
# checks the last quarter second against ffmpeg's decode with tail_check: no
# silent frames and no padding left on. Seeks into the last frames may
# leave no audio, as the first frames after a seek give none.
set -u
RENDER=$1
TAIL=$2
DIR=$3
rm -rf "$DIR"
mkdir -p "$DIR"
FAILS=0
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }
SIG="0.3*sin(2*PI*440*t)+0.15*sin(2*PI*(200+400*t)*t)+0.05*(random(0)-0.5)|0.25*sin(2*PI*660*t+1)+0.2*sin(2*PI*3000*t)+0.05*(random(1)-0.5)"
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=20.3" -c:a libmp3lame -b:a 128k "$DIR/cbr.mp3"
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=17.77" -c:a libmp3lame -q:a 2 "$DIR/vbr.mp3"
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=9.1" -ac 1 -c:a libmp3lame -q:a 5 "$DIR/mono.mp3"
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=13.4" -c:a libmp3lame -b:a 320k "$DIR/c320.mp3"
# The CBR one again with an APE tag and an ID3v1 tag after the frames.
cp "$DIR/cbr.mp3" "$DIR/tags.mp3"
{ printf 'APETAGEX'; head -c 120 /dev/zero; printf 'TAG'; head -c 125 /dev/zero; } >> "$DIR/tags.mp3"
for f in "$DIR"/*.mp3; do
    ch=$(ffprobe -v error -show_entries stream=channels -of csv=p=0 "$f")
    if [ "$ch" = 1 ]; then pan="pan=stereo|c0=c0|c1=c0"; else pan="pan=stereo|c0=c0|c1=c1"; fi
    ff -i "$f" -af "$pan" -f s16le "$f.ref.raw" # ffmpeg's own upmix lowers a mono file 3 dB
    # The length the player gives, the trimmed one.
    ms=$("$RENDER" "$f" /dev/null 2>&1 | sed -n 's/.* of \([0-9]*\) ms, state.*/\1/p')
    # From the start, then seeks spread over the file, two near its end and
    # two into its last frames.
    for pct in start 7 23 38 50 61 77 89 95 97 last120 last250; do
        if [ "$pct" = start ]; then
            log=$("$RENDER" "$f" "$f.raw" 2>&1)
        else
            case "$pct" in last*) at=$((ms - ${pct#last})) ;; *) at=$((ms * pct / 100)) ;; esac
            log=$("$RENDER" "$f" "$f.raw" "$at" 100 2>&1)
        fi
        if [ ! -s "$f.raw" ]; then
            # The frames a seek loses to the bit reservoir can be all that's left.
            case "$pct" in last*) res="ok: no audio left after the seek" ;; *) res="FAIL: no audio" ;; esac
        else
            res=$("$TAIL" "$f.raw" "$f.ref.raw" 11025)
        fi
        case "$pct" in [0-9]*) label="$pct%" ;; *) label=$pct ;; esac
        echo "$(basename "$f") $label: $res"
        case "$res" in ok*) ;; *) FAILS=$((FAILS + 1)); echo "$log" | sed 's/^/  /' ;; esac
    done
done
echo "$FAILS failed"
[ "$FAILS" = 0 ]
