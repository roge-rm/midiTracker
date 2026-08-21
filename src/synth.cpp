#include "synth.h"
#include "pins.h"
#include <I2S.h>
#include <math.h>

namespace {

// Cross-core message format, one 32-bit FIFO word per event. The top
// nibble tags the message type; PANIC_MSG/RESET_PROGRAMS_MSG (top nibble
// 0xF) are reserved sentinels and can't collide with a real tag since
// tags below only ever use 0/1.
const uint32_t MSG_TYPE_NOTE = 0x0u;
const uint32_t MSG_TYPE_PROGRAM = 0x1u;
const uint32_t MSG_TYPE_VOLUME = 0x2u;
const uint32_t PANIC_MSG = 0xFFFFFFFFu;
const uint32_t RESET_PROGRAMS_MSG = 0xFFFFFFFEu;

// velocity == 0 means "off" -- the same note-on-with-zero-velocity-is-a-
// note-off convention already used elsewhere in this codebase (see
// MidiOutput::sendNoteOn).
inline uint32_t packNoteMsg(uint8_t channel, uint8_t note, uint8_t velocity) {
    return (MSG_TYPE_NOTE << 28) | ((uint32_t)(channel & 0x0F) << 16) | ((uint32_t)velocity << 8) | note;
}
inline uint32_t packProgramMsg(uint8_t channel, uint8_t program) {
    return (MSG_TYPE_PROGRAM << 28) | ((uint32_t)(channel & 0x0F) << 16) | program;
}
inline uint32_t packVolumeMsg(uint8_t percent) {
    return (MSG_TYPE_VOLUME << 28) | percent;
}

// Set by Synth::begin() on core 0, polled by setup1() on core 1 -- see
// the long comment on Synth::begin() in synth.h for why this exists.
// volatile (not a mutex/atomic) is sufficient: it's written exactly once,
// read in a simple spin loop, and RP2040's two cores share plain SRAM
// with no data cache to create a visibility problem.
volatile bool g_readyForPioInit = false;

// ---- everything below this point runs on core 1 only (setup1()/loop1()
// and whatever they call) -- core 0 only ever touches the pack*Msg()
// helpers above and the FIFO pushes in Synth::noteOn() etc. ----

const int SINE_TABLE_SIZE = 256;
int16_t g_sineTable[SINE_TABLE_SIZE];

// Percussion gets its own dedicated pool rather than sharing one with
// melodic voices: with a 14-25 track file, melodic content alone can
// fill every voice, and the old shared-pool steal-voice-0 fallback would
// then steal drum hits just as readily as anything else -- so drums
// never got a reliable chance to sound. Splitting the pools means a busy
// melodic passage can never steal a drum voice and vice versa. 8 drum
// voices is generous (a full kit hit -- kick+snare+hats+a cymbal -- is
// nowhere near that); 24 melodic voices is a large step up from the
// previous 16-voice shared pool for the "still lots of voice stealing"
// complaint on dense multi-track files. Per-voice CPU cost is trivial
// (Cortex-M0+ has single-cycle multiply, no per-sample division), so the
// real cost of a larger ceiling is mostly just unused headroom, not time.
const int NUM_MELODIC_VOICES = 24;
const int NUM_DRUM_VOICES = 8;
const uint16_t PANIC_RELEASE_SAMPLES = 100; // ~2ms, prompt but click-free
const uint8_t PERCUSSION_CHANNEL = 9; // MIDI channel 10, 0-indexed

// Per-voice headroom. With up to 32 voices now, a literal "all voices at
// max simultaneously" budget would force VOICE_PEAK uncomfortably low
// (quiet sparse passages just to survive a case that's rare in practice).
// Instead: a still-conservative linear budget for the first dozen voices
// (12 * 2048 == 24576, 75% of full scale), plus a soft knee (see
// renderSample()) that gently compresses rather than hard-clips beyond
// that, so an unusually loud/dense moment degrades gracefully instead of
// crackling the way a hard clip does. A power of two so renderSample()
// can scale with a shift instead of a divide -- the Cortex-M0+ cores
// here have no hardware integer divide, and this runs per active voice,
// every single sample.
const int VOICE_PEAK_SHIFT = 11;
const int16_t VOICE_PEAK = 1 << VOICE_PEAK_SHIFT; // 2048
const int32_t SOFT_KNEE_START = 24576; // 75% of full scale
const int32_t HARD_LIMIT = 32000;

enum Waveform : uint8_t { WAVE_SINE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SQUARE, WAVE_NOISE };
enum EnvStage : uint8_t { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Voice {
    bool active = false;
    EnvStage stage = ENV_ATTACK;
    uint8_t note = 0;
    Waveform waveform = WAVE_SINE;

