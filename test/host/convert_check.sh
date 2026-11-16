#!/bin/bash
# convert_check.sh <convert_check> <workdir> [mp3dir]
# MIDI files (MIDI_SONGS, default the host build's midi/ copies) and one
# module of each format from trackertunes/ are converted too and compared
# sample by sample with render_midi and render beside convert_check.
# Makes test WAVs and MP3s with ffmpeg in workdir (emptied first), converts
# each to its targets with convert_check, and compares the output with
# ffmpeg's conversion of the same file: header (ours and ffprobe's view),
# correlation per channel and length. Then cancel, card full and free
# names. MP3s in mp3dir are converted too. Every output must line up with
# ffmpeg's (lag 0) and have its length: MP3s through their LAME tags'
# encoder delay and padding. Then the resampler's low-pass: a sweep above
# 22.05 kHz in a 96 kHz file must come out near silent, and one below
# 18 kHz must match ffmpeg's soxr.
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
make_src 48000 a48_24.aiff pcm_s24be 2
make_src 44100 a44_16.aif pcm_s16be 2
make_src 44100 a44_16m.aif pcm_s16be 1
make_src 22050 a22_8m.aiff pcm_s8 1
make_src 44100 a44_sowt.aifc pcm_s16le 2
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
    local diff lag
    diff=$(echo "$cmpl" | sed -n 's/.*(diff \(-\?[0-9]*\)).*/\1/p')
    diff=${diff#-}
    lag=$(echo "$cmpl" | sed -n 's/^lag \(-\?[0-9]*\) frames.*/\1/p')
    [ "$diff" -le 1 ] || bad "$out: length differs by $diff frames"
    case "$src" in
        *clip*) ;; # a repeating signal, the lag search finds another period
        *) [ "$lag" = 0 ] || bad "$out: lag $lag frames" ;;
    esac
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
for f in "$DIR"/a*.aif*; do
    targets=$("$BIN" run "$f" list 2>/dev/null | sed -n 's/^targets://p')
    echo "== $(basename "$f"):$targets"
    n=$(echo "$targets" | grep -o '[0-9](' | wc -l)
    [ "$n" = 2 ] || bad "$f: $n targets, should be 2"
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

echo "== a 44.1k mono MP3 lists WAV only"
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=2" -ac 1 -c:a libmp3lame -b:a 96k "$DIR/m44m.mp3"
targets=$("$BIN" run "$DIR/m44m.mp3" list | sed -n 's/^targets://p')
echo "  targets:$targets"
[ "$targets" = " 0(WAV 16-bit)" ] || bad "m44m.mp3 targets:$targets"
echo "junk" > "$DIR/junk.mp3"
targets=$("$BIN" run "$DIR/junk.mp3" list | sed -n 's/^targets://p')
[ -z "$targets" ] || bad "junk.mp3 targets:$targets"
rm -f "$DIR/junk.mp3"

echo "== no LAME tag: nothing trimmed"
ff -f lavfi -i "aevalsrc='${SIG}':s=44100:d=2" -c:a libmp3lame -b:a 128k -write_xing 0 "$DIR/m44nox.mp3"
log=$("$BIN" run "$DIR/m44nox.mp3" 0)
out=$(echo "$log" | sed -n 's/^out \([^,]*\),.*/\1/p')
frames=$(( ($(stat -c %s "$DIR/$out") - 44) / 4 ))
echo "  $frames frames, a whole number of 1152-frame MP3 frames: $(( frames % 1152 == 0 ))"
[ $((frames % 1152)) = 0 ] && [ "$frames" -gt 88200 ] || bad "m44nox.mp3 gave $frames frames"
rm -f "$DIR/m44nox.mp3" "$DIR/$out"

echo "== aliasing: a 22.5-47 kHz sweep in a 96 kHz file to 44.1k"
ff -f lavfi -i "aevalsrc='0.5*sin(2*PI*(22500*t+12250*t*t/4))':s=96000:d=4" -c:a pcm_s16le "$DIR/a96_high.wav"
log=$("$BIN" run "$DIR/a96_high.wav" 2)
echo "$log" | grep -E "^(out|state)" | sed "s|^|  |"
out=$(echo "$log" | sed -n 's/^out \([^,]*\),.*/\1/p')
# Levels leave out 0.1 s at each end, where the sweep starting and stopping clicks.
ours=$("$BIN" rms "$DIR/$out" 4410 | awk '{print $2}')
ff -i "$DIR/a96_high.wav" -ar 44100 -ac 1 -c:a pcm_s16le "$DIR/a96_high.ff.wav"
ff -i "$DIR/a96_high.wav" -af aresample=44100:resampler=soxr -ac 1 -c:a pcm_s16le "$DIR/a96_high.soxr.wav"
ff -i "$DIR/a96_high.wav" -ac 1 -c:a pcm_s16le "$DIR/a96_high.in.wav"
echo "  input $("$BIN" rms "$DIR/a96_high.in.wav" 9600 | awk '{print $2}') dBFS, ours $ours, ffmpeg $("$BIN" rms "$DIR/a96_high.ff.wav" 4410 | awk '{print $2}'), soxr $("$BIN" rms "$DIR/a96_high.soxr.wav" 4410 | awk '{print $2}')"
awk -v v="$ours" 'BEGIN { exit !(v < -75) }' || bad "aliasing at $ours dBFS"
rm -f "$DIR"/a96_high*

