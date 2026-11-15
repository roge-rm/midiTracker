// render <file> <seconds> <out.raw>
// Drives a tracker player the way file_player_mode does (load, play, then
// update() every loop) and writes the stereo int16 frames the WAV ring got.
#include <Arduino.h>
#include <SdFat.h>
#include "synth.h"
#include "shared_ram_arena.h"
#include "mod_file.h"
#include "s3m_file.h"
#include "xm_file.h"
#include <new>
#include <string>
#include <strings.h>

template <class P>
static int run(const char* path, double seconds, FILE* out) {
    static_assert(sizeof(P) <= SharedRamArena::SIZE, "player doesn't fit the arena");
    P* p = new (SharedRamArena::data()) P();
    if (!p->load(path)) {
        fprintf(stderr, "load failed: %s\n", p->errorMessage());
        p->~P();
        return 1;
    }
    p->play();
    size_t want = (size_t)(seconds * 44100.0);
    size_t written = 0;
    unsigned long loops = 0;
    for (;;) {
        // The consumer takes what's in the ring, then the main loop calls update().
        size_t have = HostRing::framesCaptured();
        if (have > written) {
            size_t n = have - written;
            if (written + n > want) n = want - written;
            fwrite(HostRing::data() + written * 2, sizeof(int16_t), n * 2, out);
            written += n;
        }
        HostRing::drain();
        if (written >= want) break;
        if (p->state() != P::STATE_PLAYING) break;
        p->update();
        loops++;
    }
    fprintf(stderr, "%s: %zu frames, %lu updates, %lu reads, state %d ch %d inst %d order %d/%d row %d\n",
            path, written, loops, FsFile::readCalls, (int)p->state(), p->channelCount(), p->instrumentCount(),
            p->patternOrderPosition(), p->songLength(), p->currentRow());
    p->stop();
    p->~P();
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: render <file.mod|s3m|xm> <seconds> <out.raw>\n");
        return 2;
    }
    const char* path = argv[1];
    double seconds = atof(argv[2]);
    FILE* out = fopen(argv[3], "wb");
    if (!out) { perror(argv[3]); return 1; }
    const char* dot = strrchr(path, '.');
    int rc;
    if (dot && strcasecmp(dot, ".mod") == 0) rc = run<ModPlayer>(path, seconds, out);
    else if (dot && strcasecmp(dot, ".s3m") == 0) rc = run<S3mPlayer>(path, seconds, out);
    else if (dot && strcasecmp(dot, ".xm") == 0) rc = run<XmPlayer>(path, seconds, out);
    else { fprintf(stderr, "unknown extension\n"); rc = 2; }
    fclose(out);
    return rc;
}