    uint32_t phase = 0;    // 32-bit accumulator, one full cycle == 2^32
    uint32_t phaseInc = 0; // per-sample phase increment for this note's pitch

    int32_t amplitude = 0; // current envelope level
    int32_t envStep = 0;   // per-sample envelope delta for the active stage
    int32_t peakLevel = 0;    // end-of-attack level (velocity-scaled)
    int32_t sustainLevel = 0; // end-of-decay level; 0 for one-shot percussion
    uint16_t decaySamples = 0;
    uint16_t releaseSamples = 0;

    // Percussion-only: extra phase-inc contribution that decays to 0, for
    // a kick/tom pitch-drop ("thump"). 0/0 for everything else.
    int32_t pitchDrop = 0;
    int32_t pitchDropStep = 0;
};

Voice g_melodicVoices[NUM_MELODIC_VOICES];
Voice g_drumVoices[NUM_DRUM_VOICES];
I2S g_i2s(OUTPUT, AUDIO_BCLK, AUDIO_SDATA); // LRCLK is implicitly AUDIO_BCLK+1, see pins.h

// GM program per channel (program / 8 = instrument-family index into
// PRESETS below). Program 0 (Acoustic Grand Piano family) by default,
// matching GM's own default.
uint8_t g_channelProgram[16] = {0};

uint32_t g_noiseState = 0xACE1u; // xorshift32 seed, any nonzero value

// Master volume, 0-100 (see Synth::setVolume()). Written only from loop1()
// in response to a FIFO message, so no cross-core synchronization beyond
// that is needed.
uint8_t g_masterVolume = 80;

// -- WAV playback ring buffer (see Synth::wavStream*() in synth.h) --------
// Single-producer (core 0, via Synth::wavStreamWrite()) / single-consumer
// (core 1, inside renderSample() via popWavFrame()) lock-free ring of
// interleaved stereo int16 frames. head/tail are stored as ever-
// increasing counts rather than pre-masked indices (mask only applied at
// index time) so "empty" (head==tail) and "full" (head-tail==capacity)
// are unambiguous -- the standard SPSC ring shape. Each is written by
// exactly one core and only ever read by the other, same "plain volatile
// is enough, no mutex needed" reasoning as g_readyForPioInit above (RP2040
// shares plain SRAM between cores with no cache-coherency hazard, and
// size_t is a single 32-bit word here, read/written atomically by the
// hardware regardless of `volatile`).
const size_t WAV_RING_FRAMES = 8192; // power of two -- ~186ms at 44.1kHz, see synth.h
const size_t WAV_RING_MASK = WAV_RING_FRAMES - 1;
int16_t g_wavRing[WAV_RING_FRAMES * 2]; // interleaved L,R
volatile size_t g_wavHead = 0; // producer-owned
volatile size_t g_wavTail = 0; // consumer-owned
volatile bool g_wavActive = false; // stream is live -- renderSample() should try to pop
volatile bool g_wavEnded = false;  // no more source data -- empty means "done", not "underrun"
volatile bool g_wavUnderrun = false;
// Temporary real-hardware diagnostic (see xm_file.cpp's own g_xmDiag*
// block, and Synth::wavStreamUnderrunSamples() below) -- g_wavUnderrun
// alone is a check-and-clear boolean, polled once per producer update()
// call; a SUSTAINED drought spanning many consecutive renderSample()
// calls (core 1 runs at a fixed 44.1kHz, hardware-paced by g_i2s.write16()
// blocking) would still only ever read back as a single "yes" if the
// producer isn't polling every single sample, completely hiding how long
// the drought actually lasted. This counts every individual sample
// renderSample() couldn't pop, a real duration/severity measure -- added
// specifically to test whether a busy passage's audible "song slows down"
// symptom is a long, sustained underrun (this counter climbing fast) as
// opposed to something else entirely.
volatile uint32_t g_wavUnderrunSamples = 0;

// Consumer-side pop, called once per sample from renderSample() while
// g_wavActive. Returns false on empty (caller decides whether that means
// "done" or "underrun" via g_wavEnded).
bool popWavFrame(int16_t& outL, int16_t& outR) {
    if (g_wavHead == g_wavTail) return false;
    size_t idx = g_wavTail & WAV_RING_MASK;
    outL = g_wavRing[idx * 2 + 0];
    outR = g_wavRing[idx * 2 + 1];
    g_wavTail++;
    return true;
}

int16_t nextNoise() {
    g_noiseState ^= g_noiseState << 13;
    g_noiseState ^= g_noiseState >> 17;
    g_noiseState ^= g_noiseState << 5;
    return (int16_t)(g_noiseState >> 16);
}

// phase represents one full waveform cycle over its entire 32-bit range;
// the sine table is indexed by the top 8 bits (256 entries), and the
// algorithmic waveforms below use the top 16 bits as a 0..65535 ramp.
int16_t waveformSample(Waveform w, uint32_t phase) {
    uint16_t p16 = (uint16_t)(phase >> 16);
    switch (w) {
        case WAVE_TRIANGLE:
            if (p16 < 32768) return (int16_t)(p16 * 2 - 32768);
            return (int16_t)(32767 - (int32_t)(p16 - 32768) * 2);
        case WAVE_SAW:
            return (int16_t)(p16 - 32768);
        case WAVE_SQUARE:
            return (p16 < 32768) ? (int16_t)30000 : (int16_t)(-30000);
        case WAVE_NOISE:
            return nextNoise();
        default: { // WAVE_SINE
            int tableIdx = (phase >> 24) & (SINE_TABLE_SIZE - 1);
            return g_sineTable[tableIdx];
        }
    }
}

uint32_t hzToPhaseInc(float hz) {
    // Double intermediate: this only runs once per note-on (not per
    // sample), so the extra precision is free, and 2^32 is large enough
    // that float's ~7 significant digits would otherwise round visibly.
    return (uint32_t)((double)hz * 4294967296.0 / SAMPLE_RATE);
}

uint32_t noteToPhaseInc(uint8_t note) {
    float freqHz = 440.0f * powf(2.0f, (note - 69) / 12.0f);
    return hzToPhaseInc(freqHz);
}

// One preset per GM instrument family (program / 8, 0-15) -- not one per
// program, which would need 128 entries for barely more variety than
// broad families already give at this fidelity level. Times are in
// samples at 44.1kHz. decaySamples == 0 skips straight from attack to
// sustain (organ-like, no decay stage).
struct InstrumentPreset {
    Waveform waveform;
    uint16_t attackSamples;
    uint16_t decaySamples;
    uint8_t sustainPercent; // 0-100, fraction of peak held through sustain
    uint16_t releaseSamples;
};

const InstrumentPreset PRESETS[16] = {
    /* 0 Piano             */ {WAVE_TRIANGLE, 50, 4000, 40, 3000},
    /* 1 Chromatic Percus. */ {WAVE_SINE, 20, 3000, 15, 1500},
    /* 2 Organ             */ {WAVE_SQUARE, 10, 0, 100, 500},
    /* 3 Guitar            */ {WAVE_SAW, 30, 3500, 35, 2500},
    /* 4 Bass              */ {WAVE_TRIANGLE, 20, 2000, 70, 2000},
    /* 5 Strings           */ {WAVE_SAW, 800, 0, 90, 4000},
    /* 6 Ensemble          */ {WAVE_SAW, 600, 0, 90, 4000},
    /* 7 Brass             */ {WAVE_SQUARE, 150, 1500, 80, 2000},
    /* 8 Reed              */ {WAVE_SQUARE, 200, 1500, 85, 2000},
    /* 9 Pipe              */ {WAVE_SINE, 300, 1000, 90, 2000},
    /*10 Synth Lead        */ {WAVE_SAW, 20, 1000, 90, 1000},
    /*11 Synth Pad         */ {WAVE_TRIANGLE, 2000, 0, 95, 5000},
    /*12 Synth Effects     */ {WAVE_SINE, 100, 2000, 60, 2000},
    /*13 Ethnic            */ {WAVE_TRIANGLE, 50, 2500, 50, 2000},
    /*14 Percussive        */ {WAVE_SQUARE, 10, 1500, 5, 500},
    /*15 Sound Effects     */ {WAVE_SINE, 50, 2000, 30, 1000},
};

// Basic synthesized drum kit: the GM percussion key map assigns specific
// meanings to note numbers 27-87 on the percussion channel; this buckets
// them into a handful of archetypes rather than modeling all ~50
// individually. Kick/tom/wood block get a short pitch-drop (basePitchHz
// decaying by pitchDropStartHz over pitchDropSamples) -- a slow one for
// kick/tom's "thump", a fast one for wood block's "crack"; everything
// else is white noise shaped only by decay time (short = closed/tight,
// long = open/sustained) since there's no filtering to give real hi-hat/
// cymbal brightness at this fidelity level.
enum DrumType { DRUM_KICK, DRUM_SNARE, DRUM_CLOSED_HAT, DRUM_OPEN_HAT, DRUM_TOM, DRUM_CYMBAL, DRUM_WOODBLOCK, DRUM_DEFAULT, DRUM_TYPE_COUNT };

struct DrumPreset {
    Waveform waveform;
    float basePitchHz;
    uint16_t decaySamples;
    float pitchDropStartHz; // 0 = no pitch sweep
    uint16_t pitchDropSamples;
};

const DrumPreset DRUM_PRESETS[DRUM_TYPE_COUNT] = {
    /*KICK       */ {WAVE_SINE, 60.0f, 7000, 120.0f, 1800},
    /*SNARE      */ {WAVE_NOISE, 0.0f, 5000, 0.0f, 0},
    /*CLOSED_HAT */ {WAVE_NOISE, 0.0f, 1200, 0.0f, 0},
    /*OPEN_HAT   */ {WAVE_NOISE, 0.0f, 10000, 0.0f, 0},
    /*TOM        */ {WAVE_SINE, 120.0f, 8000, 80.0f, 1200},
    /*CYMBAL     */ {WAVE_NOISE, 0.0f, 16000, 0.0f, 0},
    // Only tonal, non-noise preset besides Kick/Tom -- a wood block is a
    // resonant knock, not a noise burst. Bright settle pitch, short decay
    // (shorter than even Closed Hat -- wood doesn't sustain), and a small,
    // fast pitch drop (150 samples vs. Tom's 1200) for the initial "crack"
    // transient rather than Tom's slower pitch-bend "thump".
    /*WOODBLOCK  */ {WAVE_SINE, 900.0f, 900, 300.0f, 150},
    /*DEFAULT    */ {WAVE_NOISE, 0.0f, 3000, 0.0f, 0},
};

DrumType classifyDrum(uint8_t note) {
    switch (note) {
        case 35: case 36: return DRUM_KICK;
        case 37: case 38: case 39: case 40: return DRUM_SNARE;
        case 42: case 44: case 54: case 56: return DRUM_CLOSED_HAT;
        case 46: return DRUM_OPEN_HAT;
        case 41: case 43: case 45: case 47: case 48: case 50: return DRUM_TOM;
        case 49: case 51: case 52: case 53: case 55: case 57: case 59: return DRUM_CYMBAL;
        case 75: case 76: case 77: return DRUM_WOODBLOCK; // Claves, Hi/Lo Wood Block
        default: return DRUM_DEFAULT;
    }
}

void startMelodicVoice(Voice& v, uint8_t channel, uint8_t note, uint8_t velocity) {
    // Retriggering an already-sounding note, or stealing a busy voice
    // (more notes than NUM_MELODIC_VOICES at once), means `v.amplitude`
    // (and `v.phase`) may already be mid-flight here. Ramp amplitude from
    // wherever it actually is, and only reset phase for a genuinely fresh
    // start from silence -- forcing either to a hard value while the
    // other is still "live" is an instant discontinuity in the mixed
    // output, i.e. an audible click, and retriggers are common enough
    // (any repeated note) to pop constantly if this isn't handled.
    int32_t startAmplitude = v.amplitude;
    bool wasActive = v.active;

    const InstrumentPreset& preset = PRESETS[(g_channelProgram[channel] >> 3) & 0x0F];

    v.active = true;
    v.note = note;
    v.waveform = preset.waveform;
    if (!wasActive) v.phase = 0;
    v.phaseInc = noteToPhaseInc(note);
    v.pitchDrop = 0;
    v.pitchDropStep = 0;

    int32_t peak = ((int32_t)VOICE_PEAK * velocity) / 127;
    v.peakLevel = peak;
    v.sustainLevel = (peak * preset.sustainPercent) / 100;
    v.decaySamples = preset.decaySamples;
    v.releaseSamples = preset.releaseSamples;

    v.stage = ENV_ATTACK;
    v.amplitude = startAmplitude;
    uint16_t atk = preset.attackSamples > 0 ? preset.attackSamples : 1;
    v.envStep = (peak - startAmplitude) / atk;
    if (v.envStep == 0) v.envStep = (peak >= startAmplitude) ? 1 : -1;
}

void startPercussionVoice(Voice& v, uint8_t note, uint8_t velocity) {
    const DrumPreset& p = DRUM_PRESETS[classifyDrum(note)];

    v.active = true;
    v.note = note;
    v.waveform = p.waveform;
    v.phase = 0; // drum hits are one-shot triggers, always a fresh start
    v.phaseInc = (p.basePitchHz > 0.0f) ? hzToPhaseInc(p.basePitchHz) : 0;

    v.pitchDrop = (p.pitchDropStartHz > 0.0f) ? (int32_t)hzToPhaseInc(p.pitchDropStartHz) : 0;
    v.pitchDropStep = 0;
    if (v.pitchDrop != 0 && p.pitchDropSamples > 0) {
        v.pitchDropStep = -(v.pitchDrop / (int32_t)p.pitchDropSamples);
        if (v.pitchDropStep == 0) v.pitchDropStep = -1;
    }

    // A modest prominence boost (x1.25, as a shift-friendly add-a-quarter
    // rather than a real multiply): real drum mixes are usually pushed
    // hot specifically so they cut through everything else, and now that
    // percussion has its own guaranteed voices (no more losing the voice
    // to melodic stealing), it's worth actually being audible over a
    // dense 14-25 track arrangement.
    int32_t peak = ((int32_t)VOICE_PEAK * velocity) / 127;
    peak += peak >> 2;
    v.peakLevel = peak;
    v.sustainLevel = 0; // one-shot: decays fully to silence, ignores note-off
    v.decaySamples = p.decaySamples;
    v.releaseSamples = 0; // unused -- see handleNoteOff()

    v.stage = ENV_ATTACK;
    v.amplitude = 0;
    // Fast but not instantaneous attack: real percussion transients are
    // near-instant, but a true single-sample jump to full amplitude is
    // exactly the kind of discontinuity that clicks, especially at high
    // velocity. ~10 samples (0.2ms) is imperceptible as a ramp.
    v.envStep = peak / 10;
    if (v.envStep < 1) v.envStep = 1;
}

void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (channel == PERCUSSION_CHANNEL) {
        for (int i = 0; i < NUM_DRUM_VOICES; i++) {
            if (g_drumVoices[i].active && g_drumVoices[i].note == note) {
                startPercussionVoice(g_drumVoices[i], note, velocity);
                return;
            }
        }
        for (int i = 0; i < NUM_DRUM_VOICES; i++) {
            if (!g_drumVoices[i].active) {
                startPercussionVoice(g_drumVoices[i], note, velocity);
                return;
            }
        }
        startPercussionVoice(g_drumVoices[0], note, velocity);
        return;
    }

