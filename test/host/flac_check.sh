#!/bin/bash
# flac_check.sh <render_flac> <render_wav> <workdir> [render_flac_ops]
# Makes WAVs with ffmpeg in workdir (emptied first) and the same audio as
# FLAC (ffmpeg, and flac_make.py for 8-bit and verbatim frames), then
# checks that FlacPlayer gives the same frames as WavPlayer gives for the
# WAV: from the start, after a seek before playing and after a seek while
# playing, with and without a seek table. Convert's decode path must equal
# ffmpeg's 16-bit decode of the WAV. Then files it must refuse, and cut,
# corrupt and junk files, which must stop cleanly. With render_flac_ops,
# prints the share of samples on dr_flac's 64-bit prediction path.
set -u
RENDER=$1
WAV=$2
DIR=$3
OPS=${4:-}
MAKE="python3 -I $(cd "$(dirname "$0")" && pwd)/flac_make.py"
rm -rf "$DIR"
mkdir -p "$DIR"
FAILS=0
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
SIG="0.3*sin(2*PI*440*t)+0.15*sin(2*PI*(200+400*t)*t)+0.05*(random(0)-0.5)|0.25*sin(2*PI*660*t+1)+0.2*sin(2*PI*3000*t)+0.05*(random(1)-0.5)"

# same <a.raw> <b.raw> <what>: the same frames, not empty
same() {
    if [ -s "$1" ] && cmp -s "$1" "$2"; then return 0; fi
    bad "$3 ($(stat -c %s "$1" 2>/dev/null) and $(stat -c %s "$2" 2>/dev/null) bytes)"
    return 1
}

# check <name>: name.flac against name.wav
check() {
    local n=$1 f="$DIR/$1.flac" w="$DIR/$1.wav" log
    log=$("$RENDER" "$f" "$DIR/f.raw" 2>&1) || { bad "$n.flac didn't play: $log"; return; }
    echo "ok   $(echo "$log" | head -1 | sed "s|^$DIR/||")"
    "$WAV" "$w" "$DIR/w.raw" 2>/dev/null
    same "$DIR/f.raw" "$DIR/w.raw" "$n.flac differs from the wav"
    "$RENDER" "$f" "$DIR/f.raw" 1700 >/dev/null 2>&1
    "$WAV" "$w" "$DIR/w.raw" 1700 2>/dev/null
    same "$DIR/f.raw" "$DIR/w.raw" "$n.flac differs after a seek before playing"
    log=$("$RENDER" "$f" "$DIR/f.raw" 900 2500 2>&1)
    echo "     $(echo "$log" | grep "seek to" | sed "s|^$DIR/||")"
    "$WAV" "$w" "$DIR/w.raw" 900 2>/dev/null
    same "$DIR/f.raw" "$DIR/w.raw" "$n.flac differs after a seek while playing"
    "$RENDER" "$f" "$DIR/f.raw" decode >/dev/null 2>&1
    ff -i "$w" -f s16le "$DIR/w.raw"
    same "$DIR/f.raw" "$DIR/w.raw" "$n.flac decode differs from ffmpeg's 16-bit decode of the wav"
    rm -f "$DIR/f.raw" "$DIR/w.raw"
}

# wav <name> <rate> <channels> <codec> <seconds>
wav() { ff -f lavfi -i "aevalsrc='${SIG}':s=$2:d=$5" -ac "$3" -c:a "$4" "$DIR/$1.wav"; }

echo "== ffmpeg's FLACs"
wav f16s 44100 2 pcm_s16le 4 && ff -i "$DIR/f16s.wav" -c:a flac "$DIR/f16s.flac" && check f16s
wav f16m 44100 1 pcm_s16le 4 && ff -i "$DIR/f16m.wav" -c:a flac -frame_size 4096 "$DIR/f16m.flac" && check f16m
cp "$DIR/f16s.wav" "$DIR/f16lp.wav" && ff -i "$DIR/f16lp.wav" -c:a flac -frame_size 4096 -lpc_coeff_precision 12 "$DIR/f16lp.flac" && check f16lp
cp "$DIR/f16s.wav" "$DIR/f16sm.wav" && ff -i "$DIR/f16sm.wav" -c:a flac -frame_size 256 "$DIR/f16sm.flac" && check f16sm
wav f24s 48000 2 pcm_s24le 4 && ff -i "$DIR/f24s.wav" -c:a flac -frame_size 4096 "$DIR/f24s.flac" && check f24s
wav f24m 44100 1 pcm_s24le 4 && ff -i "$DIR/f24m.wav" -c:a flac -frame_size 1152 "$DIR/f24m.flac" && check f24m
wav f96 96000 2 pcm_s24le 4 && ff -i "$DIR/f96.wav" -c:a flac -frame_size 4096 "$DIR/f96.flac" && check f96
wav f96x 96000 2 pcm_s16le 4 && ff -i "$DIR/f96x.wav" -c:a flac -frame_size 4608 "$DIR/f96x.flac" && check f96x