echo "== pass band: a 20 Hz-18 kHz sweep in 96k and 48k files, against soxr"
for r in 96000 48000; do
    ff -f lavfi -i "aevalsrc='0.5*sin(2*PI*(20*t+17980*t*t/8))|0.5*sin(2*PI*(20*t+17980*t*t/8)+1)':s=$r:d=4" -c:a pcm_s24le "$DIR/p$r.wav"
    log=$("$BIN" run "$DIR/p$r.wav" 1)
    out=$(echo "$log" | sed -n 's/^out \([^,]*\),.*/\1/p')
    ff -i "$DIR/p$r.wav" -af aresample=44100:resampler=soxr -f s16le "$DIR/p$r.ref.raw"
    cmpl=$("$BIN" cmp "$DIR/$out" "$DIR/p$r.ref.raw" 44100 2)
    echo "$cmpl" | sed "s|^|  $r: |"
    echo "$cmpl" | awk '/^ch / { if ($4 < 0.999) exit 1 }' || bad "p$r.wav: pass band correlation"
    echo "$cmpl" | grep -q "^lag 0 " || bad "p$r.wav: lag"
    rm -f "$DIR/p$r"*
done

echo "== free names: a second m44.mp3 to WAV is m44-3.wav"
log=$("$BIN" run "$DIR/m44.mp3" 0)
echo "$log" | grep -q "^out m44-3.wav" || bad "free name: $(echo "$log" | grep '^out')"
echo "== m44-4.wav taken mid-convert: m44-5.wav"
log=$("$BIN" run "$DIR/m44.mp3" 0 -1 -1 2)
echo "$log" | grep -E "^(out|took|final|files)" | sed "s|^|  |"
echo "$log" | grep -q "^final m44-5.wav" || bad "taken mid-convert: $(echo "$log" | grep '^final')"
echo "$log" | grep -q "^files ok" || bad "taken mid-convert files"
echo "== path too long"
long="$DIR/$(printf 'd%.0s' $(seq 1 200))"
mkdir -p "$long"
cp "$DIR/m44.mp3" "$long/m44.mp3"
log=$("$BIN" run "$long/m44.mp3" 0)
echo "$log" | grep "^begin" | sed "s|^|  |"
echo "$log" | grep -q "^begin failed: Path too long" || bad "long path: $(echo "$log" | grep '^begin')"
rm -rf "$long"
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

BINDIR=$(dirname "$BIN")
ROOT=$(cd "$BINDIR/../.." && pwd)
MIDI_SONGS=${MIDI_SONGS:-$BINDIR/midi}

# played <src> <ref> <loops>: stereo and mono conversions of a copy of src
# against ref (16-bit stereo, raw or WAV), which must be the same sample
# for sample. Unless the song loops, ref must end where ours does (a MIDI
# ref has a 5 s tail, ours stops when it's silent, so it may be shorter).
played() {
    local src=$1 ref=$2 loops=$3 b log out cmpl t
    b=$(basename "$src")
    for t in 1 2; do
        log=$("$BIN" run "$DIR/$b" "$t" 2>&1)
        echo "$log" | grep -E "^(targets|state|synth)" | sed "s|^|  |"
        echo "$log" | grep -q "^targets: 1(WAV 16-bit 44.1k stereo) 2(WAV 16-bit 44.1k mono)$" || bad "$b targets"
        echo "$log" | grep -q "^state DONE" && echo "$log" | grep -q "^files ok" || { bad "$b target $t didn't finish"; continue; }
        case "$b" in *.mid) echo "$log" | grep -q "^synth after: clean, MIDI out put back, player freed" || bad "$b synth or MIDI out left wrong" ;; esac
        out="$DIR/$(echo "$log" | sed -n 's/^out \([^,]*\),.*/\1/p')"
        if [ "$t" = 1 ]; then cmpl=$("$BIN" same "$out" "$ref"); else cmpl=$("$BIN" same "$out" "$ref" mono); fi
        echo "  $cmpl"
        echo "$cmpl" | grep -q " 0 samples differ" || bad "$b target $t differs from the host render"
        local na nb
        na=$(echo "$cmpl" | sed -n 's/^same: ours \([0-9]*\) ref \([0-9]*\).*/\1/p')
        nb=$(echo "$cmpl" | sed -n 's/^same: ours \([0-9]*\) ref \([0-9]*\).*/\2/p')
        case "$b:$loops" in
            *.mid:*) [ "$na" -le "$nb" ] && [ "$na" -ge $((nb - 5 * 44100)) ] || bad "$b length $na against $nb" ;;
            *:=*) [ "$na" = "${loops#=}" ] || bad "$b length $na, should be ${loops#=}" ;;
            *:1) [ "$na" -lt "$nb" ] || bad "$b didn't stop where it loops" ;;
            *) [ "$na" = "$nb" ] || bad "$b length $na, the render has $nb" ;;
        esac
        rm -f "$out"
    done
    rm -f "$DIR/$b" "$ref"
}