    // Retrigger if this note is already sounding, else take a free voice,
    // else steal voice 0. No fancier voice-stealing heuristic (e.g.
    // oldest/quietest) at this scope.
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        if (g_melodicVoices[i].active && g_melodicVoices[i].note == note) {
            startMelodicVoice(g_melodicVoices[i], channel, note, velocity);
            return;
        }
    }
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        if (!g_melodicVoices[i].active) {
            startMelodicVoice(g_melodicVoices[i], channel, note, velocity);
            return;
        }
    }
    startMelodicVoice(g_melodicVoices[0], channel, note, velocity);
}

// Channel-blind by design: MIDI channel isn't tracked per active voice
// (only the note number), so a note-off can't distinguish "channel 1's
// note 60" from "channel 2's note 60" if both happened to sound at once.
// That's a rare, low-stakes edge case for a verification tool; tracking
// channel per voice to close it isn't worth the added complexity here.
// Percussion voices are never searched -- they ignore note-off entirely
// (see startPercussionVoice()'s comment) and live in a separate pool.
void handleNoteOff(uint8_t note) {
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        Voice& v = g_melodicVoices[i];
        if (v.active && v.note == note && v.stage != ENV_RELEASE) {
            v.stage = ENV_RELEASE;
            uint16_t rel = v.releaseSamples > 0 ? v.releaseSamples : 1;
            v.envStep = -(v.amplitude / rel);
            if (v.envStep > -1) v.envStep = -1;
        }
    }
}

