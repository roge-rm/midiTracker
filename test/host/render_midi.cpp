// render_midi <file.mid> <out.wav> [max seconds] [tail seconds]
// Drives MidiPlayer and the real synth the way the device does: update() on
// core 0, then core 1's loop1() for a block of samples. micros() follows the
// rendered sample count. Writes a 16-bit stereo WAV and prints
// peak, clip count and RMS level.
#include <Arduino.h>
#include "synth.h"
#include "midi_file.h"
#include "midi_host.h"
#include "I2S.h"
#include "host_snap.h"
#include <string.h>
#include <math.h>

extern uint32_t g_hostLevelSamples, g_hostLimitSamples, g_hostTankClips, g_hostGovernorSheds;
extern bool g_hostGovernorModel;
extern uint16_t g_hostNoteMask[128];
extern uint8_t g_hostNoteVel[128];
static void le32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void le16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

static bool writeWav(const char* path, const std::vector<int16_t>& d) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return false; }
    uint32_t bytes = (uint32_t)(d.size() * 2);
    fwrite("RIFF", 1, 4, f); le32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 2); le32(f, HOST_SAMPLE_RATE); le32(f, HOST_SAMPLE_RATE * 4); le16(f, 4); le16(f, 16);
    fwrite("data", 1, 4, f); le32(f, bytes);
    fwrite(d.data(), 2, d.size(), f);
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: render_midi <file.mid> <out.wav> [max seconds=120] [tail seconds=3]\n"); return 2; }
    double maxSec = argc > 3 ? atof(argv[3]) : 120, tailSec = argc > 4 ? atof(argv[4]) : 3;
    const size_t BLOCK = 64; // frames of core 1 work per update()
    hostSetFrames(0);
    Synth::begin();
    setup1();
    // MT_LEVEL (0-2), MT_VOLUME (0-100), MT_REVERB (0/1) and MT_REVERB_TYPE (0-2) override the device defaults.
    if (getenv("MT_REVERB")) Synth::setReverbEnabled(atoi(getenv("MT_REVERB")) != 0);
    if (getenv("MT_REVERB_TYPE")) Synth::setReverbType((uint8_t)atoi(getenv("MT_REVERB_TYPE")));
    if (getenv("MT_LEVEL")) Synth::setOutputLevel((uint8_t)atoi(getenv("MT_LEVEL")));
    if (getenv("MT_VOLUME")) Synth::setVolume((uint8_t)atoi(getenv("MT_VOLUME")));
    // Takes the settings in before the song starts (offlineBegin() applies what's
    // queued, offlineEnd() leaves the synth as setup1() does), so the song's own
    // messages reach the synth on the same samples as without them.
    Synth::offlineBegin();
    Synth::offlineEnd();
    // MT_GOVERNOR=1 runs the render governor on a modelled cycle count.
    g_hostGovernorModel = getenv("MT_GOVERNOR") && atoi(getenv("MT_GOVERNOR"));
    MidiPlayer player;
    if (!player.load(argv[1])) { fprintf(stderr, "load failed: %s\n", player.errorMessage()); return 1; }
    player.play();
    size_t maxFrames = (size_t)(maxSec * HOST_SAMPLE_RATE), tailFrames = (size_t)(tailSec * HOST_SAMPLE_RATE);
    size_t endAt = 0; // set once playback is done
    unsigned long updates = 0;
    // SNAP=<seconds> SNAP_OUT=<file>: the note state over the 280 ms before
    // that point, for the player screen renders (look_render).
    static HostSnap::Midi snap;
    int snapNext = -1;
    size_t snapAt = 0;
    if (getenv("SNAP") && getenv("SNAP_OUT")) {
        memcpy(snap.magic, "MSNP", 4);
        snap.format = player.format();
        snap.tracks = player.trackCount();
        snapAt = (size_t)(atof(getenv("SNAP")) * HOST_SAMPLE_RATE);
        snapNext = 0;
    }
    for (;;) {
        size_t have = g_i2sCapture.size() / 2;
        if (have >= maxFrames) break;
        if (endAt && have >= endAt) break;
        hostSetFrames(have);
        player.update();
        updates++;
        if (snapNext >= 0 && have >= snapAt - (size_t)(HostSnap::Midi::FRAMES - 1 - snapNext) * HOST_SAMPLE_RATE * 40 / 1000) {
            HostSnap::MidiFrame& fr = snap.frames[snapNext];
            fr.elapsedMs = player.elapsedMs();
            fr.bpm = player.currentBPM();
            fr.tap = UiTap::g_midi;
            memcpy(fr.noteMask, g_hostNoteMask, sizeof(fr.noteMask));
            memcpy(fr.noteVel, g_hostNoteVel, sizeof(fr.noteVel));
            if (++snapNext == HostSnap::Midi::FRAMES) {
                snapNext = -1;
                FILE* f = fopen(getenv("SNAP_OUT"), "wb");
                if (f) { fwrite(&snap, sizeof(snap), 1, f); fclose(f); }
            }
        }
        if (!endAt && player.state() != MidiPlayer::STATE_PLAYING) endAt = have + tailFrames;
        for (size_t i = 0; i < BLOCK; i++) loop1();
    }
    size_t frames = g_i2sCapture.size() / 2;
    if (frames > maxFrames) g_i2sCapture.resize(maxFrames * 2), frames = maxFrames;
    double sum = 0; int peak = 0; size_t clips = 0;
    for (int16_t s : g_i2sCapture) {
        int a = abs((int)s);
        if (a > peak) peak = a;
        if (a >= 32767) clips++;
        sum += (double)s * s;
    }
    double rms = frames ? sqrt(sum / (frames * 2)) : 0;
    fprintf(stderr, "%s: %.1f s, %lu updates, track end %s, peak %d (%.1f dBFS), clips %zu, rms %.1f dBFS, leveler %u, limit %u, tank clips %u, shed %u\n", argv[1],
            (double)frames / HOST_SAMPLE_RATE, updates, endAt ? "reached" : "not reached", peak,
            peak ? 20 * log10(peak / 32768.0) : -99.0, clips, rms > 0 ? 20 * log10(rms / 32768.0) : -99.0, g_hostLevelSamples, g_hostLimitSamples, g_hostTankClips, g_hostGovernorSheds);
    player.stop();
    return writeWav(argv[2], g_i2sCapture) ? 0 : 1;
}
