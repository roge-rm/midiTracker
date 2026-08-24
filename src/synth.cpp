#include "synth.h"
#include "pins.h"
#include <I2S.h>
#include <math.h>
#include <stdlib.h>

namespace {

// Cross-core message format, one 32-bit FIFO word per event. The top
// nibble tags the message type; PANIC_MSG/RESET_PROGRAMS_MSG (top nibble
// 0xF) are reserved sentinels and can't collide with a real tag since
// tags below only ever use 0/1.
const uint32_t MSG_TYPE_NOTE = 0x0u;
const uint32_t MSG_TYPE_PROGRAM = 0x1u;
const uint32_t MSG_TYPE_VOLUME = 0x2u;
const uint32_t MSG_TYPE_OUTPUT_LEVEL = 0x3u;
const uint32_t MSG_TYPE_REVERB_ENABLED = 0x4u;
const uint32_t MSG_TYPE_REVERB_MIX = 0x5u;
const uint32_t MSG_TYPE_REVERB_TYPE = 0x6u;
// Doorbell only -- the actual preset fields are written directly into the
// shared PRESETS[]/DRUM_PRESETS[] arrays by Synth::setInstrumentPreset()/
// setDrumPreset() before this is pushed (see synth.h's long comment on
// why that's safe); this message just tells core 1 which index's derived
// caches (filter alpha, vibrato/tremolo/PWM depth) need recomputing.
const uint32_t MSG_TYPE_PRESET_DIRTY = 0x7u;
const uint32_t MSG_TYPE_DRUM_PRESET_DIRTY = 0x8u;
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
inline uint32_t packOutputLevelMsg(uint8_t level) {
    return (MSG_TYPE_OUTPUT_LEVEL << 28) | level;
}
inline uint32_t packReverbEnabledMsg(bool enabled) {
    return (MSG_TYPE_REVERB_ENABLED << 28) | (enabled ? 1u : 0u);
}
inline uint32_t packReverbMixMsg(uint8_t percent) {
    return (MSG_TYPE_REVERB_MIX << 28) | percent;
}
inline uint32_t packReverbTypeMsg(uint8_t type) {
    return (MSG_TYPE_REVERB_TYPE << 28) | type;
}
inline uint32_t packPresetDirtyMsg(uint8_t family) {
    return (MSG_TYPE_PRESET_DIRTY << 28) | family;
}
inline uint32_t packDrumPresetDirtyMsg(uint8_t drumType) {
    return (MSG_TYPE_DRUM_PRESET_DIRTY << 28) | drumType;
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

// Filled once in setup1() from PRESETS[i].cutoffHz (see cutoffHzToAlphaQ14())
// -- avoids repeating the expf() conversion on every single note-on. The
// drum-kit equivalent (g_drumFilterAlpha) is declared further down, once
// DRUM_TYPE_COUNT exists to size it.
uint32_t g_presetFilterAlpha[16];

// Filled once in setup1() from PRESETS[i].vibratoDepthPercent.
uint32_t g_presetVibratoDepthQ16[16];

// Filled once in setup1() from PRESETS[i].tremoloDepthPercent/
// pwmDepthPercent -- same shared LFO as vibrato above, just modulating
// amplitude (tremolo) or square-wave duty cycle (PWM) instead of pitch.
uint32_t g_presetTremoloDepthQ16[16];
uint32_t g_presetPwmDepthQ16[16];

// Shared vibrato LFO: a single phase accumulator advanced once per sample
// (in renderSample(), not per voice) and read by every melodic voice, each
// scaled by its own preset depth (see Voice::vibratoRange and its use in
// renderVoice()). Deliberate simplification: every voice's vibrato is
// therefore in-phase with every other voice's, rather than each note
// having its own independent LFO phase -- a true per-voice LFO would need
// its own accumulator (and a sine lookup) per voice for a difference
// unlikely to be audible at this fidelity level, whereas this is one
// lookup total per sample no matter how many voices are active.
const float LFO_RATE_HZ = 5.5f; // a natural vocal/instrumental vibrato rate
uint32_t g_lfoPhaseInc = 0; // computed in setup1() from LFO_RATE_HZ
uint32_t g_lfoPhase = 0;

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

// Attenuate-only auto-leveler: catches a *source* that runs consistently
// hot for a whole file/passage (some tracker files vs. others, or a busy
// MIDI passage) rather than the single-sample overs softLimit() already
// handles below. Deliberately one-directional -- it can only ever reduce
// gain below unity, never boost above it -- see the long comment on
// Synth::setVolume() in synth.h for the real-hardware volume scare this
// was built in response to: a wrong time constant here is a "sounds a bit
// off" bug, never a "surprise loud" one, since the absolute ceiling is
// still whatever the existing static taper already established.
//
// g_levelerEnvelope is a plain leaky-integrator follower of the combined
// mix's magnitude, updated every sample (cheap, shift-only) with a
// ~186ms attack and a ~3s release -- slow enough on both ends that it
// tracks "how loud has this passage/file generally been" rather than
// riding individual note transients (that's softLimit()'s job, not this
// one's); a faster attack would make it react like a compressor and risk
// audible pumping in time with the music. g_levelerGainQ16 (the actual
// makeup gain applied) is only
// recomputed every LEVELER_UPDATE_PERIOD samples, since a genuine integer
// divide (no hardware divide on the Cortex-M0+) is unnecessary at full
// 44.1kHz for something tracking loudness over hundreds of ms, not
// individual samples.
// Raised from an original 18000 (~55%) after real-hardware listening found
// tracker files (.mod/.s3m/.xm) sounding noticeably quieter than MIDI --
// not because anything was turned down for them, but because the tracker
// headroom fix (MIX_SHIFT_TYPICAL_CHANNELS in mod_file.cpp/s3m_file.cpp/
// xm_file.cpp) deliberately made a busy tracker mix's raw level hotter,
// so it was sitting above the old 18000 target for long stretches and
// getting constantly ducked back down by this leveler -- while sparser
// MIDI polyphony tripped that threshold far less often. Net effect: the
// leveler was fighting the very fix meant to bring tracker up to match
// MIDI/WAV. 26000 sits just above SOFT_KNEE_START (24576, where softLimit()
// below starts its own per-sample compression) on purpose: normal "loud"
// content is now left to softLimit()'s graceful per-sample compression
// instead of being pre-emptively ducked by this slower, sustained-level
// leveler, which now only steps in for something genuinely, persistently
// excessive -- a passage softLimit() alone is already compressing hard
// and still isn't enough.
const int32_t LEVELER_TARGET = 26000; // ~79% of full scale
const int LEVELER_UPDATE_PERIOD = 256; // samples between gain recomputes (~5.8ms)
int32_t g_levelerEnvelope = 0;
uint32_t g_levelerGainQ16 = 65536; // unity; never set above this -- attenuate-only
int g_levelerUpdateCounter = 0;

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

    // One-pole low-pass filter state (see renderVoice()). filterState
    // tracks the filter's own output history, so it lives per-voice just
    // like phase/amplitude; filterAlpha (Q14, 1..16384 -- see
    // cutoffHzToAlphaQ14()) is copied in from the preset at voice-start
    // and is otherwise constant for the voice's lifetime -- no filter
    // envelope/LFO at this scope, just a fixed per-instrument brightness.
    int32_t filterState = 0;
    uint32_t filterAlpha = 16384;

    // Max phase-inc deviation from the shared vibrato LFO (see g_lfoPhase
    // above and renderVoice() below), computed once at note-on from the
    // note's own phaseInc and the preset's vibratoDepthPercent -- 0 for
    // any preset/voice with no vibrato (all percussion, and several
    // melodic presets), which also skips the per-sample multiply in
    // renderVoice() entirely.
    uint32_t vibratoRange = 0;

    // Tremolo (amplitude) and PWM (square-wave duty cycle) depth, Q16 --
    // both driven by the same shared LFO as vibrato above, just copied
    // straight from the preset's precomputed table at note-start (unlike
    // vibratoRange, these don't scale with the note's own pitch, so no
    // extra per-note-on math is needed). 0 skips the corresponding
    // per-sample work entirely, same as vibratoRange == 0 does.
    uint32_t tremoloDepthQ16 = 0;
    uint32_t pwmDepthQ16 = 0;

    // Stereo pan, 0-255 (0=full left, 128=center, 255=full right) -- same
    // convention/split (L uses 255-pan, R uses pan, both >>8) the S3M/XM/
    // MOD mixers already use for their own per-channel panning, reused
    // here for consistency. Set once at note-start from the MIDI channel
    // (melodic) or fixed center (percussion, see startPercussionVoice()) --
    // not a live-updating CC10 response, just a per-part spread so
    // simultaneous tracks/channels are spatially distinguishable, the same
    // "make different parts distinguishable" goal the filter/vibrato
    // presets already serve.
    int32_t pan = 128;
};

Voice g_melodicVoices[NUM_MELODIC_VOICES];
Voice g_drumVoices[NUM_DRUM_VOICES];
I2S g_i2s(OUTPUT, AUDIO_BCLK, AUDIO_SDATA); // LRCLK is implicitly AUDIO_BCLK+1, see pins.h

// GM program per channel (program / 8 = instrument-family index into
// PRESETS below). Program 0 (Acoustic Grand Piano family) by default,
// matching GM's own default.
uint8_t g_channelProgram[16] = {0};

uint32_t g_noiseState = 0xACE1u; // xorshift32 seed, any nonzero value

// Converts a master-volume percent (0-100, linear on the UI's slider) to a
// Q16 output gain (0..65536) using a cubic audio taper instead of a
// straight linear multiply. Confirmed on real hardware (see the long
// comment above Synth::setVolume() in synth.h): with a plain linear
// percent/100 scale, 0% was correctly silent but 5% was already
// uncomfortably loud -- and not just for the synth voices, for WAV and
// .mod playback too, which share no mixing code with this or with each
// other. The only thing all three share is this final volume-scaling
// step, so a linear fader -- not any one engine's per-sample mixing --
// was the actual cause: human loudness perception is roughly logarithmic,
// so a linear 0-100 fader spends almost its entire *audible* range in the
// first ~10%. A cubic taper (percent^3) is a cheap standard approximation
// of a proper log/audio-taper pot without needing runtime log/pow calls
// in the audio path -- this is only ever computed once per volume change
// (from loop1()'s MSG_TYPE_VOLUME handler), not per sample.
uint32_t volumePercentToGainQ16(uint8_t percent) {
    if (percent == 0) return 0; // exact hard mute, no taper rounding involved
    float frac = percent / 100.0f;
    float gain = frac * frac * frac;
    int32_t q16 = (int32_t)(gain * 65536.0f + 0.5f);
    if (q16 < 1) q16 = 1; // any nonzero percent should be at least faintly audible
    if (q16 > 65536) q16 = 65536;
    return (uint32_t)q16;
}

// Master output gain, Q16 fixed point (0..65536, see volumePercentToGainQ16()
// above). Written only from loop1() in response to a FIFO message (see
// MSG_TYPE_VOLUME below) or setup1()'s initial default, so no cross-core
// synchronization beyond volatile-free plain SRAM sharing is needed, same
// as g_readyForPioInit above.
uint32_t g_masterGainQ16 = 0; // set from the default percent in setup1()

// Output Level: a final, coarse attenuation stage on top of the Volume
// percent above, matching the picoTracker v2 hardware's own reference
// firmware (xiphonics/picoTracker) which offers the same three-way
// "Headphone Low" / "Headphone High" / "Line Level" choice for the exact
// same reason this exists at all -- this board's PCM5102A DAC has no
// gain/volume register of its own (it always outputs its full native
// ~2.1 Vrms line level) and its TPA6139A2 headphone buffer is hardware-
// strapped (a fixed resistor on its GAIN pin) to that chip's *lowest*
// available gain step -- confirmed from the board's own published
// schematic and the TPA6139A2 datasheet's gain table. So there is no
// lower analog gain to reach for; digital attenuation is the only lever
// that exists at all, on this firmware or the original.
//
// The reference firmware implements that attenuation by patching the raw
// PIO instructions that shift the 16-bit sample into a 32-bit I2S frame,
// inserting extra zero-padding bits after the sign bit (see its
// audio_i2s.pio/picoTrackerAudioDriver.cpp) -- a bit-level trick specific
// to its hand-rolled pico-sdk PIO/DMA driver. This firmware instead uses
// the Arduino I2S library, which owns its own PIO program internally with
// no hook for that kind of patch, so replicating it exactly isn't
// practical here. The effect of N bits of that padding is mathematically
// just an attenuation by 2^-N, though, so an equivalent plain right-shift
// in this existing Q16 gain chain reproduces the same audible result
// without touching I2S internals at all. Reverse-engineered from the
// reference firmware's compiled PIO instructions (decoded 3 vs. 1 offset-
// bit immediates between its "HP High"/"Line Level" presets, ~3 bits/
// 18dB apart) rather than sourced from any published spec, so these are a
// reasoned starting point, not a guaranteed match -- like every other
// audio level in this file, verify by ear on real hardware.
enum OutputLevel : uint8_t { OUTPUT_LEVEL_HP_LOW, OUTPUT_LEVEL_HP_HIGH, OUTPUT_LEVEL_LINE, OUTPUT_LEVEL_COUNT };
const uint8_t OUTPUT_LEVEL_SHIFT[OUTPUT_LEVEL_COUNT] = {
    3, // HP Low  (default): -18dB extra cut -- safest, matches the reference firmware's own default
    1, // HP High: -6dB extra cut
    0, // Line Level: no extra cut -- as loud as this firmware's existing (already headphone-safety-
       // tuned) taper/leveler/limiter chain gets; intended for an external line-level input, not
       // direct headphone/earbud listening
};
uint8_t g_outputLevelShift = OUTPUT_LEVEL_SHIFT[OUTPUT_LEVEL_HP_LOW]; // set from the default in setup1()

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
// pulseWidth is only meaningful for WAVE_SQUARE (the duty-cycle threshold,
// 32768 == fixed 50% -- see Voice::pwmDepthQ16 for who modulates it and
// why); every other waveform ignores it.
int16_t waveformSample(Waveform w, uint32_t phase, uint16_t pulseWidth) {
    uint16_t p16 = (uint16_t)(phase >> 16);
    switch (w) {
        case WAVE_TRIANGLE:
            if (p16 < 32768) return (int16_t)(p16 * 2 - 32768);
            return (int16_t)(32767 - (int32_t)(p16 - 32768) * 2);
        case WAVE_SAW:
            return (int16_t)(p16 - 32768);
        case WAVE_SQUARE:
            return (p16 < pulseWidth) ? (int16_t)30000 : (int16_t)(-30000);
        case WAVE_NOISE:
            return nextNoise();
        default: { // WAVE_SINE
            int tableIdx = (phase >> 24) & (SINE_TABLE_SIZE - 1);
            return g_sineTable[tableIdx];
        }
    }
}

// Converts a filter cutoff in Hz to a one-pole low-pass coefficient in
// Q14 fixed point (1..16384, where 16384 == alpha 1.0 == no filtering).
// Standard RC/exponential-smoother derivation: alpha = 1 - e^(-2*pi*fc/fs).
// Only ever called at startup (once per preset, see g_presetFilterAlpha/
// g_drumFilterAlpha below), so the float math and expf() call cost nothing
// at runtime -- renderVoice()'s per-sample filter step is pure integer
// multiply-shift. Q14, not Q16: real-hardware measurement found the
// per-voice render cost blowing well past budget at just 10-11 active
// voices, root-caused to Cortex-M0+ having no hardware 32x32->64 multiply
// -- every "just use int64_t to be safe" in this file was silently
// costing tens of cycles per voice per sample in software-emulated 64-bit
// multiplication. Q14 keeps the worst case (diff up to 65535, alpha up to
// 16384) at ~1.07e9, safely inside int32 range, so the filter step below
// is a single-cycle 32-bit multiply instead.
uint32_t cutoffHzToAlphaQ14(float cutoffHz) {
    float alpha = 1.0f - expf(-2.0f * (float)M_PI * cutoffHz / SAMPLE_RATE);
    int32_t q14 = (int32_t)(alpha * 16384.0f + 0.5f);
    if (q14 < 1) q14 = 1;
    if (q14 > 16384) q14 = 16384;
    return (uint32_t)q14;
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
    float cutoffHz; // one-pole low-pass cutoff; ~18000+ is effectively "off"
                     // (sine/triangle have little content up there anyway)
    float vibratoDepthPercent; // max pitch deviation as % of note freq, via
                                // the shared LFO (0 = no vibrato); ~1% is
                                // roughly +-17 cents, a typical sung/played
                                // vibrato depth
    float tremoloDepthPercent; // max amplitude deviation, via the same
                                // shared LFO (0 = no tremolo) -- e.g. a
                                // rotary-speaker-style wobble for Organ
    float pwmDepthPercent; // WAVE_SQUARE only: how far the duty cycle
                            // swings from 50%, via the same shared LFO
                            // (0 = fixed 50% duty, no PWM) -- ignored for
                            // every other waveform
};

// Release times bumped well past their original 500-5000 (11-113ms) range
// -- a real-hardware measurement (since removed, a temporary diagnostic)
// found peak polyphony sitting at 9/24 melodic, 6/8 drum with zero voice
// steals even on a busy file, so there's plenty of spare voice/CPU
// headroom to let notes actually ring out instead of cutting off the
// instant a note-off arrives -- which is exactly what a
// release that short sounds like at normal note-to-note timing, and not
// how any of these instruments actually behave. Tuned per family by real-
// instrument intuition: mallets/pads (which are *defined* by a long
// natural ring/tail) get the longest, organ/percussive (which realistically
// have almost none) stay short, everything else lands in between.
// tremoloDepthPercent/pwmDepthPercent (new trailing fields): Organ gets a
// deliberately strong tremolo (rotary-speaker/Leslie character) and PWM
// (square wave alone is a static buzz; a slowly sweeping duty cycle is
// what actually makes it read as "alive"); Brass/Reed get a subtler PWM
// for the same reason without the Leslie wobble; Pad/Strings/Ensemble get
// a gentle tremolo alongside their existing vibrato for a bit of shimmer.
// Everything else stays at 0/0 -- not every instrument needs movement on
// top of what the filter/vibrato pass already gave it.
const InstrumentPreset DEFAULT_PRESETS[16] = {
    /* 0 Piano             */ {WAVE_TRIANGLE, 50, 4000, 40, 12000, 6500.0f, 0.0f, 0.0f, 0.0f},
    /* 1 Chromatic Percus. */ {WAVE_SINE, 20, 3000, 15, 30000, 18000.0f, 0.0f, 0.0f, 0.0f},
    /* 2 Organ             */ {WAVE_SQUARE, 10, 0, 100, 800, 9000.0f, 0.0f, 18.0f, 15.0f},
    /* 3 Guitar            */ {WAVE_SAW, 30, 3500, 35, 10000, 5500.0f, 0.5f, 0.0f, 0.0f},
    /* 4 Bass              */ {WAVE_TRIANGLE, 20, 2000, 70, 8000, 1600.0f, 0.0f, 0.0f, 0.0f},
    /* 5 Strings           */ {WAVE_SAW, 800, 0, 90, 12000, 4200.0f, 1.2f, 5.0f, 0.0f},
    /* 6 Ensemble          */ {WAVE_SAW, 600, 0, 90, 12000, 3800.0f, 1.0f, 5.0f, 0.0f},
    /* 7 Brass             */ {WAVE_SQUARE, 150, 1500, 80, 6000, 6000.0f, 0.8f, 0.0f, 10.0f},
    /* 8 Reed              */ {WAVE_SQUARE, 200, 1500, 85, 6000, 5000.0f, 1.0f, 0.0f, 10.0f},
    /* 9 Pipe              */ {WAVE_SINE, 300, 1000, 90, 4500, 18000.0f, 0.6f, 0.0f, 0.0f},
    /*10 Synth Lead        */ {WAVE_SAW, 20, 1000, 90, 5000, 9000.0f, 0.4f, 4.0f, 0.0f},
    /*11 Synth Pad         */ {WAVE_TRIANGLE, 2000, 0, 95, 30000, 3000.0f, 0.3f, 6.0f, 0.0f},
    /*12 Synth Effects     */ {WAVE_SINE, 100, 2000, 60, 6000, 18000.0f, 0.0f, 0.0f, 0.0f},
    /*13 Ethnic            */ {WAVE_TRIANGLE, 50, 2500, 50, 6000, 4000.0f, 1.0f, 0.0f, 0.0f},
    /*14 Percussive        */ {WAVE_SQUARE, 10, 1500, 5, 800, 7000.0f, 0.0f, 0.0f, 0.0f},
    /*15 Sound Effects     */ {WAVE_SINE, 50, 2000, 30, 3000, 18000.0f, 0.0f, 0.0f, 0.0f},
};

// Short display names, in the same order as DEFAULT_PRESETS/PRESETS --
// used by both the pariSynth editor's UI and its .syn bank files (see
// Synth::instrumentFamilyName()). Matches the family comments above.
const char* const INSTRUMENT_FAMILY_NAMES[16] = {
    "Piano", "Chrom.Percus.", "Organ", "Guitar", "Bass", "Strings",
    "Ensemble", "Brass", "Reed", "Pipe", "SynthLead", "SynthPad",
    "SynthFX", "Ethnic", "Percussive", "SoundFX",
};

// Live-editable copy of DEFAULT_PRESETS above -- what note-start actually
// reads (startMelodicVoice(), unchanged by this addition). Initialized
// from DEFAULT_PRESETS in setup1(); mutated only via
// Synth::setInstrumentPreset()/resetInstrumentPresetToDefault(), both of
// which write here directly from core 0 -- see synth.h's long comment on
// why that's safe. Not const.
InstrumentPreset PRESETS[16];

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

uint32_t g_drumFilterAlpha[DRUM_TYPE_COUNT];

struct DrumPreset {
    Waveform waveform;
    float basePitchHz;
    uint16_t decaySamples;
    float pitchDropStartHz; // 0 = no pitch sweep
    uint16_t pitchDropSamples;
    float cutoffHz; // one-pole low-pass cutoff -- the noise-based hits'
                     // only source of spectral brightness (see comment
                     // above DrumType), since decay time alone previously
                     // had to carry closed-hat-vs-cymbal distinctiveness.
};

const DrumPreset DEFAULT_DRUM_PRESETS[DRUM_TYPE_COUNT] = {
    // decaySamples bumped up for Kick/Tom (7000->8500, 8000->9500, ~20%
    // longer) for a fuller low-end body/tail -- see startPercussionVoice()'s
    // comment for the accompanying extra level boost these two get. Pitch
    // left untouched -- more low-end weight, not a different kick/tom tuning.
    /*KICK       */ {WAVE_SINE, 60.0f, 8500, 120.0f, 1800, 2000.0f},
    /*SNARE      */ {WAVE_NOISE, 0.0f, 5000, 0.0f, 0, 5500.0f},
    // Cutoff brought down twice now (12000->7500->5000, 9000->6000->4200)
    // after real-hardware listening still found hats too forward even
    // after the first cut and losing the general drum boost -- raw white
    // noise has equal energy at every frequency, so a high cutoff on it
    // reads as harsh digital hiss rather than a real cymbal's resonant
    // metallic character. Still brighter than Snare (5500), just not by
    // as much as before.
    /*CLOSED_HAT */ {WAVE_NOISE, 0.0f, 1200, 0.0f, 0, 5000.0f},
    /*OPEN_HAT   */ {WAVE_NOISE, 0.0f, 10000, 0.0f, 0, 4200.0f},
    /*TOM        */ {WAVE_SINE, 120.0f, 9500, 80.0f, 1200, 2500.0f},
    /*CYMBAL     */ {WAVE_NOISE, 0.0f, 16000, 0.0f, 0, 15000.0f},
    // Only tonal, non-noise preset besides Kick/Tom -- a wood block is a
    // resonant knock, not a noise burst. Bright settle pitch, short decay
    // (shorter than even Closed Hat -- wood doesn't sustain), and a small,
    // fast pitch drop (150 samples vs. Tom's 1200) for the initial "crack"
    // transient rather than Tom's slower pitch-bend "thump".
    /*WOODBLOCK  */ {WAVE_SINE, 900.0f, 900, 300.0f, 150, 18000.0f},
    /*DEFAULT    */ {WAVE_NOISE, 0.0f, 3000, 0.0f, 0, 6000.0f},
};

// Short display names, same order/meaning as DrumType/DEFAULT_DRUM_PRESETS
// above -- see Synth::drumPresetName().
const char* const DRUM_PRESET_NAMES[DRUM_TYPE_COUNT] = {
    "Kick", "Snare", "ClosedHat", "OpenHat", "Tom", "Cymbal", "Woodblock", "Default",
};

// Live-editable copy of DEFAULT_DRUM_PRESETS above -- see PRESETS[16]'s
// identical comment just above; same pattern, same safety argument.
DrumPreset DRUM_PRESETS[DRUM_TYPE_COUNT];

// Recomputes PRESETS[family]'s derived caches (filter alpha, vibrato/
// tremolo/PWM depth in Q16) from its current field values -- called once
// per family from setup1(), and again for a single family whenever
// Synth::setInstrumentPreset()/resetInstrumentPresetToDefault() pushes a
// MSG_TYPE_PRESET_DIRTY doorbell (see loop1()). cutoffHzToAlphaQ14() is
// declared further up this file (used by setup1() already, before this
// function existed).
void recomputeInstrumentPresetCache(int family) {
    g_presetFilterAlpha[family] = cutoffHzToAlphaQ14(PRESETS[family].cutoffHz);
    g_presetVibratoDepthQ16[family] = (uint32_t)(PRESETS[family].vibratoDepthPercent / 100.0f * 65536.0f + 0.5f);
    g_presetTremoloDepthQ16[family] = (uint32_t)(PRESETS[family].tremoloDepthPercent / 100.0f * 65536.0f + 0.5f);
    g_presetPwmDepthQ16[family] = (uint32_t)(PRESETS[family].pwmDepthPercent / 100.0f * 65536.0f + 0.5f);
}

// Same as recomputeInstrumentPresetCache() above, for one drum type's
// single derived cache value.
void recomputeDrumPresetCache(int drumType) {
    g_drumFilterAlpha[drumType] = cutoffHzToAlphaQ14(DRUM_PRESETS[drumType].cutoffHz);
}

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