void releaseVoice(Voice& v) {
    if (!v.active) return;
    v.stage = ENV_RELEASE;
    v.envStep = -(v.amplitude / PANIC_RELEASE_SAMPLES);
    if (v.envStep > -1) v.envStep = -1;
}

void handlePanic() {
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) releaseVoice(g_melodicVoices[i]);
    for (int i = 0; i < NUM_DRUM_VOICES; i++) releaseVoice(g_drumVoices[i]);
}

// Renders one voice's current sample and advances its phase/envelope by
// one sample. Returns its contribution to the mix (0 if inactive). Shared
// by both voice pools so the envelope state machine only exists once.
int32_t renderVoice(Voice& v) {
    if (!v.active) return 0;

    uint32_t effectivePhaseInc = v.phaseInc + (v.pitchDrop > 0 ? (uint32_t)v.pitchDrop : 0);
    int16_t raw = waveformSample(v.waveform, v.phase);
    int32_t contribution = ((int32_t)raw * v.amplitude) >> VOICE_PEAK_SHIFT;
    v.phase += effectivePhaseInc;

    if (v.pitchDropStep != 0) {
        v.pitchDrop += v.pitchDropStep;
        if (v.pitchDrop < 0) { v.pitchDrop = 0; v.pitchDropStep = 0; }
    }

    v.amplitude += v.envStep;
    switch (v.stage) {
        case ENV_ATTACK:
            if ((v.envStep >= 0 && v.amplitude >= v.peakLevel) ||
                (v.envStep < 0 && v.amplitude <= v.peakLevel)) {
                v.amplitude = v.peakLevel;
                if (v.decaySamples == 0) {
                    v.amplitude = v.sustainLevel;
                    if (v.sustainLevel <= 0) { v.active = false; }
                    else { v.stage = ENV_SUSTAIN; v.envStep = 0; }
                } else {
                    v.stage = ENV_DECAY;
                    int32_t diff = v.sustainLevel - v.peakLevel;
                    v.envStep = diff / (int32_t)v.decaySamples;
                    if (v.envStep == 0) v.envStep = (diff >= 0) ? 1 : -1;
                }
            }
            break;
        case ENV_DECAY:
            if ((v.envStep >= 0 && v.amplitude >= v.sustainLevel) ||
                (v.envStep < 0 && v.amplitude <= v.sustainLevel)) {
                v.amplitude = v.sustainLevel;
                if (v.sustainLevel <= 0) { v.active = false; }
                else { v.stage = ENV_SUSTAIN; v.envStep = 0; }
            }
            break;
        case ENV_SUSTAIN:
            break; // held, envStep == 0
        case ENV_RELEASE:
            if (v.amplitude <= 0) { v.amplitude = 0; v.active = false; }
            break;
    }
    return contribution;
}

