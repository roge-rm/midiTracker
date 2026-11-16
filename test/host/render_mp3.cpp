// render_mp3 <file.mp3> <out.raw>
// Drives Mp3Player the way the profile does (load, play, update() every
// loop) and writes the stereo int16 frames the WAV ring got at 44.1 kHz.
// Prints the format, the read() calls and, built with MP3_COUNT_OPS, the
// 32x32->64 multiplies per decoded frame.
#include <Arduino.h>
#include <SdFat.h>
#include "synth.h"
#include "mp3_file.h"

#ifdef MP3_COUNT_OPS
extern "C" { unsigned long xmp3_mulshiftCount, xmp3_madd64Count; }
#endif

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: render_mp3 <file.mp3> <out.raw>\n");
        return 2;
    }
    static Mp3Player p;
    if (!p.load(argv[1])) {
        fprintf(stderr, "%s: load failed: %s\n", argv[1], p.errorMessage());
        return 1;
    }
    fprintf(stderr, "%s: MPEG%s %lu Hz, %d ch, %lu bps, vbr tag '%s', id3 %lu bytes, %lu ms; decoder %zu bytes, arena %zu bytes\n", argv[1],
            p.mpegVersion() == 25 ? "2.5" : p.mpegVersion() == 2 ? "2" : "1", (unsigned long)p.sampleRate(), p.channels(),
            (unsigned long)p.bitrate(), p.vbrTag(), (unsigned long)p.id3Bytes(), (unsigned long)p.totalMs(),
            Mp3Player::decoderBytes(), Mp3Player::arenaBytes());
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    p.play();
    size_t written = 0, idle = 0;
    unsigned long loops = 0;
    unsigned long reads0 = FsFile::readCalls;
    while (p.state() == Mp3Player::STATE_PLAYING && idle < 4) {
        p.update();
        loops++;
        size_t have = HostRing::framesCaptured();
        idle = have > written ? 0 : idle + 1;
        fwrite(HostRing::data() + written * 2, sizeof(int16_t), (have - written) * 2, out);
        written = have;
        HostRing::drain();
    }
    p.update();
    fclose(out);
    fprintf(stderr, "%s: %zu frames, %lu updates, %lu reads, elapsed %lu of %lu ms, state %d\n", argv[1], written, loops,
            FsFile::readCalls - reads0, (unsigned long)p.elapsedMs(), (unsigned long)p.totalMs(), (int)p.state());
#ifdef MP3_COUNT_OPS
    double frames = (double)written * p.sampleRate() / 44100.0 / p.samplesPerFrame();
    fprintf(stderr, "%s: per frame %.0f MULSHIFT32, %.0f MADD64 (%.0f frames)\n", argv[1], xmp3_mulshiftCount / frames,
            xmp3_madd64Count / frames, frames);
#endif
    return 0;
}
