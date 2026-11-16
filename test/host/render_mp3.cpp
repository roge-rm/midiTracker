// render_mp3 <file.mp3> <out.raw> [seekMs atMs]
// Drives Mp3Player the way the profile does (load, play, update() every
// loop), with core 1's core1Push()/core1Decode() run after each update(),
// and writes the stereo int16 frames the WAV ring got at 44.1 kHz. With
// seekMs and atMs it seeks to seekMs once atMs has been written, and the
// output starts again from there, and the clock at the first frame after
// the seek is printed.
// Prints the format, the read() calls and, built with MP3_COUNT_OPS, the
// 32x32->64 multiplies per decoded frame.
#include <Arduino.h>
#include <unistd.h>
#include <SdFat.h>
#include "synth.h"
#include "mp3_file.h"

#ifdef MP3_COUNT_OPS
extern "C" { unsigned long xmp3_mulshiftCount, xmp3_madd64Count; }
#endif

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: render_mp3 <file.mp3> <out.raw> [seekMs atMs]\n");
        return 2;
    }
    static Mp3Player p;
    if (!p.load(argv[1])) {
        fprintf(stderr, "%s: load failed: %s\n", argv[1], p.errorMessage());
        return 1;
    }
    fprintf(stderr, "%s: MPEG%s %lu Hz, %d ch, %lu bps, %s, vbr tag '%s', id3 %lu bytes, %lu ms; decoder %zu bytes, arena %zu bytes\n", argv[1],
            p.mpegVersion() == 25 ? "2.5" : p.mpegVersion() == 2 ? "2" : "1", (unsigned long)p.sampleRate(), p.channels(),
            (unsigned long)p.bitrate(), p.isVbr() ? "VBR" : "CBR", p.vbrTag(), (unsigned long)p.id3Bytes(), (unsigned long)p.totalMs(),
            Mp3Player::decoderBytes(), Mp3Player::arenaBytes());
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    long seekMs = argc > 4 ? atol(argv[3]) : -1, atMs = argc > 4 ? atol(argv[4]) : 0;
    p.play();
    size_t written = 0, idle = 0;
    unsigned long loops = 0;
    unsigned long reads0 = FsFile::readCalls;
    bool seeked = false;
    unsigned long seekLoop = 0;
    // A seek that misses can take a few dozen updates to find a frame.
    while (p.state() == Mp3Player::STATE_PLAYING && idle < 1024) {
        p.update();
        if (Mp3Player::core1Push()) Mp3Player::core1Decode();
        loops++;
        size_t have = HostRing::framesCaptured();
        idle = have > written ? 0 : idle + 1;
        if (seeked && have > written) {
            fprintf(stderr, "%s: first audio after the seek at elapsed %lu ms, %lu updates later\n", argv[1], (unsigned long)p.elapsedMs(),
                    loops - seekLoop);
            seeked = false;
        }
        fwrite(HostRing::data() + written * 2, sizeof(int16_t), (have - written) * 2, out);
        written = have;
        HostRing::drain();
        if (seekMs >= 0 && written >= (size_t)atMs * 44100 / 1000) {
            p.seekTo((uint32_t)seekMs);
            fprintf(stderr, "%s: seek to %ld ms at %zu frames, state %d, elapsed %lu\n", argv[1], seekMs, written, (int)p.state(),
                    (unsigned long)p.elapsedMs());
            fflush(out);
            if (ftruncate(fileno(out), 0) == 0) rewind(out); // the output starts at the seek
            seekMs = -1;
            seeked = true;
            seekLoop = loops;
        }
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