// Soft knee above SOFT_KNEE_START: an unusually loud/dense mix (many
// voices summing near full scale) compresses gently instead of hard-
// clipping, which is audible as harsh crackling/popping -- exactly what
// too little headroom caused before. Below the knee, level is untouched.
int32_t softLimit(int32_t mix) {
    int32_t sign = (mix < 0) ? -1 : 1;
    int32_t mag = mix * sign;
    if (mag > SOFT_KNEE_START) {
        mag = SOFT_KNEE_START + (mag - SOFT_KNEE_START) / 4;
    }
    if (mag > HARD_LIMIT) mag = HARD_LIMIT;
    return mag * sign;
}

// Fills outLeft/outRight with this sample's mixed output. Synth voices
// contribute identically to both channels (they were never stereo to
// begin with), so mixL/mixR only actually diverge once a WAV stream
// (genuinely stereo -- see popWavFrame()) is active; softLimit()/volume
// then apply per-channel, same cost shape as the old single-channel path
// (still one divide per channel per sample, not per voice).
void renderSample(int16_t& outLeft, int16_t& outRight) {
    int32_t mix = 0;
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) mix += renderVoice(g_melodicVoices[i]);
    for (int i = 0; i < NUM_DRUM_VOICES; i++) mix += renderVoice(g_drumVoices[i]);

    int32_t mixL = mix;
    int32_t mixR = mix;

    if (g_wavActive) {
        int16_t wl, wr;
        if (popWavFrame(wl, wr)) {
            mixL += wl;
            mixR += wr;
        } else if (g_wavEnded) {
            g_wavActive = false; // fully drained after EOF -- stop checking every sample
        } else {
            g_wavUnderrun = true;
            g_wavUnderrunSamples++;
        }
    }

    int32_t limitedL = softLimit(mixL);
    int32_t limitedR = softLimit(mixR);
    // A single divide per channel per sample (not per voice) is cheap
    // even without hardware integer divide, so this doesn't need the
    // shift-friendly treatment renderVoice()'s per-voice envelope scaling
    // gets.
    outLeft = (int16_t)((limitedL * (int32_t)g_masterVolume) / 100);
    outRight = (int16_t)((limitedR * (int32_t)g_masterVolume) / 100);
}

} // namespace