    uint8_t family = (g_channelProgram[channel] >> 3) & 0x0F;
    const InstrumentPreset& preset = PRESETS[family];

    v.active = true;
    v.note = note;
    v.waveform = preset.waveform;
    if (!wasActive) { v.phase = 0; v.filterState = 0; }
    v.phaseInc = noteToPhaseInc(note);
    v.pitchDrop = 0;
    v.pitchDropStep = 0;
    v.filterAlpha = g_presetFilterAlpha[family];
    // >>8 extra, on top of the usual >>16, pre-shrinks this for the
    // per-sample runtime multiply in renderVoice() to stay a plain int32
    // multiply instead of needing int64_t -- see that call site's comment.
    // This is a one-time note-start computation (a 64-bit multiply here
    // costs nothing, unlike doing it every sample), so precision is only
    // sacrificed where it doesn't cost anything to keep.
    v.vibratoRange = (uint32_t)(((uint64_t)v.phaseInc * g_presetVibratoDepthQ16[family]) >> 24);
    v.tremoloDepthQ16 = g_presetTremoloDepthQ16[family];
    v.pwmDepthQ16 = g_presetPwmDepthQ16[family];
    // Spread channels 0-15 across the stereo field, but not hard to the
    // extremes (32..224 of the 0..255 range) -- a channel fully isolated
    // to one ear is fatiguing on headphones/earbuds, which is exactly
    // what most listeners here will be using (see this file's whole
    // Volume/Output Level history).
    v.pan = 32 + ((int32_t)channel * 192) / 15;

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
    DrumType drumType = classifyDrum(note);
    const DrumPreset& p = DRUM_PRESETS[drumType];

