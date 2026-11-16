#!/bin/bash
# convert_check.sh <convert_check> <workdir> [mp3dir]
# Makes test WAVs and MP3s with ffmpeg in workdir (emptied first), converts
# each to its targets with convert_check, and compares the output with
# ffmpeg's conversion of the same file: header (ours and ffprobe's view),
# correlation per channel and length. Then cancel, card full and free
# names. MP3s in mp3dir are converted too.
set -u
BIN=$1
DIR=$2
MP3DIR=${3:-}
rm -rf "$DIR"
mkdir -p "$DIR"
FAILS=0
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# 12 s, different on each side: tones, a sweep, a beat and some noise.
SIG="0.3*sin(2*PI*440*t)+0.15*sin(2*PI*(200+400*t)*t)*sin(2*PI*0.7*t)+0.05*(random(0)-0.5)|0.25*sin(2*PI*660*t+1)+0.2*sin(2*PI*3000*t)*lt(mod(t,0.5),0.1)+0.05*(random(1)-0.5)"
make_src() { # rate name codec channels
    ff -f lavfi -i "aevalsrc='${SIG}':s=$1:d=12" -ac "$4" -c:a "$3" "$DIR/$2"
}
make_src 48000 w48_24.wav pcm_s24le 2
make_src 96000 w96_f32.wav pcm_f32le 2
make_src 22050 w22_u8m.wav pcm_u8 1
make_src 44100 w44_16.wav pcm_s16le 2
make_src 44100 w44_16m.wav pcm_s16le 1
make_src 44100 w44_adpcm.wav adpcm_ima_wav 2
make_src 32000 w32_adpcmm.wav adpcm_ima_wav 1
make_src 44100 w44_s32m.wav pcm_s32le 1
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=12" -c:a libmp3lame -b:a 192k "$DIR/m44.mp3"
ff -f lavfi -i "aevalsrc='${SIG}':s=48000:d=12" -ac 1 -c:a libmp3lame -q:a 4 "$DIR/m48m.mp3"
ff -f lavfi -i "aevalsrc='${SIG}':s=22050:d=12" -c:a libmp3lame -b:a 64k "$DIR/m22.mp3"
if [ -n "$MP3DIR" ] && [ -d "$MP3DIR" ]; then
    for f in "$MP3DIR"/*.mp3; do [ -e "$f" ] && cp "$f" "$DIR/x_$(basename "$f")"; done
fi

# convert <src> <target> -> checks one conversion against ffmpeg
convert() {
    local src=$1 t=$2 base log out rate ch ref
    log=$("$BIN" run "$src" "$t" 2>&1)
    echo "$log" | sed "s|^|  |"
    out=$(echo "$log" | sed -n 's/^out \([^,]*\), \([0-9]*\) Hz \([0-9]\) ch/\1/p')
    rate=$(echo "$log" | sed -n 's/^out [^,]*, \([0-9]*\) Hz.*/\1/p')
    ch=$(echo "$log" | sed -n 's/^out [^,]*, [0-9]* Hz \([0-9]\) ch/\1/p')
    if ! echo "$log" | grep -q "^state DONE" || ! echo "$log" | grep -q "^files ok"; then
        bad "$src target $t didn't finish"
        return
    fi
    out="$DIR/$out"
    ref="$out.ref.raw"
    ff -i "$src" -ar "$rate" -ac "$ch" -f s16le "$ref"
    local probe
    probe=$(ffprobe -v error -show_entries stream=codec_name,sample_rate,channels -of csv=p=0 "$out")
    echo "  ffprobe: $probe"
    [ "$probe" = "pcm_s16le,$rate,$ch" ] || bad "$out: ffprobe says $probe"
    local cmpl
    cmpl=$("$BIN" cmp "$out" "$ref" "$rate" "$ch")
    echo "$cmpl" | sed "s|^|  |"
    echo "$cmpl" | grep -q "^header ok" || bad "$out: header"
    local min=0.995
    case "$src" in
        *adpcm*) min=0.99 ;;
        *clip*) min=0.8 ;; # ffmpeg mixes to mono before it clips, Helix clips each side first
    esac
    echo "$cmpl" | awk -v min="$min" '/^ch / { if ($4 < min) exit 1 }' || bad "$out: correlation under $min"
    local diff
    diff=$(echo "$cmpl" | sed -n 's/.*(diff \(-\?[0-9]*\)).*/\1/p')
    diff=${diff#-}
    local maxd=8
    case "$src" in *.mp3) maxd=4000 ;; esac # ffmpeg drops the encoder delay and padding, Helix doesn't
    [ "$diff" -le "$maxd" ] || bad "$out: length differs by $diff frames"
}

echo "== target lists and names"
"$BIN" names || bad "names"

for f in "$DIR"/w*.wav; do
    targets=$("$BIN" run "$f" list 2>/dev/null | sed -n 's/^targets://p')
    echo "== $(basename "$f"):$targets"
    for t in $(echo "$targets" | grep -o '[0-9](' | tr -d '('); do
        convert "$f" "$t"
    done
done
for f in "$DIR"/m*.mp3 "$DIR"/x_*.mp3; do
    [ -e "$f" ] || continue
    echo "== $(basename "$f")"
    convert "$f" 0
    convert "$f" 2
done

echo "== free names: a second m44.mp3 to WAV is m44-3.wav"
log=$("$BIN" run "$DIR/m44.mp3" 0)
echo "$log" | grep -q "^out m44-3.wav" || bad "free name: $(echo "$log" | grep '^out')"
echo "== w44_16.wav to mono is w44_16-2.wav (the source keeps its name)"
ls "$DIR"/w44_16-2.wav >/dev/null || bad "w44_16-2.wav missing"

echo "== cancel"
before=$(ls "$DIR" | wc -l)
log=$("$BIN" run "$DIR/w96_f32.wav" 1 -1 1)
echo "$log" | sed "s|^|  |"
echo "$log" | grep -q "^state CANCELLED, msg 'Cancelled'" || bad "cancel state"
echo "$log" | grep -q "^files ok" || bad "cancel files"
[ "$(ls "$DIR" | wc -l)" = "$before" ] || bad "cancel left a file"
ls -a "$DIR" | grep -q '\.cvt$' && bad "cancel left a temp"

echo "== card full"
log=$("$BIN" run "$DIR/m44.mp3" 0 100000)
echo "$log" | sed "s|^|  |"
echo "$log" | grep -q "^state FAILED, msg 'Card full or write failed'" || bad "card full state"
echo "$log" | grep -q "^files ok" || bad "card full files"
log=$("$BIN" run "$DIR/w96_f32.wav" 2 30)
echo "$log" | grep -q "^state FAILED, msg 'Card full or write failed'" || bad "card full at the header"
[ "$(ls "$DIR" | wc -l)" = "$before" ] || bad "card full left a file"
ls -a "$DIR" | grep -q '\.cvt$' && bad "card full left a temp"

echo "== not a WAV"
echo "junk" > "$DIR/junk.wav"
log=$("$BIN" run "$DIR/junk.wav" 1)
echo "$log" | sed "s|^|  |"
echo "$log" | grep -q "^targets:$" || bad "junk.wav has targets"
echo "$log" | grep -q "^begin failed" || bad "junk.wav began"
ls -a "$DIR" | grep -q '\.cvt$' && bad "junk left a temp"

if [ "$FAILS" = 0 ]; then echo "convert-check: all passed"; else echo "convert-check: $FAILS failed"; exit 1; fi