echo "== MIDI through the synth, against render_midi (Line, volume 63)"
n=0
for f in "$MIDI_SONGS"/*.mid; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    echo "== $b"
    cp "$f" "$DIR/$b"
    MT_LEVEL=2 MT_VOLUME=63 "$BINDIR/render_midi" "$DIR/$b" "$DIR/ref.wav" 3000 5 2>/dev/null
    played "$f" "$DIR/ref.wav" 0
    n=$((n + 1))
done
[ "$n" -gt 0 ] || bad "no MIDI files in $MIDI_SONGS"

echo "== a MIDI convert cancelled"
f=$(ls "$MIDI_SONGS"/*.mid | head -1)
cp "$f" "$DIR/c.mid"
log=$("$BIN" run "$DIR/c.mid" 1 -1 3)
echo "$log" | grep -E "^(state|files|synth)" | sed "s|^|  |"
echo "$log" | grep -q "^state CANCELLED" && echo "$log" | grep -q "^files ok" || bad "MIDI cancel"
echo "$log" | grep -q "^synth after: clean, MIDI out put back, player freed" || bad "MIDI cancel left the synth or MIDI out"
rm -f "$DIR/c.mid"

echo "== modules through the trackers, against render"
TT=$ROOT/trackertunes
while IFS='|' read -r f loops; do
    [ -e "$f" ] || { bad "missing $f"; continue; }
    b=$(basename "$f")
    echo "== $b"
    cp "$f" "$DIR/$b"
    "$BINDIR/render" "$DIR/$b" 1200 "$DIR/ref.raw" 2>&1 | sed "s|^.*: |  render: |"
    played "$f" "$DIR/ref.raw" "$loops"
done <<LIST
$TT/various/Ronny Pries - Ping Pong.mod|0
$TT/artists/noxw/Noxw & Svl - Anchorage 4.9.mod|1
$TT/artists/dune/Dune - White Plastic.s3m|0
$TT/artists/madwreck/Madwreck - Can't Wait.xm|0
$TT/various/Jco - Hypnos Fury.it|0
LIST

echo "== a MOD whose last pattern jumps to its own order"
# Order 0 has a pattern loop (E60 at row 4, E62 at row 8) that has to play
# out, order 1 jumps back to itself at row 3 (B01). 64 + 10 + 4 rows of
# 6 ticks of 882 frames, then it ends.
python3 - "$DIR/selfjump.mod" <<'PY'
import sys, struct
cells = [[b'\0' * 16] * 64 for _ in range(2)]
def cell(note, sample, fx, param):
    return bytes([(sample & 0xF0) | (note >> 8), note & 0xFF, ((sample & 15) << 4) | fx, param]) + b'\0' * 12
cells[0][0] = cell(428, 1, 0, 0)
cells[0][4] = cell(0, 0, 0xE, 0x60)
cells[0][8] = cell(0, 0, 0xE, 0x62)
cells[1][0] = cell(320, 1, 0, 0)
cells[1][3] = cell(0, 0, 0xB, 1)
data = bytes([100] * 16 + [156] * 16)
head = b'selfjump'.ljust(20, b'\0')
head += b'square'.ljust(22, b'\0') + struct.pack('>HBBHH', len(data) // 2, 0, 64, 0, len(data) // 2)
head += (b'\0' * 22 + struct.pack('>HBBHH', 0, 0, 0, 0, 1)) * 30
head += bytes([2, 127]) + bytes([0, 1] + [0] * 126) + b'M.K.'
open(sys.argv[1], 'wb').write(head + b''.join(b''.join(p) for p in cells) + data)
PY
"$BINDIR/render" "$DIR/selfjump.mod" 60 "$DIR/ref.raw" 2>&1 | sed "s|^.*: |  render: |"
played "$DIR/selfjump.mod" "$DIR/ref.raw" "=$((78 * 6 * 882))"

echo "== a module that won't load"
echo "junk" > "$DIR/junk.xm"
log=$("$BIN" run "$DIR/junk.xm" 1)
echo "$log" | grep "^begin" | sed "s|^|  |"
echo "$log" | grep -q "^begin failed" || bad "junk.xm began"
ls -a "$DIR" | grep -q '\.cvt$' && bad "junk.xm left a temp"
rm -f "$DIR/junk.xm"

if [ "$FAILS" = 0 ]; then echo "convert-check: all passed"; else echo "convert-check: $FAILS failed"; exit 1; fi