void Synth::begin() {
    g_readyForPioInit = true;
}

void Synth::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0) { noteOff(note); return; }
    // Blocking, not push_nb(): a big chord landing on the same tick across
    // many tracks can generate more note-on messages than the hardware
    // FIFO's 8-word depth in one burst, and a dropped push here means a
    // silently missing note -- directly against "let them ring out". Core
    // 1 drains this FIFO every single audio sample (~23us at 44.1kHz), so
    // core 0 blocking here is a brief, bounded wait, not a real stall.
    rp2040.fifo.push(packNoteMsg(channel, note, velocity));
}

void Synth::noteOff(uint8_t note) {
    rp2040.fifo.push(packNoteMsg(0, note, 0));
}

void Synth::programChange(uint8_t channel, uint8_t program) {
    rp2040.fifo.push(packProgramMsg(channel, program));
}

void Synth::resetPrograms() {
    rp2040.fifo.push(RESET_PROGRAMS_MSG);
}

void Synth::allNotesOff() {
    rp2040.fifo.push(PANIC_MSG);
}

void Synth::setVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    rp2040.fifo.push(packVolumeMsg(percent));
}

// -- WAV playback stream (see synth.h) -- called from core 0 (WavPlayer);
// g_wav* are the same anonymous-namespace globals renderSample() reads on
// core 1, both cores sharing one address space/binary as usual on RP2040.