echo "== low rates (shorter chunks)"
wav f4k 4000 1 pcm_s16le 3 && ff -i "$DIR/f4k.wav" -c:a flac -frame_size 4096 "$DIR/f4k.flac" && check f4k
wav f8k 8000 2 pcm_s16le 3 && ff -i "$DIR/f8k.wav" -c:a flac "$DIR/f8k.flac" && check f8k
"$RENDER" "$DIR/f4k.flac" /dev/null 2>&1 | tail -1 | grep -q "state 3" || bad "f4k.flac didn't reach the end"

echo "== with seek tables"
for n in f16s f16m f16sm f24s f96; do
    cp "$DIR/$n.wav" "$DIR/t_$n.wav"
    $MAKE seektable "$DIR/$n.flac" "$DIR/t_$n.flac" 3 >/dev/null || bad "seektable $n"
    check "t_$n"
    "$RENDER" "$DIR/t_$n.flac" /dev/null 2>&1 | head -1 | grep -q "seek points [1-9]" || bad "t_$n.flac: seek table not used"
done

echo "== 8-bit and verbatim frames (flac_make.py)"
wav u8m 22050 1 pcm_u8 3 && $MAKE verbatim "$DIR/u8m.wav" "$DIR/u8m.flac" 1000 && check u8m
wav u8s 44100 2 pcm_u8 3 && $MAKE verbatim "$DIR/u8s.wav" "$DIR/u8s.flac" 4096 && check u8s
wav v16s 32000 2 pcm_s16le 3 && $MAKE verbatim "$DIR/v16s.wav" "$DIR/v16s.flac" 4096 && check v16s
wav v24m 44100 1 pcm_s24le 3 && $MAKE verbatim "$DIR/v24m.wav" "$DIR/v24m.flac" 2000 && check v24m
ffprobe -v error -show_entries stream=bits_per_raw_sample -of csv=p=0 "$DIR/u8s.flac" | grep -q '^8$' || bad "u8s.flac isn't 8-bit to ffprobe"

echo "== no length in STREAMINFO: plays to the end, seeks only to the start"
$MAKE nolength "$DIR/f16s.flac" "$DIR/nolen.flac"
"$RENDER" "$DIR/nolen.flac" "$DIR/f.raw" >/dev/null 2>&1
"$WAV" "$DIR/f16s.wav" "$DIR/w.raw" 2>/dev/null
same "$DIR/f.raw" "$DIR/w.raw" "nolen.flac differs from the wav"
log=$("$RENDER" "$DIR/nolen.flac" "$DIR/f.raw" 1700 2>&1)
echo "  $(echo "$log" | grep "seek to" | sed "s|^$DIR/||")"
same "$DIR/f.raw" "$DIR/w.raw" "nolen.flac: a seek before playing should be ignored"
echo "$log" | grep "seek to" | grep -q " 0 reads" || bad "nolen.flac: the seek read the card"
rm -f "$DIR/f.raw" "$DIR/w.raw"

echo "== an ID3v2 tag before the stream"
cp "$DIR/f16s.wav" "$DIR/id3.wav" && $MAKE id3 "$DIR/f16s.flac" "$DIR/id3.flac" && check id3

