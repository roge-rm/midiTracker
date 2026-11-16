// render_wav <file.wav> <out.raw> [seek_ms]
// Drives WavPlayer the way file_player_mode does (load, play, update()
// every loop) and writes the stereo int16 frames the WAV ring got. With
// seek_ms it seeks there first. Prints the read() calls, the elapsed time
// at the end and the final state.
#include <Arduino.h>
#include <SdFat.h>
#include "synth.h"
#include "wav_file.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: render_wav <file.wav> <out.raw> [seek_ms]\n");
        return 2;
    }
    static WavPlayer p;
    if (!p.load(argv[1])) {
        fprintf(stderr, "%s: load failed: %s\n", argv[1], p.errorMessage());
        return 1;
    }
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    if (argc > 3) p.seekTo((uint32_t)atoi(argv[3]));
    p.play();
    size_t written = 0, idle = 0;
    unsigned long loops = 0;
    unsigned long reads0 = FsFile::readCalls;
    while (p.state() == WavPlayer::STATE_PLAYING && idle < 4) {
        p.update();
        loops++;
        size_t have = HostRing::framesCaptured();
        idle = have > written ? 0 : idle + 1;
        fwrite(HostRing::data() + written * 2, sizeof(int16_t), (have - written) * 2, out);
        written = have;
        HostRing::drain(); // the consumer took everything written so far
    }
    p.update(); // lets a drained, ended stream reach DONE
    fclose(out);
    fprintf(stderr, "%s: %zu frames, %lu updates, %lu reads, elapsed %lu of %lu ms, state %d\n", argv[1], written, loops,
            FsFile::readCalls - reads0, (unsigned long)p.elapsedMs(), (unsigned long)p.totalMs(), (int)p.state());
    return 0;
}
