// render_flac <file.flac> <out.raw> [seekMs [atMs]]
// Drives FlacPlayer the way the file player does (load, play, update()
// every loop), with core 1's core1Push()/core1Decode() run after each
// update(), and writes the stereo int16 frames the WAV ring got at
// 44.1 kHz. With seekMs alone it seeks there before playing; with atMs too
// it seeks once atMs has been written, and the output starts again from
// there. Prints the format, the arena use, each seek's card reads and time,
// and, built with FLAC_COUNT_OPS, the residual samples on dr_flac's 32 and
// 64-bit prediction paths and the multiplies per sample.
// render_flac <file.flac> <out.raw> decode   writes decodeNext()'s frames instead (Convert's path),
// in the file's own channels.
#include <Arduino.h>
#include <unistd.h>
#include <time.h>
#include <SdFat.h>
#include "synth.h"
#include "flac_file.h"

#ifdef FLAC_COUNT_OPS
extern "C" { extern unsigned long long flacOps[2][2]; } // [32-bit, 64-bit][samples, multiplies]
#endif

static double nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void seek(FlacPlayer& p, const char* name, long ms, size_t written) {
    unsigned long c0 = FsFile::readCalls, b0 = FsFile::readBytes;
    double t0 = nowMs();
    p.seekTo((uint32_t)ms);
    fprintf(stderr, "%s: seek to %ld ms at %zu frames: state %d, elapsed %lu, %lu reads, %lu bytes, %.2f ms on the host\n", name, ms,
            written, (int)p.state(), (unsigned long)p.elapsedMs(), FsFile::readCalls - c0, FsFile::readBytes - b0, nowMs() - t0);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: render_flac <file.flac> <out.raw> [seekMs [atMs] | decode]\n");
        return 2;
    }
    static FlacPlayer p;
    bool decode = argc > 3 && !strcmp(argv[3], "decode");
    if (!(decode ? p.openOffline(argv[1]) : p.load(argv[1]))) {
        fprintf(stderr, "%s: load failed: %s\n", argv[1], p.errorMessage());
        return 1;
    }
    char fmt[16];
    p.formatText(fmt, sizeof(fmt));
    fprintf(stderr, "%s: %lu Hz, %d ch, %s, max block %u, max frame %lu, %lu ms, seek points %lu%s; arena %zu bytes (ring %zu)\n",
            argv[1], (unsigned long)p.sampleRate(), p.channels(), fmt, p.maxBlock(), (unsigned long)p.maxFrameBytes(),
            (unsigned long)p.totalMs(), (unsigned long)p.seekPoints(), p.seekTableDropped() ? " (table left out)" : "", p.arenaUsed(),
            p.ringBytes());
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    if (decode) {
        const int16_t* pcm;
        uint8_t ch;
        int n;
        size_t frames = 0;
        while ((n = p.decodeNext(pcm, ch)) >= 0) {
            fwrite(pcm, sizeof(int16_t), (size_t)n * ch, out);
            frames += (size_t)n;
        }
        fclose(out);
        uint32_t done, total;
        p.inputProgress(done, total);
        fprintf(stderr, "%s: decoded %zu frames, read failed %d, input %lu of %lu\n", argv[1], frames, (int)p.readFailed(),
                (unsigned long)done, (unsigned long)total);
        p.close();
        return 0;
    }
    long seekMs = argc > 3 ? atol(argv[3]) : -1, atMs = argc > 4 ? atol(argv[4]) : -1;
    if (seekMs >= 0 && atMs < 0) {
        seek(p, argv[1], seekMs, 0);
        seekMs = -1;
    }
    p.play();
    size_t written = 0, idle = 0;
    unsigned long loops = 0;
    unsigned long reads0 = FsFile::readCalls;
    double t0 = nowMs();
    while (p.state() == FlacPlayer::STATE_PLAYING && idle < 1024) {
        p.update();
        if (FlacPlayer::core1Push()) FlacPlayer::core1Decode();
        loops++;
        size_t have = HostRing::framesCaptured();
        idle = have > written ? 0 : idle + 1;
        fwrite(HostRing::data() + written * 2, sizeof(int16_t), (have - written) * 2, out);
        written = have;
        HostRing::drain();
        if (seekMs >= 0 && written >= (size_t)atMs * 44100 / 1000) {
            seek(p, argv[1], seekMs, written);
            fflush(out);
            if (ftruncate(fileno(out), 0) == 0) rewind(out); // the output starts at the seek
            seekMs = -1;
        }
    }
    double ms = nowMs() - t0;
    p.update();
    fclose(out);
    fprintf(stderr, "%s: %zu frames, %lu updates, %lu reads, elapsed %lu of %lu ms, state %d, %.1f ns per output frame on the host\n",
            argv[1], written, loops, FsFile::readCalls - reads0, (unsigned long)p.elapsedMs(), (unsigned long)p.totalMs(),
            (int)p.state(), written ? ms * 1e6 / (double)written : 0.0);
#ifdef FLAC_COUNT_OPS
    unsigned long long s32 = flacOps[0][0], s64 = flacOps[1][0];
    double all = (double)(s32 + s64);
    fprintf(stderr, "%s: residual samples %llu on the 32-bit path, %llu on the 64-bit path (%.0f%%); multiplies per sample %.1f and %.1f\n",
            argv[1], s32, s64, all ? 100.0 * s64 / all : 0.0, s32 ? (double)flacOps[0][1] / s32 : 0.0,
            s64 ? (double)flacOps[1][1] / s64 : 0.0);
#endif
    p.close();
    return 0;
}