echo "== a seek table too big for the arena is left out"
wav big 44100 1 pcm_s16le 40
ff -i "$DIR/big.wav" -c:a flac -frame_size 256 "$DIR/big0.flac"
$MAKE seektable "$DIR/big0.flac" "$DIR/big.flac" 1 | sed "s|^|  |"
log=$("$RENDER" "$DIR/big.flac" /dev/null 2>&1 | head -1)
echo "  ${log#$DIR/}"
echo "$log" | grep -q "table left out" || bad "big.flac kept its seek table"
"$RENDER" "$DIR/big.flac" "$DIR/f.raw" 31000 >/dev/null 2>&1
"$WAV" "$DIR/big.wav" "$DIR/w.raw" 31000 2>/dev/null
same "$DIR/f.raw" "$DIR/w.raw" "big.flac differs after a seek"
rm -f "$DIR"/big* "$DIR/f.raw" "$DIR/w.raw"

echo "== refused"
refuse() { # <file> <message>
    local log
    if log=$("$RENDER" "$1" /dev/null 2>&1); then bad "$(basename "$1") played"; return; fi
    echo "  $(basename "$1"): ${log##*: }"
    echo "$log" | grep -q "load failed: $2" || bad "$(basename "$1"): $log"
}
ff -f lavfi -i "aevalsrc='${SIG}|0.1*sin(2*PI*100*t)':s=44100:d=1" -c:a flac "$DIR/c3.flac"
refuse "$DIR/c3.flac" "more than 2 channels"
$MAKE bits "$DIR/f16s.flac" "$DIR/b32.flac" 32 && refuse "$DIR/b32.flac" "32-bit not supported"
ff -f lavfi -i "aevalsrc='${SIG}':s=96000:d=2" -c:a flac -frame_size 32768 "$DIR/huge.flac"
refuse "$DIR/huge.flac" "blocks too big for RAM"
ff -f lavfi -i "aevalsrc='${SIG}':s=3000:d=1" -c:a flac "$DIR/r3k.flac"
refuse "$DIR/r3k.flac" "sample rate not supported"
echo "junk junk junk junk junk junk junk junk" > "$DIR/junk.flac"
refuse "$DIR/junk.flac" "not a FLAC file"
head -c 30 "$DIR/f16s.flac" > "$DIR/head.flac"
refuse "$DIR/head.flac" "not a FLAC file"
: > "$DIR/empty.flac"
refuse "$DIR/empty.flac" "not a FLAC file"

echo "== cut and corrupt files stop cleanly"
full=$("$RENDER" "$DIR/f16s.flac" /dev/null 2>&1 | sed -n 's/.*: \([0-9]*\) frames,.*/\1/p')
ends() { # <file> <min permille of full> <max permille>
    local log n st
    log=$("$RENDER" "$1" "$DIR/f.raw" "${@:4}" 2>&1)
    n=$(echo "$log" | sed -n 's/.*: \([0-9]*\) frames,.*/\1/p')
    st=$(echo "$log" | sed -n 's/.*state \([0-9]\).*/\1/p' | tail -1)
    echo "  $(basename "$1") ${*:4}: $n of $full frames, state $st"
    [ "$st" = 3 ] || bad "$(basename "$1"): state $st, should be 3 (done)"
    [ -n "$n" ] && [ $((n * 1000)) -ge $((full * $2)) ] && [ $((n * 1000)) -le $((full * $3)) ] || bad "$(basename "$1"): $n frames"
}
$MAKE cut "$DIR/f16s.flac" "$DIR/cut.flac" 600
ends "$DIR/cut.flac" 500 620
ends "$DIR/cut.flac" 0 200 3500
$MAKE corrupt "$DIR/f16s.flac" "$DIR/bad.flac"
ends "$DIR/bad.flac" 800 1000
ends "$DIR/bad.flac" 450 625 1500
$MAKE badfirst "$DIR/f16s.flac" "$DIR/badfirst.flac"
ends "$DIR/badfirst.flac" 900 1000
ends "$DIR/badfirst.flac" 450 625 1500
rm -f "$DIR/f.raw"

if [ -n "$OPS" ]; then
    echo "== prediction paths (dr_flac uses 64-bit sums when bits + coefficient precision + log2(order) > 32)"
    for n in f16s f16lp f16sm f24s v16s; do
        "$OPS" "$DIR/$n.flac" /dev/null 2>&1 | grep "residual samples" | sed "s|^$DIR/|  |"
    done
fi

rm -rf "$DIR"
if [ "$FAILS" = 0 ]; then echo "flac-check: all passed"; else echo "flac-check: $FAILS failed"; exit 1; fi