void Synth::wavStreamReset() {
    g_wavActive = false; // set first -- consumer stops looking before the buffer moves under it
    g_wavHead = 0;
    g_wavTail = 0;
    g_wavEnded = false;
    g_wavUnderrun = false;
}

void Synth::wavStreamSetActive(bool active) {
    g_wavActive = active;
}

size_t Synth::wavStreamWrite(const int16_t* frames, size_t count) {
    size_t free = WAV_RING_FRAMES - (g_wavHead - g_wavTail);
    size_t n = count < free ? count : free;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (g_wavHead + i) & WAV_RING_MASK;
        g_wavRing[idx * 2 + 0] = frames[i * 2 + 0];
        g_wavRing[idx * 2 + 1] = frames[i * 2 + 1];
    }
    g_wavHead += n;
    return n;
}

size_t Synth::wavStreamFree() {
    return WAV_RING_FRAMES - (g_wavHead - g_wavTail);
}

void Synth::wavStreamEnd() {
    g_wavEnded = true;
}

bool Synth::wavStreamTookUnderrun() {
    bool v = g_wavUnderrun;
    g_wavUnderrun = false;
    return v;
}

uint32_t Synth::wavStreamUnderrunSamples() {
    return g_wavUnderrunSamples;
}

// Dual-core Arduino entry points: the earlephilhower core calls these on
// core 1 automatically (weak symbols, picked up because they're defined
// here) essentially in parallel with setup()/loop() on core 0 -- hence
// the wait below, see Synth::begin()'s comment in synth.h.
void setup1() {
    while (!g_readyForPioInit) {
        // Busy-wait for core 0 to finish claiming PIO resources for the SD
        // card/display before this core claims one for I2S.
    }

    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        g_sineTable[i] = (int16_t)(32000.0f * sinf(2.0f * (float)M_PI * i / SINE_TABLE_SIZE));
    }

    // Larger than the library default (6 buffers x 64 words, ~9ms) to
    // absorb brief core 1 stalls without an audible underrun/pop -- most
    // notably, core 0 and core 1 share the same flash XIP bus, so core 0
    // reading MIDI track data off the SD card during playback can briefly
    // starve core 1's code fetches. ~46ms of buffering is still plenty
    // responsive for note timing; trading a bit of latency for resilience
    // is the right call here.
    g_i2s.setBuffers(8, 256);
    g_i2s.setBitsPerSample(16);
    g_i2s.begin(SAMPLE_RATE);
}

void loop1() {
    uint32_t msg;
    while (rp2040.fifo.pop_nb(&msg)) {
        if (msg == PANIC_MSG) {
            handlePanic();
            continue;
        }
        if (msg == RESET_PROGRAMS_MSG) {
            for (int i = 0; i < 16; i++) g_channelProgram[i] = 0;
            continue;
        }
        uint32_t type = msg >> 28;
        if (type == MSG_TYPE_VOLUME) {
            g_masterVolume = (uint8_t)(msg & 0xFF);
            continue;
        }
        uint8_t channel = (msg >> 16) & 0x0F;
        if (type == MSG_TYPE_PROGRAM) {
            g_channelProgram[channel] = (uint8_t)(msg & 0xFF);
            continue;
        }
        uint8_t note = msg & 0xFF;
        uint8_t velocity = (msg >> 8) & 0xFF;
        if (velocity > 0) handleNoteOn(channel, note, velocity);
        else handleNoteOff(note);
    }
    int16_t left, right;
    renderSample(left, right);
    g_i2s.write16(left, right); // blocks until DMA buffer space is free, paces this loop
}
