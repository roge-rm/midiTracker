// Stubs for the MIDI render target: sample-count clock, MidiOutput that
// feeds the synth the way midi_output.cpp does, and the synth's I2S capture.
#include <Arduino.h>
#include <string.h>
#include "midi_output.h"
#include "synth.h"
#include "midi_host.h"
#include "ui_tap.h"

HostRp2040 rp2040;
HostSerial Serial;

namespace { uint64_t g_frames = 0; }
#ifdef HOST_REAL_CLOCK
// convert_check: MidiPlayer runs on its offline clock, the converter's step budget on real time.
#include <chrono>
static auto g_start = std::chrono::steady_clock::now();
uint32_t micros() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count();
}
#else
uint32_t micros() { return (uint32_t)(g_frames * 1000000ull / HOST_SAMPLE_RATE); }
#endif
uint32_t millis() { return micros() / 1000; }
void hostSetFrames(uint64_t f) { g_frames = f; }

// Note bookkeeping as midi_output.cpp's sendNoteOn()/sendNoteOff() keep it,
// for the player screen snapshots.
uint16_t g_hostNoteMask[128];
uint8_t g_hostNoteVel[128];

namespace MidiOutput {
// Same routing as midi_output.cpp's sendRaw() for the internal synth.
void sendRaw(uint8_t status, uint8_t d1, uint8_t d2, uint8_t len) {
    uint8_t ch = status & 0x0F;
    if (d1 < 128 && ((status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && d2 == 0))) {
        g_hostNoteMask[d1] &= (uint16_t)~(1u << ch);
    } else if (d1 < 128 && (status & 0xF0) == 0x90) {
        g_hostNoteMask[d1] |= (uint16_t)(1u << ch);
        g_hostNoteVel[d1] = d2;
        UiTap::midiNoteOn(ch, d1, d2);
    }
    switch (status & 0xF0) {
        case 0x80: Synth::noteOff(ch, d1); break;
        case 0x90: Synth::noteOn(ch, d1, d2); break;
        case 0xB0: Synth::controlChange(ch, d1, d2); break;
        case 0xC0: Synth::programChange(ch, d1); break;
        case 0xD0: Synth::channelPressure(ch, d1); break;
        case 0xE0: Synth::pitchBend(ch, (int16_t)((((uint16_t)d2 << 7) | d1) - 8192)); break;
        default: break;
    }
}
void sendSysExRaw(const uint8_t*, size_t) {}
// Routing state only, for convert_check: the stub sends to the synth either way.
static MidiOutTarget g_target = MIDI_OUT_BOTH;
static bool g_audio = false;
void setTarget(MidiOutTarget t) { g_target = t; }
MidiOutTarget getTarget() { return g_target; }
void setAudioOutput(bool on) { g_audio = on; }
bool audioOutputEnabled() { return g_audio; }
uint8_t* sysExScratch() { static uint8_t b[1100]; return b; }
void allNotesOffAllChannels() {
    memset(g_hostNoteMask, 0, sizeof(g_hostNoteMask));
    Synth::allNotesOff();
    for (uint8_t ch = 0; ch < 16; ch++) {
        Synth::controlChange(ch, 120, 0);
        Synth::controlChange(ch, 123, 0);
    }
}
}