    v.active = true;
    v.note = note;
    v.waveform = p.waveform;
    v.phase = 0; // drum hits are one-shot triggers, always a fresh start
    v.filterState = 0;
    v.filterAlpha = g_drumFilterAlpha[drumType];
    v.pan = 128; // drums stay centered, matching how real kits are usually mixed
    v.phaseInc = (p.basePitchHz > 0.0f) ? hzToPhaseInc(p.basePitchHz) : 0;

    v.pitchDrop = (p.pitchDropStartHz > 0.0f) ? (int32_t)hzToPhaseInc(p.pitchDropStartHz) : 0;
    v.pitchDropStep = 0;
    if (v.pitchDrop != 0 && p.pitchDropSamples > 0) {
        v.pitchDropStep = -(v.pitchDrop / (int32_t)p.pitchDropSamples);
        if (v.pitchDropStep == 0) v.pitchDropStep = -1;
    }

    // A modest prominence boost (x1.125, shift-friendly rather than a real
    // multiply): real drum mixes are usually pushed hot specifically so
    // they cut through everything else. Originally x1.25, cut back after
    // real-hardware listening found drums sticking out too much once two
    // other, later additions started stacking with this same amplitude
    // boost to the same "make drums cut through" end: the per-drum-type
    // brightness filter (closed hat/cymbal are now genuinely brighter on
    // their own, not just louder -- see DRUM_PRESETS' cutoffHz) and
    // stereo panning (drums stay centered while melodic voices spread out,
    // and centered/mono-summed content reads as more forward than panned
    // content even at equal amplitude). Reduce further (or drop this
    // entirely) if it's still too hot.
    int32_t peak = ((int32_t)VOICE_PEAK * velocity) / 127;
    // Hats skip the general boost below entirely and then get two
    // successive -25% cuts on top of that (~0.56x/-5dB combined): high-
    // frequency noise is already perceptually piercing on its own (equal-
    // loudness perception makes it read as more prominent per unit
    // amplitude than low-frequency content), and real-hardware listening
    // kept finding hats "too forward" through the general-boost removal,
    // two rounds of cutoff cuts (see DRUM_PRESETS), and the first -25%.
    if (drumType == DRUM_CLOSED_HAT || drumType == DRUM_OPEN_HAT) {
        peak -= peak >> 2;
        peak -= peak >> 2;
    } else {
        peak += peak >> 3;
    }
    // Extra low-end "oomph" for Kick/Tom specifically (on top of the
    // general boost above): the only two tonal/bass-register drum types
    // (a sine fundamental + pitch-drop "thump") -- everything else here is
    // noise-based and doesn't have real low-end content to boost. +25% on
    // top of the general +12.5% (~1.4x/+3dB total) rather than a pitch
    // change, so this adds weight/punch without altering their tuning.
    if (drumType == DRUM_KICK || drumType == DRUM_TOM) {
        peak += peak >> 2;
    }
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
void renderVoice(Voice& v, int16_t lfoValue, int32_t& outL, int32_t& outR) {
    if (!v.active) { outL = 0; outR = 0; return; }

    // Signed accumulate: pitchDrop (percussion pitch-sweep) and the shared
    // vibrato LFO both nudge phaseInc, one always down (pitchDrop, never
    // negative itself) and the other bidirectionally. int64_t keeps this
    // safe regardless of sign without needing to reason about where
    // v.phaseInc (uint32_t, can be a large fraction of its range for high
    // notes) sits relative to either adjustment.
    int64_t phaseIncAdj = (int64_t)v.phaseInc;
    if (v.pitchDrop > 0) phaseIncAdj += v.pitchDrop;
    if (v.vibratoRange != 0) {
        // lfoValue is g_sineTable's output, ~-32000..32000 (not a true
        // Q15 -32768..32767) -- close enough for a musical modulation
        // depth that a >>7 shift (instead of the "true" >>15 -- see
        // vibratoRange's own >>24 note-start comment, 8 of the 15 bits
        // already came off there) stands in for a /32768 divide (the
        // Cortex-M0+ has no hardware divide); the ~2% resulting shortfall
        // in actual vs. nominal depth is inaudible. Plain int32 multiply,
        // not int64_t -- vibratoRange was pre-shrunk at note-start
        // specifically so this per-sample multiply doesn't need it (see
        // cutoffHzToAlphaQ14()'s comment for why that matters here).
        phaseIncAdj += ((int32_t)v.vibratoRange * lfoValue) >> 7;
    }
    uint32_t effectivePhaseInc = (uint32_t)phaseIncAdj;

    // PWM: the same shared LFO nudges the square wave's duty cycle around
    // its fixed 50% (32768) center instead of pitch. Only WAVE_SQUARE
    // voices ever have a nonzero pwmDepthQ16 (see PRESETS), so this is a
    // no-op shift+add for every other waveform.
    // Plain int32 multiply, not int64: pwmDepthQ16 is a preset-fixed
    // percentage (max ~15%, see PRESETS), never phaseInc-scaled the way
    // vibratoRange is, so (max ~9830) * (lfoValue up to ~32768) tops out
    // around 3.2e8 -- nowhere near int32 overflow. See cutoffHzToAlphaQ14()'s
    // comment for why this matters on a Cortex-M0+ (no hardware 64-bit
    // multiply).
    uint16_t pulseWidth = 32768;
    if (v.pwmDepthQ16 != 0) {
        int32_t swing = ((int32_t)v.pwmDepthQ16 * lfoValue) >> 15;
        pulseWidth = (uint16_t)(int32_t)(32768 + swing);
    }
    int16_t raw = waveformSample(v.waveform, v.phase, pulseWidth);

    // One-pole low-pass, applied to the raw oscillator before the envelope
    // (VCF-before-VCA, the usual analog-synth order) so the filter's own
    // brightness is independent of where the envelope happens to be. Q14
    // fixed point (see cutoffHzToAlphaQ14()'s comment for why Q14, not
    // Q16) -- plain int32 multiply, no int64_t needed: diff up to 65535 *
    // alpha up to 16384 tops out around 1.07e9, safely inside int32 range.
    int32_t diff = (int32_t)raw - v.filterState;
    v.filterState += (diff * (int32_t)v.filterAlpha) >> 14;

    int32_t contribution = (v.filterState * v.amplitude) >> VOICE_PEAK_SHIFT;
    // Tremolo: same shared LFO again, this time as a gain wobble around
    // unity (65536 Q16) rather than a pitch or duty-cycle nudge. Applied
    // post-envelope so it modulates the note's own current loudness
    // rather than fighting the attack/decay/release shape. tremMod itself
    // is a safe plain int32 multiply (same reasoning as PWM's swing
    // above), but contribution (up to ~32767) times the Q16 gain (up to
    // ~77332 at max depth) can exceed int32 -- contribution is shifted
    // down 2 bits first (negligible precision loss for a modulation
    // effect) to keep this a 32-bit multiply too; the final shift is
    // adjusted from 16 to 14 to compensate.
    if (v.tremoloDepthQ16 != 0) {
        int32_t tremMod = ((int32_t)v.tremoloDepthQ16 * lfoValue) >> 15;
        contribution = ((contribution >> 2) * (65536 + tremMod)) >> 14;
    }
    // Same pan split (255-pan for L, pan for R, both >>8) the S3M/XM/MOD
    // mixers already use -- see Voice::pan's comment.
    outL = (contribution * (255 - v.pan)) >> 8;
    outR = (contribution * v.pan) >> 8;
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

// -- Lo-fi reverb -----------------------------------------------------------
// A small mono Freeverb-style tank (4 parallel comb filters + 2 series
// allpass filters) fed from the onboard MIDI synth's own voices only --
// see renderSample()'s synthMixL/R, captured before WAV/tracker content is
// mixed in -- so a WAV or tracker file playing back never reaches the
// tank, only actual synth notes do. The wet output still gets added into
// the full (WAV-included) mix afterward, since there's nowhere else for
// it to go once everything's combined into one output signal. Comb/
// allpass delay lengths are Freeverb's own well-known values, not
// arbitrary: they're chosen to be mutually non-resonant so the tank
// doesn't ring at an audible pitch the way a naive choice of lengths can.
//
// The wet signal is then deliberately degraded -- held for REVERB_DECIMATE
// samples at a time (a crude sample-rate reduction, ~11kHz effective) and
// bit-masked (a crude bitcrush) -- for an intentionally lo-fi, chiptune-
// appropriate ambience rather than chasing a generic "nice concert hall"
// plugin sound, which felt more true to what the rest of this synth
// already is. Only the wet signal gets this treatment; the dry mix is
// untouched, so it still reads as degraded *ambience*, not a degraded mix.
//
// The tank itself still runs at the full sample rate underneath the held/
// decimated output -- only the read-out is stepped, not the delay lines'
// own timing -- otherwise the decay/diffusion physics would run at the
// decimated rate too and the whole thing would sound wrong, not just lo-fi.
const int REVERB_COMB_COUNT = 4;
const int REVERB_COMB_LEN0 = 1116, REVERB_COMB_LEN1 = 1188, REVERB_COMB_LEN2 = 1277, REVERB_COMB_LEN3 = 1356;
const int REVERB_ALLPASS_LEN0 = 556, REVERB_ALLPASS_LEN1 = 441;

int16_t g_reverbCombBuf0[REVERB_COMB_LEN0];
int16_t g_reverbCombBuf1[REVERB_COMB_LEN1];
int16_t g_reverbCombBuf2[REVERB_COMB_LEN2];
int16_t g_reverbCombBuf3[REVERB_COMB_LEN3];
int16_t g_reverbAllpassBuf0[REVERB_ALLPASS_LEN0];
int16_t g_reverbAllpassBuf1[REVERB_ALLPASS_LEN1];
size_t g_reverbCombPos0 = 0, g_reverbCombPos1 = 0, g_reverbCombPos2 = 0, g_reverbCombPos3 = 0;
size_t g_reverbAllpassPos0 = 0, g_reverbAllpassPos1 = 0;
int32_t g_reverbDampState0 = 0, g_reverbDampState1 = 0, g_reverbDampState2 = 0, g_reverbDampState3 = 0;

// Q16 fixed point. FEEDBACK sets decay time (higher = longer tail);
// DAMP_ALPHA is a one-pole lowpass *inside* each comb's feedback path,
// same technique as the synth voices' own filterAlpha -- real rooms
// absorb high frequencies fastest, so damping the feedback (not the
// output) is what gives the tail a warm, closing-in-on-itself decay
// instead of ringing brightly forever.
const int32_t REVERB_COMB_FEEDBACK_Q16 = 53740;  // ~0.82
const int32_t REVERB_DAMP_ALPHA_Q16 = 13107;      // ~0.2
const int32_t REVERB_ALLPASS_FEEDBACK_Q16 = 32768; // 0.5, the standard Schroeder allpass value

// Ceiling for the Settings "Reverb Mix" percent (see reverbMixPercentToQ16()
// below) -- ~0.22 send level, the point this whole effect was actually
// reasoned about and tuned within (see this block's header comment).
// Reverb Mix's 0-100% maps *within* this ceiling rather than to a literal
// 0-100% wet blend, since a stronger send than this hasn't been verified
// not to get muddy/boomy given the comb feedback gain staging -- 100% on
// the control means "as strong as this reverb was designed to go," not
// "fully wet."
const int32_t REVERB_WET_MAX_Q16 = 14417;

// Runtime state for the two Settings controls (see Synth::setReverbEnabled()/
// setReverbMix() in synth.h) -- written only from loop1() in response to a
// FIFO message, same convention as g_masterGainQ16/g_outputLevelShift.
// Reverb defaults on (this whole effect exists to be heard) at a mix
// roughly matching what shipped before these became adjustable.
bool g_reverbEnabled = true;
uint32_t g_reverbMixQ16 = 0; // set from the default percent in setup1()

// Which reverb algorithm renderSample()'s reverb block runs, set from the
// Settings "Reverb Type" control (see Synth::setReverbType()) -- same
// write-only-from-loop1()-via-FIFO convention as g_reverbEnabled above.
// All three share the same comb+allpass tank (reverbProcess() below);
// Lush and Shimmer differ only in how they post-process its output (see
// chorusProcess()/shimmerProcess()'s own header comments) rather than
// being separate tanks, so switching types costs nothing extra in memory
// and doesn't need its own enable/disable bookkeeping.
const uint8_t REVERB_TYPE_LOFI = 0;
const uint8_t REVERB_TYPE_LUSH = 1;
const uint8_t REVERB_TYPE_SHIMMER = 2;
uint8_t g_reverbType = REVERB_TYPE_LOFI;

uint32_t reverbMixPercentToQ16(uint8_t percent) {
    if (percent > 100) percent = 100;
    return (uint32_t)(((uint64_t)REVERB_WET_MAX_Q16 * percent) / 100);
}

// Plain int32 multiplies throughout, not int64_t: delayed/dampState/bufOut
// are all bounded to roughly +-32767 (they either come straight out of an
// int16_t delay buffer or are a lowpass of one), and REVERB_*_Q16 are
// small fixed constants (max 65536), so the worst case (~65535 * 65536)
// still fits int32 -- no need for the software-emulated 64-bit multiply
// this Cortex-M0+ doesn't have in hardware (see cutoffHzToAlphaQ14()'s
// comment). `input` (the raw mix sum, not clamped to int16 range) is only
// ever added, never multiplied, so its larger magnitude doesn't risk
// overflow here either.
int32_t reverbComb(int16_t* buf, size_t& pos, int len, int32_t input, int32_t& dampState) {
    int32_t delayed = buf[pos];
    dampState += ((delayed - dampState) * REVERB_DAMP_ALPHA_Q16) >> 16;
    int32_t fedBack = input + ((dampState * REVERB_COMB_FEEDBACK_Q16) >> 16);
    if (fedBack > 32767) fedBack = 32767;
    if (fedBack < -32768) fedBack = -32768;
    buf[pos] = (int16_t)fedBack;
    pos++;
    if (pos >= (size_t)len) pos = 0;
    return delayed;
}

int32_t reverbAllpass(int16_t* buf, size_t& pos, int len, int32_t input) {
    int32_t bufOut = buf[pos];
    int32_t output = bufOut - input;
    int32_t fedBack = input + ((bufOut * REVERB_ALLPASS_FEEDBACK_Q16) >> 16);
    if (fedBack > 32767) fedBack = 32767;
    if (fedBack < -32768) fedBack = -32768;
    buf[pos] = (int16_t)fedBack;
    pos++;
    if (pos >= (size_t)len) pos = 0;
    return output;
}

// Runs the tank every sample (see this block's header comment on why),
// returns the full-rate wet signal -- reverbProcess()'s caller is what
// applies the decimate/bitcrush lo-fi treatment to what it reads out.
int32_t reverbProcess(int32_t input) {
    int32_t combSum = 0;
    combSum += reverbComb(g_reverbCombBuf0, g_reverbCombPos0, REVERB_COMB_LEN0, input, g_reverbDampState0);
    combSum += reverbComb(g_reverbCombBuf1, g_reverbCombPos1, REVERB_COMB_LEN1, input, g_reverbDampState1);
    combSum += reverbComb(g_reverbCombBuf2, g_reverbCombPos2, REVERB_COMB_LEN2, input, g_reverbDampState2);
    combSum += reverbComb(g_reverbCombBuf3, g_reverbCombPos3, REVERB_COMB_LEN3, input, g_reverbDampState3);
    combSum >>= 2; // 4 combs summed -- bring back down to roughly single-signal scale

    int32_t out = reverbAllpass(g_reverbAllpassBuf0, g_reverbAllpassPos0, REVERB_ALLPASS_LEN0, combSum);
    out = reverbAllpass(g_reverbAllpassBuf1, g_reverbAllpassPos1, REVERB_ALLPASS_LEN1, out);
    return out;
}

const int REVERB_DECIMATE = 4; // hold the wet output for this many samples -- ~11kHz effective
const int32_t REVERB_BIT_MASK = ~0x3F; // keep roughly the top 10 bits -- crude bitcrush grit
int32_t g_reverbHeld = 0;
int g_reverbDecimateCounter = 0;

// -- Lush/modulated reverb (Reverb Type: Lush) -------------------------------
// Runs the same comb+allpass tank as Lo-fi (reverbProcess() above), but
// instead of decimating/bitcrushing the wet output, reads it back through a
// small delay line whose read position is slowly wobbled by its own LFO
// (g_reverbChorusLfoPhase -- deliberately much slower than the ~5.5Hz voice
// vibrato rate; a chorus wants a gentle drift, not a warble). This is a
// post-process chorus on the tank's *output*, not a modulation of the comb
// filters' own internal feedback taps: those delay lengths were tuned to be
// mutually non-resonant (see reverbProcess()'s header comment), and wobbling
// them directly risks reintroducing the metallic ringing that tuning was
// meant to avoid. A wobbled reader gets the same "chorused, lively"
// character without touching that tuning.
const float REVERB_CHORUS_LFO_RATE_HZ = 0.3f; // slow drift, not a warble
uint32_t g_reverbChorusLfoPhaseInc = 0; // computed in setup1()
uint32_t g_reverbChorusLfoPhase = 0;

const int REVERB_CHORUS_BUF_LEN = 512; // power of two, ample margin over the delay range below
const int REVERB_CHORUS_BUF_MASK = REVERB_CHORUS_BUF_LEN - 1;
// Allocated only while Reverb Type is actually Lush (see
// reverbTypeChanged()), not a static array -- Lush and Shimmer are
// mutually exclusive, so there's no reason to pay for both buffers'
// worth of RAM at once when at most one is ever in use.
int16_t* g_reverbChorusBuf = nullptr;
size_t g_reverbChorusPos = 0;
const int32_t REVERB_CHORUS_BASE_Q8 = 300 << 8;  // ~6.8ms center delay
const int32_t REVERB_CHORUS_DEPTH_Q8 = 100 << 8; // +-~2.3ms sweep

int32_t chorusProcess(int32_t tankWet) {
    if (!g_reverbChorusBuf) return 0; // shouldn't happen -- see reverbTypeChanged()

    // Clamp before storing -- tankWet is the tank's raw output, whose
    // `input` isn't itself clamped to int16 range for a loud/many-voice
    // send (see reverbProcess()'s comment), but this delay line is an
    // int16_t buffer.
    int32_t clamped = tankWet;
    if (clamped > 32767) clamped = 32767;
    if (clamped < -32768) clamped = -32768;
    g_reverbChorusBuf[g_reverbChorusPos] = (int16_t)clamped;

    // lfoValue in [-32767, 32767], scaled to +-REVERB_CHORUS_DEPTH_Q8 --
    // safe int32 (32767 * 25600 =~ 8.4e8, nowhere near the ~2.1e9 int32
    // ceiling).
    int16_t lfoValue = g_sineTable[(g_reverbChorusLfoPhase >> 24) & (SINE_TABLE_SIZE - 1)];
    g_reverbChorusLfoPhase += g_reverbChorusLfoPhaseInc;
    int32_t depthQ8 = ((int32_t)lfoValue * REVERB_CHORUS_DEPTH_Q8) >> 15;
    int32_t delayQ8 = REVERB_CHORUS_BASE_Q8 + depthQ8;
    int32_t intDelay = delayQ8 >> 8;
    int32_t frac = delayQ8 & 0xFF; // Q8 fraction, 0..255

    // Adding a large multiple of the (power-of-two) buffer length before
    // masking isn't strictly needed -- unsigned wraparound already makes
    // this correct mod REVERB_CHORUS_BUF_LEN on its own -- but it keeps
    // the intent obvious rather than relying on that wraparound silently.
    size_t idx0 = (g_reverbChorusPos - (size_t)intDelay + REVERB_CHORUS_BUF_LEN * 4) & REVERB_CHORUS_BUF_MASK;
    size_t idx1 = (idx0 - 1) & REVERB_CHORUS_BUF_MASK;
    int32_t s0 = g_reverbChorusBuf[idx0];
    int32_t s1 = g_reverbChorusBuf[idx1];
    int32_t interpolated = s0 + (((s1 - s0) * frac) >> 8);

    g_reverbChorusPos = (g_reverbChorusPos + 1) & REVERB_CHORUS_BUF_MASK;
    return interpolated;
}

// -- Shimmer reverb (Reverb Type: Shimmer) -----------------------------------
// Classic ambient/shoegaze effect (Valhalla Shimmer-style): the tank's own
// wet output is pitch-shifted up an octave and fed back into the tank's
// input, so successive passes climb another octave each time, decaying
// naturally via the comb feedback coefficient (REVERB_COMB_FEEDBACK_Q16)
// instead of spiraling forever -- an ascending, evolving wash rather than a
// static one. The pitch shift itself is a standard two-tap granular
// shifter: a circular delay line is written at the normal (1x) rate while
// two read taps advance through it at 2x speed (an octave up); each tap
// restarts (jumps back a full grain, to exactly as far behind the write
// pointer as a 2x-speed read can go without ever overtaking it mid-grain)
// when it finishes its grain, and a triangular crossfade between the two
// taps (offset by half a grain from each other) hides that restart as a
// smooth fade instead of a click.
const int SHIMMER_GRAIN_LEN = 1024;  // ~23ms grain at 44.1kHz
const int SHIMMER_BUF_LEN = 2048;    // power of two, 2x grain length for lookback margin
const int SHIMMER_BUF_MASK = SHIMMER_BUF_LEN - 1;
const int32_t SHIMMER_PITCH_RATIO_Q16 = 2 << 16; // +1 octave (2x read speed vs write)
// Q16, ~0.4 -- keeps the regenerative feedback decaying rather than
// runaway, and stays well clear of int32 overflow multiplied against a
// worst-case +-32767 shimmerOut (32767 * 26214 =~ 8.6e8).
const int32_t SHIMMER_FEEDBACK_Q16 = 26214;

// Allocated only while Reverb Type is actually Shimmer -- see the same
// reasoning on g_reverbChorusBuf above (Lush and Shimmer are mutually
// exclusive, so only one of the two extra buffers is ever resident).
int16_t* g_shimmerBuf = nullptr;
size_t g_shimmerWritePos = 0;
int32_t g_shimmerReadPosQ16[2] = {0, 0};
int32_t g_shimmerGrainPos[2] = {0, SHIMMER_GRAIN_LEN / 2}; // half a grain apart, so their crossfades interleave
int32_t g_shimmerFeedbackHeld = 0;

int32_t shimmerReadTap(int tapIdx) {
    int32_t posQ16 = g_shimmerReadPosQ16[tapIdx];
    int32_t intPos = posQ16 >> 16;
    int32_t frac = posQ16 & 0xFFFF; // Q16 fraction

    size_t idx0 = ((size_t)intPos + SHIMMER_BUF_LEN * 4) & SHIMMER_BUF_MASK;
    size_t idx1 = (idx0 + 1) & SHIMMER_BUF_MASK;
    int32_t s0 = g_shimmerBuf[idx0];
    int32_t s1 = g_shimmerBuf[idx1];
    // s1-s0 maxes ~65535 and frac is Q16 (max 65535) -- their product
    // wouldn't fit int32 (~4.3e9), so frac is pre-shrunk to Q8 first
    // (same trick as chorusProcess()/cutoffHzToAlphaQ14()): worst case
    // becomes 65535 * 255 =~ 1.7e7, safely inside int32.
    int32_t interpolated = s0 + (((s1 - s0) * (frac >> 8)) >> 8);

    // Triangular window over the grain, 0 at both ends, peaking (Q8,
    // ~256) at the midpoint.
    int32_t g = g_shimmerGrainPos[tapIdx];
    int32_t window = (g < SHIMMER_GRAIN_LEN / 2)
        ? (g * 256) / (SHIMMER_GRAIN_LEN / 2)
        : ((SHIMMER_GRAIN_LEN - g) * 256) / (SHIMMER_GRAIN_LEN / 2);

    return (interpolated * window) >> 8;
}

int32_t shimmerProcess(int32_t tankWet) {
    if (!g_shimmerBuf) return 0; // shouldn't happen -- see reverbTypeChanged()

    int32_t clamped = tankWet;
    if (clamped > 32767) clamped = 32767;
    if (clamped < -32768) clamped = -32768;
    g_shimmerBuf[g_shimmerWritePos] = (int16_t)clamped;

    int32_t sum = shimmerReadTap(0) + shimmerReadTap(1);

    for (int t = 0; t < 2; t++) {
        g_shimmerReadPosQ16[t] += SHIMMER_PITCH_RATIO_Q16;
        g_shimmerGrainPos[t]++;
        if (g_shimmerGrainPos[t] >= SHIMMER_GRAIN_LEN) {
            g_shimmerGrainPos[t] = 0;
            // Restart this tap's grain a full grain-length behind the
            // *current* write pointer -- exactly far enough that a
            // 2x-speed read can run the whole next grain without ever
            // reading ahead of what's been written (see this block's
            // header comment).
            g_shimmerReadPosQ16[t] = ((int32_t)g_shimmerWritePos - SHIMMER_GRAIN_LEN) << 16;
        }
    }

    g_shimmerWritePos = (g_shimmerWritePos + 1) & SHIMMER_BUF_MASK;
    return sum;
}

// Frees whichever of g_reverbChorusBuf/g_shimmerBuf the *previous* type
// owned (if any) and allocates+zeroes whichever the *new* type owns (if
// not already allocated) -- called only from loop1()'s MSG_TYPE_REVERB_TYPE
// handler, so this always runs on core 1, never racing renderSample()'s own
// use of these pointers (both happen from the same core, and the FIFO-drain
// loop that calls this always finishes before renderSample() runs for that
// iteration). calloc, not malloc: a freshly allocated delay line must start
// silent, not with whatever garbage happened to be in that RAM before --
// stale content read back as audio would be an audible glitch, not just an
// uninitialized-read footgun. Also resets that type's read/write cursors,
// so switching back into a type later starts clean rather than referencing
// positions from a since-freed, differently-sized allocation.
void reverbTypeChanged(uint8_t newType) {
    if (newType != REVERB_TYPE_LUSH && g_reverbChorusBuf) {
        free(g_reverbChorusBuf);
        g_reverbChorusBuf = nullptr;
    }
    if (newType != REVERB_TYPE_SHIMMER && g_shimmerBuf) {
        free(g_shimmerBuf);
        g_shimmerBuf = nullptr;
    }
    if (newType == REVERB_TYPE_LUSH && !g_reverbChorusBuf) {
        g_reverbChorusBuf = (int16_t*)calloc(REVERB_CHORUS_BUF_LEN, sizeof(int16_t));
        g_reverbChorusPos = 0;
        g_reverbChorusLfoPhase = 0;
    }
    if (newType == REVERB_TYPE_SHIMMER && !g_shimmerBuf) {
        g_shimmerBuf = (int16_t*)calloc(SHIMMER_BUF_LEN, sizeof(int16_t));
        g_shimmerWritePos = 0;
        g_shimmerReadPosQ16[0] = 0;
        g_shimmerReadPosQ16[1] = 0;
        g_shimmerGrainPos[0] = 0;
        g_shimmerGrainPos[1] = SHIMMER_GRAIN_LEN / 2;
        g_shimmerFeedbackHeld = 0;
    }
    g_reverbType = newType;
}

// Fills outLeft/outRight with this sample's mixed output. Each synth
// voice contributes to mixL/mixR according to its own Voice::pan (melodic
// voices spread across the stereo field by MIDI channel, drums centered --
// see startMelodicVoice()/startPercussionVoice()), and a WAV stream
// (genuinely stereo -- see popWavFrame()) adds in on top of that; softLimit()/
// the leveler/volume then apply per-channel.
void renderSample(int16_t& outLeft, int16_t& outRight) {
    // One shared LFO lookup for this whole sample -- see g_lfoPhase's
    // comment -- rather than each voice maintaining (and advancing) its
    // own phase.
    int16_t lfoValue = g_sineTable[(g_lfoPhase >> 24) & (SINE_TABLE_SIZE - 1)];
    g_lfoPhase += g_lfoPhaseInc;

    int32_t mixL = 0, mixR = 0;
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        int32_t l, r;
        renderVoice(g_melodicVoices[i], lfoValue, l, r);
        mixL += l; mixR += r;
    }
    for (int i = 0; i < NUM_DRUM_VOICES; i++) {
        int32_t l, r;
        renderVoice(g_drumVoices[i], lfoValue, l, r);
        mixL += l; mixR += r;
    }

    // Reverb sends from the synth voices only (see reverbProcess()'s
    // header comment) -- captured before WAV/tracker content is mixed in
    // below, so a WAV/tracker file playing back never feeds the tank, only
    // the onboard MIDI synth's own voices do.
    int32_t synthMixL = mixL, synthMixR = mixR;

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

    // Reverb send: mono sum of the synth-only mix (see synthMixL/R above)
    // feeds the tank (see reverbProcess()'s header comment); which
    // post-processing runs on the tank's output depends on Reverb Type
    // (see g_reverbType's own comment for why all three share one tank).
    // The wet signal gets added back identically to both (full,
    // WAV-included) channels -- placed here (before the leveler/limiter)
    // so it's covered by the same headroom management as everything else,
    // not an uncontrolled extra on top of it. Skipped entirely (tank
    // included) when off -- see Synth::setReverbEnabled() -- so a
    // disabled reverb costs nothing, and re-enabling it starts from a
    // cold (silent) tank rather than resuming a stale one, unnoticeable
    // for an ambience effect like this.
    if (g_reverbEnabled) {
        int32_t monoSend = (synthMixL + synthMixR) >> 1;
        int32_t wetSignal;

        if (g_reverbType == REVERB_TYPE_SHIMMER) {
            // Regenerative: last sample's (already-decaying, see
            // SHIMMER_FEEDBACK_Q16) pitch-shifted output feeds back into
            // this sample's tank input, on top of the dry send.
            int32_t tankWet = reverbProcess(monoSend + g_shimmerFeedbackHeld);
            int32_t shimmerOut = shimmerProcess(tankWet);
            g_shimmerFeedbackHeld = (shimmerOut * SHIMMER_FEEDBACK_Q16) >> 16;
            // Tank halved so the pitched wash (the actual point of this
            // mode) leads rather than just riding underneath a full-level
            // plain reverb.
            wetSignal = (tankWet >> 1) + shimmerOut;
        } else {
            int32_t tankWet = reverbProcess(monoSend);
            if (g_reverbType == REVERB_TYPE_LUSH) {
                wetSignal = chorusProcess(tankWet);
            } else { // REVERB_TYPE_LOFI
                if (g_reverbDecimateCounter == 0) {
                    g_reverbHeld = tankWet & REVERB_BIT_MASK;
                }
                g_reverbDecimateCounter++;
                if (g_reverbDecimateCounter >= REVERB_DECIMATE) g_reverbDecimateCounter = 0;
                wetSignal = g_reverbHeld;
            }
        }

        // Plain int32: wetSignal stays within a few x +-32767 across all
        // three modes (see each mode's own overflow comments above),
        // g_reverbMixQ16 maxes out at REVERB_WET_MAX_Q16 (14417) -- worst
        // case product is nowhere near int32 overflow.
        int32_t reverbWetOut = (wetSignal * (int32_t)g_reverbMixQ16) >> 16;
        mixL += reverbWetOut;
        mixR += reverbWetOut;
    }

    // Auto-leveler: track loudness, then (rarely) update the attenuation.
    int32_t magL = mixL < 0 ? -mixL : mixL;
    int32_t magR = mixR < 0 ? -mixR : mixR;
    int32_t magnitude = magL > magR ? magL : magR;
    if (magnitude > g_levelerEnvelope) {
        g_levelerEnvelope += (magnitude - g_levelerEnvelope) >> 13; // ~186ms attack
    } else {
        g_levelerEnvelope += (magnitude - g_levelerEnvelope) >> 17; // ~3s release
    }
    if (++g_levelerUpdateCounter >= LEVELER_UPDATE_PERIOD) {
        g_levelerUpdateCounter = 0;
        if (g_levelerEnvelope > LEVELER_TARGET) {
            g_levelerGainQ16 = (uint32_t)(((int64_t)LEVELER_TARGET << 16) / g_levelerEnvelope);
        } else {
            g_levelerGainQ16 = 65536; // unity -- never boosts past this
        }
    }
    mixL = (int32_t)(((int64_t)mixL * g_levelerGainQ16) >> 16);
    mixR = (int32_t)(((int64_t)mixR * g_levelerGainQ16) >> 16);

    int32_t limitedL = softLimit(mixL);
    int32_t limitedR = softLimit(mixR);
    // Q16 multiply-shift against the precomputed taper gain (see
    // volumePercentToGainQ16()) rather than a runtime percent/100 divide --
    // cheaper (no hardware divide on the Cortex-M0+) and, unlike the old
    // divide, correct by construction for the audio taper. g_outputLevelShift
    // (see its own comment) is a final plain right-shift on top -- the
    // Headphone Low/High/Line Level coarse attenuation.
    outLeft = (int16_t)((((int64_t)limitedL * g_masterGainQ16) >> 16) >> g_outputLevelShift);
    outRight = (int16_t)((((int64_t)limitedR * g_masterGainQ16) >> 16) >> g_outputLevelShift);
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
    // Written here immediately, not just via the FIFO message below --
    // Synth::getChannelProgram() reads g_channelProgram[] directly, and a
    // caller that reads it back on the very same tick (e.g. pariSynth's
    // manual EDIT+LEFT/RIGHT handler, which calls
    // Ui::updatePariSynthChannelCell() right after this) would otherwise
    // often see the stale value -- core1 only applies the FIFO message
    // whenever it next drains its queue, not synchronously. Plain uint8_t
    // writes are already treated as safely atomic across cores in this
    // codebase (see synth.h's comment on the live-editable preset arrays);
    // core1's own write of the identical value when it later drains the
    // FIFO is redundant but harmless.
    g_channelProgram[channel] = program;
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

void Synth::setOutputLevel(uint8_t level) {
    if (level >= OUTPUT_LEVEL_COUNT) level = OUTPUT_LEVEL_HP_LOW;
    rp2040.fifo.push(packOutputLevelMsg(level));
}

void Synth::setReverbEnabled(bool enabled) {
    rp2040.fifo.push(packReverbEnabledMsg(enabled));
}

void Synth::setReverbMix(uint8_t percent) {
    if (percent > 100) percent = 100;
    rp2040.fifo.push(packReverbMixMsg(percent));
}

void Synth::setReverbType(uint8_t type) {
    if (type > 2) type = 0;
    rp2040.fifo.push(packReverbTypeMsg(type));
}

// -- Instrument/drum preset editing (see synth.h's long comment) --------
// Waveform/SynthWaveform enumerator order is kept identical by
// construction (see both declarations), so these are plain value casts,
// not a lookup -- but written as named functions rather than an inline
// static_cast at each call site so a future reordering of either enum
// fails loudly (a bad cast here is easy to spot; a silent static_cast
// elsewhere is not).
Waveform toInternalWaveform(Synth::SynthWaveform w) { return (Waveform)w; }
Synth::SynthWaveform toPublicWaveform(Waveform w) { return (Synth::SynthWaveform)w; }

const char* Synth::instrumentFamilyName(uint8_t family) {
    if (family >= 16) return "";
    return INSTRUMENT_FAMILY_NAMES[family];
}

void Synth::getInstrumentPreset(uint8_t family, Synth::InstrumentPresetParams& out) {
    if (family >= 16) return;
    const InstrumentPreset& p = PRESETS[family];
    out.waveform = toPublicWaveform(p.waveform);
    out.attackSamples = p.attackSamples;
    out.decaySamples = p.decaySamples;
    out.sustainPercent = p.sustainPercent;
    out.releaseSamples = p.releaseSamples;
    out.cutoffHz = p.cutoffHz;
    out.vibratoDepthPercent = p.vibratoDepthPercent;
    out.tremoloDepthPercent = p.tremoloDepthPercent;
    out.pwmDepthPercent = p.pwmDepthPercent;
}

void Synth::setInstrumentPreset(uint8_t family, const Synth::InstrumentPresetParams& in) {
    if (family >= 16) return;
    InstrumentPreset& p = PRESETS[family];
    p.waveform = toInternalWaveform(in.waveform);
    p.attackSamples = in.attackSamples;
    p.decaySamples = in.decaySamples;
    p.sustainPercent = in.sustainPercent;
    p.releaseSamples = in.releaseSamples;
    p.cutoffHz = in.cutoffHz;
    p.vibratoDepthPercent = in.vibratoDepthPercent;
    p.tremoloDepthPercent = in.tremoloDepthPercent;
    p.pwmDepthPercent = in.pwmDepthPercent;
    rp2040.fifo.push(packPresetDirtyMsg(family));
}

void Synth::resetInstrumentPresetToDefault(uint8_t family) {
    if (family >= 16) return;
    PRESETS[family] = DEFAULT_PRESETS[family];
    rp2040.fifo.push(packPresetDirtyMsg(family));
}

const char* Synth::drumPresetName(uint8_t drumType) {
    if (drumType >= DRUM_TYPE_COUNT) return "";
    return DRUM_PRESET_NAMES[drumType];
}

void Synth::getDrumPreset(uint8_t drumType, Synth::DrumPresetParams& out) {
    if (drumType >= DRUM_TYPE_COUNT) return;
    const DrumPreset& p = DRUM_PRESETS[drumType];
    out.waveform = toPublicWaveform(p.waveform);
    out.basePitchHz = p.basePitchHz;
    out.decaySamples = p.decaySamples;
    out.pitchDropStartHz = p.pitchDropStartHz;
    out.pitchDropSamples = p.pitchDropSamples;
    out.cutoffHz = p.cutoffHz;
}

void Synth::setDrumPreset(uint8_t drumType, const Synth::DrumPresetParams& in) {
    if (drumType >= DRUM_TYPE_COUNT) return;
    DrumPreset& p = DRUM_PRESETS[drumType];
    p.waveform = toInternalWaveform(in.waveform);
    p.basePitchHz = in.basePitchHz;
    p.decaySamples = in.decaySamples;
    p.pitchDropStartHz = in.pitchDropStartHz;
    p.pitchDropSamples = in.pitchDropSamples;
    p.cutoffHz = in.cutoffHz;
    rp2040.fifo.push(packDrumPresetDirtyMsg(drumType));
}

void Synth::resetDrumPresetToDefault(uint8_t drumType) {
    if (drumType >= DRUM_TYPE_COUNT) return;
    DRUM_PRESETS[drumType] = DEFAULT_DRUM_PRESETS[drumType];
    rp2040.fifo.push(packDrumPresetDirtyMsg(drumType));
}

void Synth::resetAllPresetsToDefault() {
    for (uint8_t i = 0; i < 16; i++) Synth::resetInstrumentPresetToDefault(i);
    for (uint8_t i = 0; i < DRUM_TYPE_COUNT; i++) Synth::resetDrumPresetToDefault(i);
}

uint8_t Synth::getChannelProgram(uint8_t channel) {
    if (channel >= 16) return 0;
    return g_channelProgram[channel];
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

    // Seed the live-editable preset tables from their shipped defaults --
    // see PRESETS[16]/DRUM_PRESETS[]'s comments just above their
    // declarations. Must happen before the cache-fill loops right below,
    // which read PRESETS[i]/DRUM_PRESETS[i].
    for (int i = 0; i < 16; i++) PRESETS[i] = DEFAULT_PRESETS[i];
    for (int i = 0; i < DRUM_TYPE_COUNT; i++) DRUM_PRESETS[i] = DEFAULT_DRUM_PRESETS[i];

    for (int i = 0; i < 16; i++) recomputeInstrumentPresetCache(i);
    for (int i = 0; i < DRUM_TYPE_COUNT; i++) recomputeDrumPresetCache(i);
    g_lfoPhaseInc = hzToPhaseInc(LFO_RATE_HZ);
    g_reverbChorusLfoPhaseInc = hzToPhaseInc(REVERB_CHORUS_LFO_RATE_HZ);
    g_masterGainQ16 = volumePercentToGainQ16(80); // matches the previous default percent
    g_reverbMixQ16 = reverbMixPercentToQ16(70); // matches the level this effect first shipped at

    // Larger than the library default (6 buffers x 64 words, ~9ms) to
    // absorb brief core 1 stalls without an audible underrun/pop -- most
    // notably, core 0 and core 1 share the same flash XIP bus, so core 0
    // reading MIDI track data off the SD card during playback can briefly
    // starve core 1's code fetches. Bumped from 8 buffers (~46ms) to 12
    // (~70ms) after round-2 popping diagnostics on busy multi-track MIDI
    // files showed availableForWrite() repeatedly dropping to ~19% of
    // capacity during passages with heavy track-switching (more open
    // tracks == more SD reads interleaved == more XIP contention) -- never
    // a confirmed full drain in that data, but not much margin left
    // either. Still plenty responsive for note timing; trading a bit more
    // latency for resilience is the right call here.
    g_i2s.setBuffers(12, 256);
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
            g_masterGainQ16 = volumePercentToGainQ16((uint8_t)(msg & 0xFF));
            continue;
        }
        if (type == MSG_TYPE_OUTPUT_LEVEL) {
            uint8_t level = (uint8_t)(msg & 0xFF);
            if (level < OUTPUT_LEVEL_COUNT) g_outputLevelShift = OUTPUT_LEVEL_SHIFT[level];
            continue;
        }
        if (type == MSG_TYPE_REVERB_ENABLED) {
            g_reverbEnabled = (msg & 0xFF) != 0;
            continue;
        }
        if (type == MSG_TYPE_REVERB_MIX) {
            g_reverbMixQ16 = reverbMixPercentToQ16((uint8_t)(msg & 0xFF));
            continue;
        }
        if (type == MSG_TYPE_REVERB_TYPE) {
            uint8_t t = (uint8_t)(msg & 0xFF);
            if (t <= REVERB_TYPE_SHIMMER && t != g_reverbType) reverbTypeChanged(t);
            continue;
        }
        if (type == MSG_TYPE_PRESET_DIRTY) {
            uint8_t family = (uint8_t)(msg & 0xFF);
            if (family < 16) recomputeInstrumentPresetCache(family);
            continue;
        }
        if (type == MSG_TYPE_DRUM_PRESET_DIRTY) {
            uint8_t drumType = (uint8_t)(msg & 0xFF);
            if (drumType < DRUM_TYPE_COUNT) recomputeDrumPresetCache(drumType);
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
