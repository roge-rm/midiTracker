#include "synth.h"
#include "pins.h"
#include <I2S.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/sync.h>

#ifdef HOST_MIDI
// Host only: samples where the leveler was below unity or the soft limit's knee was passed.
uint32_t g_hostLevelSamples = 0, g_hostLimitSamples = 0;
uint32_t g_hostTankClips = 0; // comb writes clamped to int16
// Host only: when set, the render governor sees a modelled cycle count
// (see loop1()) and counts the voices it sheds.
bool g_hostGovernorModel = false;
uint32_t g_hostGovernorSheds = 0;
#else
#include <hardware/structs/systick.h>
#include <hardware/clocks.h>
#endif

#if defined(MP3_PLAYER) && !defined(HOST_MIDI)
#include "mp3_file.h"
#endif

#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
#include <Arduino.h>
#include <hardware/timer.h>
// Profile build only: where each core is, in RAM that a watchdog reset
// leaves alone, so a hang can be read back after the reboot (see
// tracker_profile.cpp). where1/where0 are PW_* codes.
__attribute__((section(".uninitialized_data.proftrace"))) Synth::ProfileTrace g_trace;
#define PROF_WHERE1(n) (g_trace.where1 = (n))
#define PROF_WHERE0(n) (g_trace.where0 = (n))
#else
#define PROF_WHERE1(n) ((void)0)
#define PROF_WHERE0(n) ((void)0)
#endif

namespace {

// Cross-core message format, one 32-bit word per event in the message
// ring (see pushMsg()). The top nibble tags the message type, leaving 28
// payload bits. 0xC-0xE carry channel messages (CC, pitch bend, channel
// pressure). PANIC_MSG/RESET_PROGRAMS_MSG (top nibble 0xF) are reserved
// sentinels.
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
const uint32_t MSG_TYPE_LFO_RATE = 0x9u;
const uint32_t MSG_TYPE_LFO_VOICES = 0xAu;
const uint32_t MSG_TYPE_STEREO_SPREAD = 0xBu;
const uint32_t MSG_TYPE_CC = 0xCu;
const uint32_t MSG_TYPE_BEND = 0xDu;
const uint32_t MSG_TYPE_PRESSURE = 0xEu;
const uint32_t PANIC_MSG = 0xFFFFFFFFu;
const uint32_t RESET_PROGRAMS_MSG = 0xFFFFFFFEu;
const uint32_t RESET_CONTROLLERS_MSG = 0xFFFFFFFDu;

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
// tenthsHz easily fits the 28 payload bits available (max realistic value
// is a few hundred), 0 is the sentinel for the "Off" setting -- see
// g_lfoEnabled's comment.
inline uint32_t packLfoRateMsg(uint16_t tenthsHz) {
    return (MSG_TYPE_LFO_RATE << 28) | tenthsHz;
}
inline uint32_t packLfoVoicesMsg(uint8_t maxVoices) {
    return (MSG_TYPE_LFO_VOICES << 28) | maxVoices;
}
inline uint32_t packStereoSpreadMsg(uint8_t percent) {
    return (MSG_TYPE_STEREO_SPREAD << 28) | percent;
}
inline uint32_t packCcMsg(uint8_t channel, uint8_t cc, uint8_t value) {
    return (MSG_TYPE_CC << 28) | ((uint32_t)(channel & 0x0F) << 16) | ((uint32_t)(cc & 0x7F) << 8) | (value & 0x7F);
}
// bend14 is the raw 14-bit value, 8192 = centre.
inline uint32_t packBendMsg(uint8_t channel, uint16_t bend14) {
    return (MSG_TYPE_BEND << 28) | ((uint32_t)(channel & 0x0F) << 16) | (bend14 & 0x3FFF);
}
inline uint32_t packPressureMsg(uint8_t channel, uint8_t pressure) {
    return (MSG_TYPE_PRESSURE << 28) | ((uint32_t)(channel & 0x0F) << 16) | (pressure & 0x7F);
}

// Set by Synth::begin() on core 0, polled by setup1() on core 1 -- see
// the long comment on Synth::begin() in synth.h for why this exists.
// volatile (not a mutex/atomic) is sufficient: it's written exactly once,
// read in a simple spin loop, and RP2040's two cores share plain SRAM
// with no data cache to create a visibility problem.
volatile bool g_readyForPioInit = false;


// Core 0 -> core 1 message ring: single producer (core 0, pushMsg() from
// the Synth:: methods) and single consumer (core 1, popMsg() in loop1()),
// lock-free. head/tail are ever-increasing counts masked at index time,
// head written only by core 0 and tail only by core 1. Ordering is the
// same as the WAV ring below: the producer reads tail, dmb, writes the
// slot, dmb, publishes head; the consumer reads head, dmb, reads the slot,
// dmb, publishes tail. 128 deep so a big chord or a seek's catch-up burst
// rarely makes core 0 wait. rp2040.fifo is left to the core (idleOtherCore()).
const uint32_t MSG_RING_SIZE = 128; // power of two
const uint32_t MSG_RING_MASK = MSG_RING_SIZE - 1;
uint32_t g_msgRing[MSG_RING_SIZE];
volatile uint32_t g_msgHead = 0; // core 0 only
volatile uint32_t g_msgTail = 0; // core 1 only

void drainMessages(int maxMessages);

// Offline rendering (Synth::offlineBegin()). Core 0 sets g_offlineRequest,
// core 1 answers with g_offlineParked at the top of loop1() and from then
// on only writes silence to I2S, touching no synth state, until the
// request is cleared. g_offlineOwned: core 0 renders and drains the ring
// itself.
volatile bool g_offlineRequest = false;
volatile bool g_offlineParked = false;
bool g_offlineOwned = false;

// Core 0 only. Waits while the ring is full, which takes core 1 a few
// samples at most (it drains up to 32 a sample). While core 0 renders
// offline it drains the ring itself, as the host build does.
void pushMsg(uint32_t msg) {
    uint32_t head = g_msgHead;
#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
    // Profile build: a wait of 200 ms means core 1 stopped draining, so
    // say where it is, once per boot.
    static bool told = false;
    g_trace.lastMsg0 = msg;
    g_trace.pushes0++;
    if (g_offlineOwned && head - g_msgTail >= MSG_RING_SIZE) drainMessages(MSG_RING_SIZE);
    if (head - g_msgTail >= MSG_RING_SIZE) {
        uint32_t was = g_trace.where0, t0 = time_us_32();
        g_trace.where0 = Synth::PW0_PUSH_WAIT;
        while (head - g_msgTail >= MSG_RING_SIZE) {
            uint32_t waited = time_us_32() - t0;
            g_trace.pushWaitUs = waited;
            if (!told && waited > 200000) {
                told = true;
                Synth::profileDump("pushMsg waited 200 ms for core 1");
            }
        }
        g_trace.where0 = was;
    }
#endif
    while (head - g_msgTail >= MSG_RING_SIZE) {
#ifdef HOST_MIDI
        drainMessages(MSG_RING_SIZE); // host: core 1 runs on this thread
#else
        if (g_offlineOwned) drainMessages(MSG_RING_SIZE);
#endif
    }
    __dmb(); // tail read before the freed slot is written
    g_msgRing[head & MSG_RING_MASK] = msg;
    __dmb(); // slot written before head covers it
    g_msgHead = head + 1;
}

// Core 1 only.
bool __not_in_flash_func(popMsg)(uint32_t& msg) {
    uint32_t tail = g_msgTail;
    if (g_msgHead == tail) return false;
    __dmb(); // head read before the slot it covers
    msg = g_msgRing[tail & MSG_RING_MASK];
    __dmb(); // slot read before it is handed back
    g_msgTail = tail + 1;
    return true;
}

// ---- everything below this point runs on core 1 only (setup1()/loop1()
// and whatever they call) -- core 0 only ever touches the pack*Msg()
// helpers and pushMsg() above, from Synth::noteOn() etc. ----

const int SINE_TABLE_SIZE = 256;
// One extra entry (a copy of [0]) so the interpolated read needs no wrap.
int16_t g_sineTable[SINE_TABLE_SIZE + 1];

// Filled once in setup1() from PRESETS[i].cutoffHz (see cutoffHzToAlphaQ14())
// -- avoids repeating the expf() conversion on every single note-on. The
// drum-kit equivalent (g_drumFilterAlpha) is declared further down, once
// DRUM_TYPE_COUNT exists to size it.
uint32_t g_presetFilterAlpha[16];
// The preset cutoff as an index into CUTOFF_ALPHA_Q14 (nearest semitone),
// or CUTOFF_UNTRACKED for a cutoff of 18 kHz and up ("off"), which skips
// key and velocity tracking.
const uint8_t CUTOFF_UNTRACKED = 0xFF;
uint8_t g_presetCutoffSemi[16];

// Filled once in setup1() from PRESETS[i].vibratoDepthPercent.
uint32_t g_presetVibratoDepthQ16[16];

// Filled once in setup1() from PRESETS[i].tremoloDepthPercent/
// pwmDepthPercent -- same shared LFO as vibrato above, just modulating
// amplitude (tremolo) or square-wave duty cycle (PWM) instead of pitch.
uint32_t g_presetTremoloDepthQ16[16];
uint32_t g_presetPwmDepthQ16[16];

// Filled from PRESETS[i].decaySamples/releaseSamples (see envTimeToK()).
// Decay 0 is stored as 0, which skips the decay stage.
uint16_t g_presetDecayK[16];
uint16_t g_presetReleaseK[16];

// Shared vibrato/tremolo/PWM LFO: a single phase accumulator advanced once
// per sample (in renderSample(), not per voice) and read by every melodic
// voice, each scaled by its own preset depth (see Voice::vibratoRange/
// tremoloDepthQ16/pwmDepthQ16 and their use in renderVoice()). Deliberate
// simplification: every voice's modulation is therefore in-phase with
// every other voice's, rather than each note having its own independent
// LFO phase -- a true per-voice LFO would need its own accumulator (and a
// sine lookup) per voice for a difference unlikely to be audible at this
// fidelity level, whereas this is one lookup total per sample no matter
// how many voices are active.
//
// Rate is user-adjustable at runtime (Settings "LFO Rate" -- see
// Synth::setLfoRateTenthsHz()), unlike this file's other fixed LFO
// (REVERB_CHORUS_LFO_RATE_HZ, Lush reverb's own separate chorus wobble,
// untouched by this setting). LFO_RATE_HZ remains the shipped/default
// rate (matches this synth's original, previously-fixed value) -- read
// only once, at setup1(), to seed g_lfoPhaseInc before any Settings value
// has been applied; every later rate change goes through
// Synth::setLfoRateTenthsHz() instead. g_lfoEnabled is the "Off" position
// at the bottom of that same setting: rather than freezing g_lfoPhase
// wherever it happened to stop (which would leave every voice offset by
// whatever that frozen phase's sine value was, not neutral), renderSample()
// substitutes a constant 0 for lfoValue when this is false -- since
// vibrato/tremolo/PWM are each just `something * lfoValue`, a 0 disables
// all three at once, for every voice, without touching any preset's own
// depth fields.
const float LFO_RATE_HZ = 5.5f; // a natural vocal/instrumental vibrato rate
uint32_t g_lfoPhaseInc = 0; // seeded in setup1() from LFO_RATE_HZ, then Synth::setLfoRateTenthsHz()
uint32_t g_lfoPhase = 0;
bool g_lfoEnabled = true;

// Settings "Stereo Spread" -- see Synth::setStereoSpread() and its use in
// spreadPan() below, for channels that never sent CC10. 0 = every melodic channel centered (mono
// panning-wise -- drums already always are, see startPercussionVoice()),
// 100 = spreadPan()'s alternating-by-channel-parity mapping at
// its full +-96 width (see that function's own comment for why it
// alternates sides rather than ramping linearly by channel number, which
// used to put every commonly-used low channel on the same side). This
// dial is purely about overall width on top of that mapping -- it
// doesn't affect which side a channel lands on. Defaults to 30 (not
// 100): real-hardware listening found even the alternating layout too
// wide at full strength.
int g_stereoSpreadPercent = 30;

// Percussion gets its own dedicated pool rather than sharing one with
// melodic voices: with a 14-25 track file, melodic content alone can
// fill every voice, and the old shared-pool steal-voice-0 fallback would
// then steal drum hits just as readily as anything else -- so drums
// never got a reliable chance to sound. Splitting the pools means a busy
// melodic passage can never steal a drum voice and vice versa. 8 drum
// voices is generous (a full kit hit -- kick+snare+hats+a cymbal -- is
// nowhere near that); 24 melodic voices is a large step up from the
// previous 16-voice shared pool for the "still lots of voice stealing"
// complaint on dense multi-track files.
//
// Per-voice CPU cost is NOT trivial -- a real-hardware g_synthDiag* test
// (doubling this to 48/16) measured ~2.0-2.2us per simultaneously ACTIVE
// voice, not the single-cycle-multiply "basically free" this comment used
// to claim. At just 11 peak active voices (an ordinary two-handed chord,
// nowhere near either ceiling) with reverb on, renderSample()'s average
// cost alone exceeded the 22us/sample budget SAMPLE_RATE enforces, and
// core 1 measurably fell behind real time (~300ms of audio backing up per
// second) -- the same "song slows down" failure mode as the S3M/XM/MOD
// mixing-throughput bug, reproduced here. The voice-array *ceiling*
// barely matters (an inactive voice's cost is a cheap early-return), but
// how many voices actually sound at once does, and this project's own
// live multitimbral pariSynth mode makes 10+ simultaneous notes an
// entirely ordinary case, not a rare edge case. 24/8 is the last value
// confirmed to not reproduce this at ordinary polyphony; raising it
// further needs either a repeat of this measurement or a real reduction
// in renderVoice()'s per-sample cost first, not just picking a bigger
// number.
const int NUM_MELODIC_VOICES = 24;
const int NUM_DRUM_VOICES = 8;
// envTimeToK(100): 40 dB down in 100 samples (~2 ms), prompt but click-free.
const uint16_t PANIC_RELEASE_K = 2950;
// envTimeToK(400): an open hi-hat choked by a closed one, about 9 ms.
const uint16_t CHOKE_RELEASE_K = 750;
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
// WAV and tracker playback's leveler state (see Leveler, further down).
// It keeps releasing while no WAV plays, so the next file starts the
// way it did when the leveler ran on every sample.
int32_t g_levelerEnvelope = 0;
uint32_t g_levelerGainQ16 = 65536; // unity; never set above this -- attenuate-only
int g_levelerUpdateCounter = 0;

// WAVE_NOISE_HP is internal (hats and cymbals, see DRUM_TYPE_HIGH_PASS);
// the editor only ever sees the first five.
enum Waveform : uint8_t { WAVE_SINE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SQUARE, WAVE_NOISE, WAVE_NOISE_HP };
enum EnvStage : uint8_t { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Voice {
    bool active = false;
    EnvStage stage = ENV_ATTACK;
    uint8_t note = 0;
    Waveform waveform = WAVE_SINE;
    uint8_t channel = 0;   // MIDI channel 0-15 that started it, for note-off matching
    uint8_t family = 0;    // preset family at note-on, for recomputing vibratoRange on bend/mod
    bool held = false;     // note-off arrived while sustain (or sostenuto, see sostLatched) held it
    bool sostLatched = false; // was sounding when CC66 went down
    // VOICE_MOD_* bits, which of vibrato, PWM, tremolo and pitch drop are
    // on (see setModFlags()), so renderVoice() tests one byte for all four.
    uint8_t modFlags = 0;
    // Note-on transpose from the program's tweak (see PROGRAM_TWEAK), in
    // semitones. `note` stays the MIDI note so note-offs still match.
    int8_t transpose = 0;
    // A stolen voice fades out before the note that stole it starts (see
    // startStealFade()). pendVel 0 = nothing waiting. pendChan is the
    // channel plus PEND_OFF and PEND_SOST_LATCHED; pendProg is the
    // channel's program at the note-on.
    uint8_t pendNote = 0;
    uint8_t pendVel = 0;
    uint8_t pendChan = 0;
    uint8_t pendProg = 0;

    // The halfwords renderVoice() reads every sample sit here, inside the
    // M0+'s 62-byte ldrh offset reach. filterAlpha: see filterState below.
    // gainL, gainR (Q15) and sendGain (Q14) come from the channel's volume,
    // expression, pan and reverb send at note-on and when one of those CCs
    // arrives (see applyChannelGains()); sendGain can go past unity.
    uint16_t filterAlpha = 16384;
    uint16_t gainL = 0;
    uint16_t gainR = 0;
    uint16_t sendGain = 0;
    // Exponential decay/release rates, Q16 fraction of the remaining
    // distance per sample (see envTimeToK()). decayK 0 skips the decay stage.
    uint16_t decayK = 0;
    uint16_t releaseK = 0;

    uint32_t startOrder = 0; // g_voiceStartCounter at note-on, for oldest-voice stealing

    uint32_t phase = 0;    // 32-bit accumulator, one full cycle == 2^32
    uint32_t phaseInc = 0; // per-sample phase increment for this note's pitch

    // Envelope levels are Q16 (level 0..~2880 << 16), so slow stages still
    // move every sample. renderVoice() shifts once when it mixes.
    int32_t amplitude = 0; // current envelope level
    int32_t envStep = 0;   // attack: linear per-sample delta
    int32_t envFloor = 0;  // decay/release: distance left at which the stage ends (see envStageFloor())
    int32_t peakLevel = 0;    // end-of-attack level (velocity-scaled)
    int32_t sustainLevel = 0; // end-of-decay level; 0 for one-shot percussion

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
    // PolyBLEP: 2^31 / (phaseInc >> 16), worked out with phaseInc at
    // note-on and on a bend, so renderVoice() never divides.
    uint32_t blepInv = 0;

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
};
static_assert(sizeof(Voice) == 88, "Voice layout");

// pendChan flags. PEND_OFF: the note-off came during the fade while a
// pedal held it, so it starts held. PEND_SOST_LATCHED: sostenuto went down
// during the fade, which latches it like any other key still down.
const uint8_t PEND_OFF = 0x80;
const uint8_t PEND_SOST_LATCHED = 0x40;

const uint8_t VOICE_MOD_VIBRATO = 1;
const uint8_t VOICE_MOD_PWM = 2;
const uint8_t VOICE_MOD_TREMOLO = 4;
const uint8_t VOICE_MOD_PITCH_DROP = 8;

// After any change to vibratoRange, pwmDepthQ16, tremoloDepthQ16 or pitchDrop.
inline void setModFlags(Voice& v) {
    v.modFlags = (v.vibratoRange ? VOICE_MOD_VIBRATO : 0) |
                 (v.pwmDepthQ16 ? VOICE_MOD_PWM : 0) |
                 (v.tremoloDepthQ16 ? VOICE_MOD_TREMOLO : 0) |
                 (v.pitchDrop > 0 || v.pitchDropStep != 0 ? VOICE_MOD_PITCH_DROP : 0);
}

// Both pools in one array so renderSample() renders them in one loop
// (one inlined copy of renderVoice() in RAM instead of two).
const int NUM_VOICES = NUM_MELODIC_VOICES + NUM_DRUM_VOICES;
Voice g_voices[NUM_VOICES];
Voice* const g_melodicVoices = g_voices;
Voice* const g_drumVoices = g_voices + NUM_MELODIC_VOICES;
// Bumped per note-on. Age is the unsigned difference from this, so
// wraparound is harmless.
uint32_t g_voiceStartCounter = 0;
I2S g_i2s(OUTPUT, AUDIO_BCLK, AUDIO_SDATA); // LRCLK is implicitly AUDIO_BCLK+1, see pins.h

// GM program per channel (program / 8 = instrument-family index into
// PRESETS below). Program 0 (Acoustic Grand Piano family) by default,
// matching GM's own default.
uint8_t g_channelProgram[16] = {0};

// Per-channel controller state, 16 B a channel. Core 1 only: core 0 sends
// CC, bend and pressure as ring messages and handleMessage() applies them
// here, so nothing is shared across cores.
struct ChannelState {
    uint8_t volume;      // CC7
    uint8_t expression;  // CC11
    uint8_t pan;         // CC10, used only once panSet
    uint8_t reverbSend;  // CC91
    uint8_t mod;         // CC1
    uint8_t pressure;    // channel aftertouch, adds to mod
    uint8_t bendRange;   // RPN 0 MSB, semitones (capped at 24)
    uint8_t bendCents;   // RPN 0 LSB
    uint8_t rpnMsb;      // CC101, 127 = null
    uint8_t rpnLsb;      // CC100
    bool sustain;        // CC64
    bool sostenuto;      // CC66
    bool soft;           // CC67
    bool panSet;         // a CC10 arrived, so Stereo Spread no longer applies
    int16_t bend;        // -8192..8191
};
ChannelState g_chan[16];
// Pitch ratio from bend and bend range, Q28 (1 << 28 = no bend), worked
// out when either changes.
uint32_t g_chanBendQ28[16];

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
// above). Written only from loop1() in response to a ring message (see
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
// are unambiguous -- the standard SPSC ring shape. head is only written by
// core 0 and tail only by core 1 (32-bit words, so loads and stores are
// atomic). Ordering uses __dmb():
// - producer: write frame data, dmb, then publish head, so core 1 never
//   sees a head that covers data not yet in SRAM;
// - consumer: read head, dmb, read frame data, dmb, then publish tail, so
//   the data reads finish before core 0 may reuse those slots;
// - producer reads tail then dmb before writing into the freed slots.
// A reset from core 0 never writes tail: it posts the head value to cut
// back to and bumps g_wavResetSeq, and core 1 applies it at the top of
// renderSample() (see wavStreamApplyReset()).
const size_t WAV_RING_FRAMES = 8192; // power of two -- ~186ms at 44.1kHz, see synth.h
const size_t WAV_RING_MASK = WAV_RING_FRAMES - 1;
int16_t g_wavRing[WAV_RING_FRAMES * 2]; // interleaved L,R
volatile size_t g_wavHead = 0; // producer-owned
volatile size_t g_wavTail = 0; // consumer-owned
volatile size_t g_wavResetTo = 0;    // core 0: head at the last reset; written before g_wavResetSeq
volatile uint32_t g_wavResetSeq = 0; // core 0 bumps it per reset
volatile uint32_t g_wavResetSeen = 0; // written by core 1 only, read by wavStreamProducerTail()
volatile bool g_wavActive = false; // stream is live -- renderSample() should try to pop
volatile bool g_wavEnded = false;  // no more source data -- empty means "done", not "underrun"
volatile bool g_wavUnderrun = false;
// Real-hardware diagnostic (see Synth::wavStreamUnderrunSamples()
// below) -- g_wavUnderrun
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

#define ALWAYS_INLINE __attribute__((always_inline)) inline

// Consumer-side pop, called once per sample from renderSample() while
// g_wavActive. Returns false on empty (caller decides whether that means
// "done" or "underrun" via g_wavEnded).
bool __not_in_flash_func(popWavFrame)(int16_t& outL, int16_t& outR) {
    size_t tail = g_wavTail;
    if (g_wavHead == tail) return false;
    __dmb(); // acquire: head read before the frame data it covers
    size_t idx = tail & WAV_RING_MASK;
    outL = g_wavRing[idx * 2 + 0];
    outR = g_wavRing[idx * 2 + 1];
    __dmb(); // release: data read before the slot is handed back
    g_wavTail = tail + 1;
    return true;
}

// Core 1, once per sample: drops every frame written before core 0's last
// wavStreamReset(). Frames written since then are kept. Never moves tail
// backwards, in case it already got past the reset point.
ALWAYS_INLINE void __not_in_flash_func(wavStreamApplyReset)() {
    uint32_t seq = g_wavResetSeq;
    if (seq == g_wavResetSeen) return;
    __dmb(); // acquire: seq read before g_wavResetTo
    size_t to = g_wavResetTo;
    if ((int32_t)(to - g_wavTail) > 0) g_wavTail = to;
    __dmb(); // release: tail written before the ack core 0 waits on
    g_wavResetSeen = seq;
}

int16_t __not_in_flash_func(nextNoise)() {
    g_noiseState ^= g_noiseState << 13;
    g_noiseState ^= g_noiseState >> 17;
    g_noiseState ^= g_noiseState << 5;
    return (int16_t)(g_noiseState >> 16);
}

// int16 saturation in three cycles when nothing clips.
ALWAYS_INLINE int32_t clampInt16(int32_t x) {
    if ((int16_t)x != x) x = (x >> 31) ^ 0x7FFF;
    return x;
}

// PolyBLEP residual for an edge at phase 0, in Q15 of the step's half
// height, positive just before the edge and negative just after. w is the
// phase (top 16 bits) plus dt, so the edge region is w < 2 * dt; outside it
// this returns 0 after one compare.
ALWAYS_INLINE int32_t __not_in_flash_func(blepResidual)(uint32_t w, uint32_t dt, uint32_t inv) {
    if (w >= 2 * dt) return 0;
    // r = distance to the region's outer end over dt, Q15, at most 32768.
    uint32_t d = w < dt ? w : 2 * dt - w;
    uint32_t r = (d * inv) >> 16;
    int32_t e = (int32_t)((r * r) >> 15);
    return w < dt ? e : -e;
}

// phase represents one full waveform cycle over its entire 32-bit range.
// Sine reads the 256-entry table at the top 8 bits and interpolates on the
// next 8; the others use the top 16 bits as a 0..65535 ramp. Saw and square
// are PolyBLEP band-limited with the voice's nominal dt (phaseInc >> 16)
// and blepInv. pulseWidth is only meaningful for WAVE_SQUARE (the
// duty-cycle threshold, 32768 == 50%, see Voice::pwmDepthQ16).
ALWAYS_INLINE int32_t __not_in_flash_func(waveformSample)(Voice& v, uint32_t phase, uint16_t pulseWidth) {
    // An if tree, not a switch: six cases make GCC use a jump table, and
    // that would sit in flash.
    uint32_t p16 = phase >> 16;
    Waveform w = v.waveform;
    if (w < WAVE_SAW) {
        if (w == WAVE_SINE) {
            uint32_t idx = phase >> 24;
            int32_t s0 = g_sineTable[idx];
            int32_t s1 = g_sineTable[idx + 1];
            return s0 + (((s1 - s0) * (int32_t)(p16 & 0xFF)) >> 8);
        }
        if (p16 < 32768) return (int32_t)(p16 * 2) - 32768; // triangle
        return 32767 - (int32_t)(p16 - 32768) * 2;
    }
    if (w <= WAVE_SQUARE) {
        uint32_t dt = v.phaseInc >> 16;
        if (w == WAVE_SAW) {
            // Falls by a full 65536 at the wrap.
            return (int32_t)p16 - 32768 - blepResidual((p16 + dt) & 0xFFFF, dt, v.blepInv);
        }
        // Square, +-30000: rises at 0, falls at pulseWidth. The residual is
        // for a step of 65536, scaled to 60000 here.
        int32_t out = p16 < pulseWidth ? 30000 : -30000;
        uint32_t w1 = (p16 + dt) & 0xFFFF, w2 = (p16 - pulseWidth + dt) & 0xFFFF;
        if ((w1 < 2 * dt) | (w2 < 2 * dt)) {
            int32_t e = blepResidual(w1, dt, v.blepInv) - blepResidual(w2, dt, v.blepInv);
            // Near Nyquist both edge regions overlap; keep the filter's int32 maths safe.
            out = clampInt16(out + ((e * 15000) >> 14));
        }
        return out;
    }
    int32_t n = nextNoise();
    if (w == WAVE_NOISE) return n;
    // WAVE_NOISE_HP: first difference of the noise, a gentle high-pass.
    // Noise voices have no use for phase, so it holds the last noise value.
    int32_t out = (n - (int32_t)phase) >> 1;
    v.phase = (uint32_t)n;
    return out;
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
    // Double intermediate: this only runs at startup and on a preset or
    // LFO rate change, and float's ~7 digits would round visibly at 2^32.
    return (uint32_t)((double)hz * 4294967296.0 / SAMPLE_RATE);
}

// Phase increment per MIDI note at 44.1 kHz, round(440 * 2^((n-69)/12) * 2^32 / 44100).
const uint32_t NOTE_PHASE_INC[128] = {
    796254u, 843601u, 893765u, 946911u, 1003217u, 1062871u, 1126073u, 1193033u,
    1263974u, 1339134u, 1418763u, 1503127u, 1592507u, 1687203u, 1787529u, 1893821u,
    2006434u, 2125742u, 2252146u, 2386065u, 2527948u, 2678268u, 2837526u, 3006254u,
    3185015u, 3374406u, 3575058u, 3787642u, 4012867u, 4251485u, 4504291u, 4772130u,
    5055896u, 5356535u, 5675051u, 6012507u, 6370030u, 6748811u, 7150117u, 7575285u,
    8025735u, 8502970u, 9008582u, 9544261u, 10111792u, 10713070u, 11350103u, 12025015u,
    12740059u, 13497623u, 14300233u, 15150569u, 16051469u, 17005939u, 18017165u, 19088521u,
    20223584u, 21426141u, 22700205u, 24050030u, 25480119u, 26995246u, 28600467u, 30301139u,
    32102938u, 34011878u, 36034330u, 38177043u, 40447168u, 42852281u, 45400411u, 48100060u,
    50960238u, 53990491u, 57200933u, 60602278u, 64205876u, 68023757u, 72068660u, 76354085u,
    80894335u, 85704563u, 90800821u, 96200119u, 101920476u, 107980983u, 114401866u, 121204555u,
    128411753u, 136047513u, 144137319u, 152708170u, 161788671u, 171409126u, 181601643u, 192400238u,
    203840952u, 215961966u, 228803732u, 242409110u, 256823506u, 272095026u, 288274639u, 305416341u,
    323577341u, 342818251u, 363203285u, 384800477u, 407681904u, 431923931u, 457607465u, 484818220u,
    513647012u, 544190053u, 576549277u, 610832681u, 647154683u, 685636503u, 726406571u, 769600953u,
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u, 1153098554u, 1221665363u,
};
static_assert(SAMPLE_RATE == 44100, "NOTE_PHASE_INC is for 44.1 kHz");

// GM's 40 log curve, round(32768 * (i / 127)^2), for velocity, CC7 and CC11.
const uint16_t GM_CURVE_Q15[128] = {
    0, 2, 8, 18, 33, 51, 73, 100, 130, 165, 203, 246, 293, 343, 398, 457,
    520, 587, 658, 733, 813, 896, 983, 1075, 1170, 1270, 1373, 1481, 1593, 1709, 1828, 1952,
    2080, 2212, 2349, 2489, 2633, 2781, 2934, 3090, 3251, 3415, 3584, 3756, 3933, 4114, 4299, 4488,
    4681, 4878, 5079, 5284, 5494, 5707, 5924, 6146, 6371, 6601, 6834, 7072, 7314, 7560, 7810, 8064,
    8322, 8584, 8850, 9120, 9394, 9673, 9955, 10241, 10532, 10827, 11125, 11428, 11735, 12045, 12360, 12679,
    13002, 13329, 13661, 13996, 14335, 14678, 15026, 15377, 15733, 16092, 16456, 16824, 17196, 17571, 17951, 18335,
    18723, 19116, 19512, 19912, 20316, 20725, 21137, 21553, 21974, 22399, 22827, 23260, 23697, 24138, 24583, 25032,
    25485, 25942, 26403, 26868, 27337, 27811, 28288, 28770, 29255, 29745, 30239, 30736, 31238, 31744, 32254, 32768,
};
// Equal-power pan, round(32768 * sin(max(i - 1, 0) / 126 * pi / 2)). Right
// gain is [pan], left is [128 - max(pan, 1)], so 0 and 1 are hard left and 64
// is centre (0.707 each side).
const uint16_t PAN_SIN_Q15[128] = {
    0, 0, 408, 817, 1225, 1633, 2041, 2449, 2856, 3263, 3669, 4074, 4480, 4884, 5287, 5690,
    6092, 6493, 6893, 7292, 7689, 8086, 8481, 8875, 9267, 9659, 10048, 10436, 10823, 11207, 11590, 11971,
    12351, 12728, 13104, 13477, 13848, 14218, 14584, 14949, 15311, 15671, 16029, 16384, 16736, 17086, 17434, 17778,
    18120, 18459, 18795, 19128, 19458, 19785, 20110, 20431, 20748, 21063, 21374, 21682, 21987, 22288, 22586, 22880,
    23170, 23458, 23741, 24021, 24297, 24569, 24837, 25102, 25362, 25619, 25872, 26120, 26365, 26606, 26842, 27074,
    27302, 27526, 27745, 27961, 28171, 28378, 28580, 28778, 28971, 29159, 29343, 29523, 29698, 29868, 30034, 30195,
    30351, 30503, 30650, 30792, 30929, 31062, 31189, 31312, 31430, 31543, 31651, 31755, 31853, 31946, 32035, 32118,
    32197, 32270, 32339, 32402, 32460, 32514, 32562, 32605, 32643, 32676, 32704, 32727, 32745, 32758, 32765, 32768,
};
// 2^(s / 12) for s = 0..11 and 2^(c / 1200) for c = 0..99, Q30, for pitch bend.
const uint32_t SEMI_RATIO_Q30[12] = {
    1073741824, 1137589835, 1205234447, 1276901417, 1352829926, 1433273380,
    1518500250, 1608794974, 1704458901, 1805811301, 1913190429, 2026954652,
};
const uint32_t CENT_RATIO_Q30[100] = {
    1073741824, 1074362221, 1074982976, 1075604090, 1076225563, 1076847394,
    1077469586, 1078092136, 1078715047, 1079338317, 1079961947, 1080585938,
    1081210289, 1081835001, 1082460074, 1083085508, 1083711303, 1084337460,
    1084963979, 1085590860, 1086218103, 1086845708, 1087473676, 1088102007,
    1088730701, 1089359758, 1089989179, 1090618963, 1091249112, 1091879624,
    1092510500, 1093141742, 1093773347, 1094405318, 1095037654, 1095670355,
    1096303422, 1096936855, 1097570653, 1098204818, 1098839349, 1099474247,
    1100109512, 1100745144, 1101381143, 1102017509, 1102654243, 1103291345,
    1103928816, 1104566654, 1105204861, 1105843437, 1106482382, 1107121695,
    1107761379, 1108401432, 1109041854, 1109682647, 1110323810, 1110965344,
    1111607248, 1112249523, 1112892169, 1113535186, 1114178575, 1114822336,
    1115466468, 1116110973, 1116755850, 1117401100, 1118046723, 1118692719,
    1119339088, 1119985830, 1120632946, 1121280436, 1121928300, 1122576538,
    1123225151, 1123874139, 1124523502, 1125173240, 1125823353, 1126473842,
    1127124707, 1127775947, 1128427564, 1129079558, 1129731928, 1130384676,
    1131037800, 1131691302, 1132345181, 1132999438, 1133654074, 1134309087,
    1134964479, 1135620249, 1136276399, 1136932927,
};

// One-pole alpha (Q14, see cutoffHzToAlphaQ14()) for a cutoff at MIDI
// note n's pitch, n = 0..135 (8 Hz to 19.9 kHz), for the note-on key and
// velocity tracking in startMelodicVoice().
const int CUTOFF_SEMI_COUNT = 136;
const uint16_t CUTOFF_ALPHA_Q14[CUTOFF_SEMI_COUNT] = {
    19, 20, 21, 23, 24, 25, 27, 29, 30, 32, 34, 36, 38, 40, 43, 45, 48,
    51, 54, 57, 60, 64, 68, 72, 76, 81, 85, 91, 96, 102, 108, 114, 121, 128,
    135, 143, 152, 161, 170, 181, 191, 203, 215, 227, 241, 255, 270, 286, 303, 320, 339,
    359, 380, 403, 426, 451, 478, 506, 535, 566, 599, 634, 671, 710, 752, 795, 841, 890,
    941, 996, 1053, 1113, 1177, 1244, 1315, 1390, 1469, 1552, 1639, 1732, 1829, 1931, 2038, 2151, 2269,
    2394, 2525, 2662, 2806, 2957, 3115, 3280, 3453, 3634, 3822, 4019, 4225, 4438, 4661, 4892, 5131, 5380,
    5637, 5904, 6178, 6462, 6753, 7053, 7360, 7674, 7995, 8323, 8656, 8993, 9335, 9680, 10027, 10375, 10723,
    11069, 11414, 11754, 12089, 12418, 12739, 13050, 13351, 13641, 13917, 14180, 14428, 14660, 14876, 15076, 15258, 15424,
};

// Per-program tweaks on top of the family preset, so the 8 programs in a
// GM family differ. The editor and pariSynth (program family * 8) use the
// family preset as is, so each family's first program is 0. Bits 0-1:
// octave (1 up, 2 down); bits 2-4: cutoff offset in steps of 4 semitones,
// signed (-4..3); bits 5-7: waveform (0 the family's, 1 sine, 2 triangle,
// 3 saw, 4 square, 5 noise).
const uint8_t PROGRAM_TWEAK[128] = {
    0x00, 0x08, 0x04, 0x64, 0x20, 0x44, 0x68, 0x84, //   0 Piano
    0x00, 0x01, 0x41, 0x40, 0x5C, 0x5D, 0x18, 0x70, //   8 Chromatic percussion
    0x00, 0x1C, 0x64, 0x5C, 0x18, 0x7C, 0x18, 0x78, //  16 Organ
    0x00, 0x04, 0x54, 0x1C, 0x10, 0x84, 0x68, 0x21, //  24 Guitar
    0x00, 0x1C, 0x08, 0x20, 0x68, 0x88, 0x04, 0x0C, //  32 Bass
    0x00, 0x1C, 0x18, 0x14, 0x04, 0x5C, 0x40, 0x38, //  40 Strings
    0x00, 0x1C, 0x04, 0x84, 0x40, 0x20, 0x44, 0x08, //  48 Ensemble
    0x00, 0x1C, 0x18, 0x14, 0x58, 0x60, 0x64, 0x04, //  56 Brass
    0x00, 0x7C, 0x78, 0x74, 0x60, 0x1C, 0x18, 0x04, //  64 Reed
    0x00, 0x01, 0x40, 0x50, 0x54, 0x58, 0x41, 0x5C, //  72 Pipe
    0x00, 0x00, 0x40, 0x20, 0x84, 0x58, 0x1C, 0x02, //  80 Synth lead
    0x00, 0x78, 0x60, 0x04, 0x7C, 0x80, 0x20, 0x64, //  88 Synth pad
    0x00, 0x70, 0x01, 0x74, 0x7C, 0x90, 0x58, 0x41, //  96 Synth effects
    0x00, 0x68, 0x60, 0x04, 0x20, 0x80, 0x7C, 0x84, // 104 Ethnic
    0x00, 0x20, 0x40, 0x21, 0x22, 0x42, 0x58, 0xA0, // 112 Percussive
    0x00, 0xB4, 0xB8, 0x01, 0x9C, 0x7A, 0xA0, 0xBC, // 120 Sound effects
};

// RMS of each waveform at full level, so a program tweak that changes the
// waveform keeps its family's loudness.
const uint16_t WAVE_RMS[6] = {22627, 18919, 18919, 30000, 18919, 13377};

// Per-sample Q16 fraction for an exponential stage that falls 40 dB in
// `samples` (the time the editor shows), about how long the old linear
// ramp sounded. 0 samples is instant. Float, but
// only runs on a preset change.
uint16_t envTimeToK(uint32_t samples) {
    if (samples <= 1) return 65535;
    float k = -expm1f(-4.6052f / (float)samples); // ln(100) = 4.6052
    int32_t q16 = (int32_t)(k * 65536.0f + 0.5f);
    if (q16 < 1) q16 = 1;
    if (q16 > 65535) q16 = 65535;
    return (uint16_t)q16;
}

// One exponential envelope step: moves `dist` (Q16, >= 0) towards 0 by k.
// Both forms keep the multiply in 32 bits (dist >> 12 <= ~46000, or
// dist >> 4 < 65536, times k < 65536); the second keeps small distances
// precise. The +1 makes sure a stage always finishes.
ALWAYS_INLINE int32_t __not_in_flash_func(envExpStep)(int32_t dist, uint16_t k) {
    uint32_t d = (uint32_t)dist;
    uint32_t step = d >= (1u << 20) ? ((d >> 12) * k) >> 4 : ((d >> 4) * k) >> 12;
    return dist - (int32_t)step - 1;
}

// Distance at which a decay or release ends: 60 dB below where it
// started, and never below 4096 (level 1/16, about -90 dB of full scale),
// so a quiet voice doesn't crawl down 1 per sample while silent.
inline int32_t envStageFloor(int32_t startDist) {
    int32_t f = startDist >> 10;
    return f > 4096 ? f : 4096;
}

// One preset per GM instrument family (program / 8, 0-15) -- not one per
// program, which would need 128 entries for barely more variety than
// broad families already give at this fidelity level. Times are in
// samples at 44.1kHz: attack is linear, decay and release are exponential
// and reach -40 dB at their time. decaySamples == 0 skips straight from
// attack to sustain (organ-like, no decay stage).
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
// from DEFAULT_PRESETS in Synth::begin(); mutated only via
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
// Derived from DRUM_PRESETS by recomputeDrumPresetCache(), so a drum hit
// needs no float math on core 1.
uint16_t g_drumDecayK[DRUM_TYPE_COUNT];
uint32_t g_drumPhaseInc[DRUM_TYPE_COUNT];
int32_t g_drumPitchDrop[DRUM_TYPE_COUNT];
int32_t g_drumPitchDropStep[DRUM_TYPE_COUNT];

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
    float fc = PRESETS[family].cutoffHz;
    if (fc >= 18000.0f) {
        g_presetCutoffSemi[family] = CUTOFF_UNTRACKED;
    } else {
        int32_t semi = (int32_t)lroundf(69.0f + 12.0f * log2f((fc > 8.0f ? fc : 8.0f) / 440.0f));
        if (semi < 0) semi = 0;
        if (semi > CUTOFF_SEMI_COUNT - 1) semi = CUTOFF_SEMI_COUNT - 1;
        g_presetCutoffSemi[family] = (uint8_t)semi;
    }
    // Capped at 30% (the editor's own limit) so renderVoice()'s vibrato
    // multiply can't overflow int32 at note 127.
    float vib = PRESETS[family].vibratoDepthPercent;
    if (vib > 30.0f) vib = 30.0f;
    if (vib < 0.0f) vib = 0.0f;
    g_presetVibratoDepthQ16[family] = (uint32_t)(vib / 100.0f * 65536.0f + 0.5f);
    g_presetTremoloDepthQ16[family] = (uint32_t)(PRESETS[family].tremoloDepthPercent / 100.0f * 65536.0f + 0.5f);
    g_presetPwmDepthQ16[family] = (uint32_t)(PRESETS[family].pwmDepthPercent / 100.0f * 65536.0f + 0.5f);
    g_presetDecayK[family] = PRESETS[family].decaySamples ? envTimeToK(PRESETS[family].decaySamples) : 0;
    g_presetReleaseK[family] = envTimeToK(PRESETS[family].releaseSamples);
}

// Same as recomputeInstrumentPresetCache() above, for one drum type's
// derived cache values.
void recomputeDrumPresetCache(int drumType) {
    const DrumPreset& p = DRUM_PRESETS[drumType];
    g_drumFilterAlpha[drumType] = cutoffHzToAlphaQ14(p.cutoffHz);
    g_drumDecayK[drumType] = p.decaySamples ? envTimeToK(p.decaySamples) : 0;
    g_drumPhaseInc[drumType] = (p.basePitchHz > 0.0f) ? hzToPhaseInc(p.basePitchHz) : 0;
    // Drop time 0 means no drop, whatever the amount.
    int32_t drop = 0, step = 0;
    if (p.pitchDropStartHz > 0.0f && p.pitchDropSamples > 0) {
        drop = (int32_t)hzToPhaseInc(p.pitchDropStartHz);
        step = -(drop / (int32_t)p.pitchDropSamples);
        if (step == 0) step = -1;
    }
    g_drumPitchDrop[drumType] = drop;
    g_drumPitchDropStep[drumType] = step;
}

// GM percussion notes 35-81: which preset plays each, a pitch ratio on the
// preset's base pitch and pitch drop (Q8, 256 = as set), and a decay rate
// shift (positive is shorter, each step halves the time). Toms spread from
// 80 to 157 Hz around the preset's 120; bongos, congas and timbales are
// short high toms, cowbell, agogos and triangles are woodblock tones.
// Notes outside the table play DRUM_DEFAULT as set.
struct DrumNote { uint8_t type; int8_t decayShift; uint16_t ratioQ8; };
const uint8_t DRUM_NOTE_FIRST = 35, DRUM_NOTE_LAST = 81;
const DrumNote DRUM_NOTES[DRUM_NOTE_LAST - DRUM_NOTE_FIRST + 1] = {
    {DRUM_KICK, 0, 256},       // 35 Acoustic Bass Drum
    {DRUM_KICK, 0, 256},       // 36 Bass Drum 1
    {DRUM_SNARE, 0, 256},      // 37 Side Stick
    {DRUM_SNARE, 0, 256},      // 38 Acoustic Snare
    {DRUM_SNARE, 0, 256},      // 39 Hand Clap
    {DRUM_SNARE, 0, 256},      // 40 Electric Snare
    {DRUM_TOM, 0, 171},        // 41 Low Floor Tom
    {DRUM_CLOSED_HAT, 0, 256}, // 42 Closed Hi-Hat
    {DRUM_TOM, 0, 197},        // 43 High Floor Tom
    {DRUM_CLOSED_HAT, 0, 256}, // 44 Pedal Hi-Hat
    {DRUM_TOM, 0, 224},        // 45 Low Tom
    {DRUM_OPEN_HAT, 0, 256},   // 46 Open Hi-Hat
    {DRUM_TOM, 0, 256},        // 47 Low-Mid Tom
    {DRUM_TOM, 0, 293},        // 48 Hi-Mid Tom
    {DRUM_CYMBAL, 0, 256},     // 49 Crash Cymbal 1
    {DRUM_TOM, 0, 336},        // 50 High Tom
    {DRUM_CYMBAL, 0, 256},     // 51 Ride Cymbal 1
    {DRUM_CYMBAL, 0, 256},     // 52 Chinese Cymbal
    {DRUM_CYMBAL, 0, 256},     // 53 Ride Bell
    {DRUM_CLOSED_HAT, 0, 256}, // 54 Tambourine
    {DRUM_CYMBAL, 0, 256},     // 55 Splash Cymbal
    {DRUM_WOODBLOCK, -1, 159}, // 56 Cowbell, 560 Hz
    {DRUM_CYMBAL, 0, 256},     // 57 Crash Cymbal 2
    {DRUM_DEFAULT, 0, 256},    // 58 Vibraslap
    {DRUM_CYMBAL, 0, 256},     // 59 Ride Cymbal 2
    {DRUM_TOM, 2, 896},        // 60 Hi Bongo
    {DRUM_TOM, 2, 666},        // 61 Low Bongo
    {DRUM_TOM, 3, 704},        // 62 Mute Hi Conga
    {DRUM_TOM, 1, 704},        // 63 Open Hi Conga
    {DRUM_TOM, 1, 486},        // 64 Low Conga
    {DRUM_TOM, 1, 768},        // 65 High Timbale
    {DRUM_TOM, 1, 563},        // 66 Low Timbale
    {DRUM_WOODBLOCK, -1, 256}, // 67 High Agogo
    {DRUM_WOODBLOCK, -1, 192}, // 68 Low Agogo
    {DRUM_CLOSED_HAT, 0, 256}, // 69 Cabasa
    {DRUM_CLOSED_HAT, 0, 256}, // 70 Maracas
    {DRUM_DEFAULT, 0, 256},    // 71 Short Whistle
    {DRUM_DEFAULT, 0, 256},    // 72 Long Whistle
    {DRUM_DEFAULT, 0, 256},    // 73 Short Guiro
    {DRUM_DEFAULT, 0, 256},    // 74 Long Guiro
    {DRUM_WOODBLOCK, 0, 711},  // 75 Claves
    {DRUM_WOODBLOCK, 0, 256},  // 76 Hi Wood Block (the looper's metronome)
    {DRUM_WOODBLOCK, 0, 192},  // 77 Low Wood Block
    {DRUM_DEFAULT, 0, 256},    // 78 Mute Cuica
    {DRUM_DEFAULT, 0, 256},    // 79 Open Cuica
    {DRUM_WOODBLOCK, 0, 1152}, // 80 Mute Triangle
    {DRUM_WOODBLOCK, -2, 1152},// 81 Open Triangle
};

// Hats and cymbals play first-differenced noise (WAVE_NOISE_HP) when their
// preset is noise, so they lose the low end before the low-pass.
const bool DRUM_TYPE_HIGH_PASS[DRUM_TYPE_COUNT] = {false, false, true, true, false, true, false, false};

DrumNote drumNote(uint8_t note) {
    if (note < DRUM_NOTE_FIRST || note > DRUM_NOTE_LAST) return {DRUM_DEFAULT, 0, 256};
    return DRUM_NOTES[note - DRUM_NOTE_FIRST];
}

// Musical, not CPU, motivation (see recomputeInstrumentPresetCache()'s
// tremoloDepthQ16/pwmDepthQ16 and DEFAULT_PRESETS' tremoloDepthPercent/
// pwmDepthPercent for who has either at all -- Organ, Strings, Ensemble,
// Brass, Reed, Synth Lead, Synth Pad): several of those voices audibly
// wavering in lockstep at once (a wide chord built from any of those
// families) reads as busy/unfocused rather than lively. Capping how many
// melodic voices modulate *at once* -- not how many voices exist, or
// which families can modulate -- keeps that "alive" character on the
// first few notes of a chord without every simultaneous note wobbling
// together. Voices beyond the cap still play normally, just flat
// (v.tremoloDepthQ16/pwmDepthQ16 forced to 0 for their whole note
// lifetime, decided once at note-on like everything else about a voice's
// character -- see startMelodicVoice()'s own comment on why mid-note
// changes are avoided). Percussion never modulates at all (see
// startPercussionVoice()), so this only applies to the melodic pool.
// User-adjustable at runtime (Settings "LFO Voices" -- see
// Synth::setLfoVoices()); 0 means no voice ever modulates, regardless of
// preset. 3 is just the shipped default -- there's no objectively correct
// number, just what stops sounding cluttered to a given ear.
int g_maxModulatedVoices = 3;

// Counts currently-active melodic voices with tremolo and/or PWM already
// running, for g_maxModulatedVoices above. Excludes `exclude` itself so a
// retriggered voice (already in g_melodicVoices[], possibly already
// modulated from its previous life) doesn't count against its own past
// state when startMelodicVoice() re-decides whether it gets modulation
// this time. One-time note-on cost, not per-sample, so a plain linear
// scan over NUM_MELODIC_VOICES is fine.
int activeModulatedVoiceCount(const Voice* exclude) {
    int count = 0;
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        const Voice& v = g_melodicVoices[i];
        if (&v == exclude) continue;
        if (v.active && (v.tremoloDepthQ16 != 0 || v.pwmDepthQ16 != 0)) count++;
    }
    return count;
}

void releaseVoice(Voice& v);
void startPending(Voice& v);

// -- Channel controllers (core 1) ------------------------------------------
// All of this runs from handleMessage() or at note-on, never per sample:
// a CC, bend or pressure change works out the new per-voice gains, phaseInc
// and vibratoRange once, and renderVoice() just uses them.

const uint8_t BEND_RANGE_MAX = 24; // semitones; keeps the bend ratio under 4 (Q28 fits)
const uint32_t BEND_UNITY_Q28 = 1u << 28;
// vibratoRange * (lfoValue >> 5) has to fit int32, |lfoValue >> 5| <= 1000.
const uint32_t VIBRATO_RANGE_MAX = 2100000;

// Melodic Stereo Spread position for a channel that never sent CC10, as a
// 0..127 pan. Even channels left, odd right, fanning out as the channel
// number rises, so the common low channels sit near centre. +-96 of 255 at
// 100% spread; g_stereoSpreadPercent scales it around centre.
uint8_t spreadPan(uint8_t channel) {
    int32_t rank = channel / 2; // 0..7 -- distance from center, in steps
    int32_t side = (channel % 2 == 0) ? -1 : 1; // even = left, odd = right
    int32_t magnitude = (2 * rank + 1) * 96 / 15; // 6..96, odd steps so rank 0 isn't dead center
    int32_t pan255 = 128 + (side * magnitude * g_stereoSpreadPercent) / 100;
    return (uint8_t)(pan255 >> 1);
}

// Sets v's gains from its channel's volume, expression, pan and reverb send.
// Drums sit in the centre until CC10 moves them.
void applyChannelGains(Voice& v, bool drum) {
    const ChannelState& c = g_chan[v.channel];
    uint32_t g = ((uint32_t)GM_CURVE_Q15[c.volume] * GM_CURVE_Q15[c.expression]) >> 15; // <= 32768
    uint8_t pan = c.panSet ? c.pan : (drum ? 64 : spreadPan(v.channel));
    v.gainL = (uint16_t)((g * PAN_SIN_Q15[128 - (pan ? pan : 1)]) >> 15);
    v.gainR = (uint16_t)((g * PAN_SIN_Q15[pan]) >> 15);
    // CC91 40 (the GM default) sends 0.707 in Q14, the same as the voice's
    // own level in each channel at centre, which is what the whole mix sent
    // before per-channel sends. 9268 / 32 = 0.707 * 16384 / 40.
    uint32_t k = ((uint32_t)c.reverbSend * 9268) >> 5; // <= 36782
    v.sendGain = (uint16_t)((g * k) >> 15);
}

// Mod wheel plus channel pressure, 127 = 50 cents of vibrato (fluidsynth's
// default depth for both), as a Q16 fraction of pitch like the preset's.
uint32_t modDepthQ16(uint8_t channel) {
    uint32_t m = (uint32_t)g_chan[channel].mod + g_chan[channel].pressure;
    if (m > 127) m = 127;
    return (m * 1935) >> 7; // 127 -> 1919, 2^(50/1200) - 1
}

// See Voice::blepInv. Runs at note-on and when the pitch changes.
void setBlepInv(Voice& v) {
    uint32_t dt = v.phaseInc >> 16;
    v.blepInv = dt ? 0x80000000u / dt : 0;
}

// Melodic voices only: phaseInc from the note and the channel's bend, and
// vibratoRange from that and the preset plus mod depth.
void applyPitch(Voice& v) {
    int32_t n = (int32_t)(v.note & 0x7F) + v.transpose;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    uint32_t inc = NOTE_PHASE_INC[n];
    uint32_t r = g_chanBendQ28[v.channel];
    if (r != BEND_UNITY_Q28) {
        uint64_t bent = ((uint64_t)inc * r) >> 28;
        inc = bent > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)bent; // Nyquist
    }
    v.phaseInc = inc;
    setBlepInv(v);
    uint32_t depth = g_presetVibratoDepthQ16[v.family] + modDepthQ16(v.channel);
    // >>24, 8 bits more than Q16, so renderVoice()'s multiply stays int32.
    uint64_t range = ((uint64_t)inc * depth) >> 24;
    v.vibratoRange = range > VIBRATO_RANGE_MAX ? VIBRATO_RANGE_MAX : (uint32_t)range;
    setModFlags(v);
}

void updateBendRatio(uint8_t channel) {
    const ChannelState& c = g_chan[channel];
    int32_t rangeCents = (int32_t)c.bendRange * 100 + c.bendCents;
    if (rangeCents > BEND_RANGE_MAX * 100) rangeCents = BEND_RANGE_MAX * 100;
    int32_t cents = ((int32_t)c.bend * rangeCents) >> 13; // -2400..2399
    if (cents == 0) { g_chanBendQ28[channel] = BEND_UNITY_Q28; return; }
    uint32_t u = (uint32_t)(cents + 2400); // 0..4799, 2400 = unity
    uint32_t oct = u / 1200, rem = u - oct * 1200;
    uint32_t r30 = (uint32_t)(((uint64_t)SEMI_RATIO_Q30[rem / 100] * CENT_RATIO_Q30[rem % 100]) >> 30);
    g_chanBendQ28[channel] = r30 >> (4 - oct); // 2^(oct - 2) * r30 / 2^30, in Q28
}

void updateChannelVoices(uint8_t channel, bool gains, bool pitch) {
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice& v = g_voices[i];
        if (!v.active || v.channel != channel) continue;
        bool drum = i >= NUM_MELODIC_VOICES;
        if (gains) applyChannelGains(v, drum);
        if (pitch && !drum) applyPitch(v);
    }
}

void startRelease(Voice& v) {
    v.held = false;
    if (v.stage == ENV_RELEASE) return;
    v.stage = ENV_RELEASE;
    v.envFloor = envStageFloor(v.amplitude);
}

// A note-off for a melodic voice: held while the sustain pedal is down, or
// while sostenuto is down and it was sounding when the pedal went down.
// Note-off for a note still queued behind a steal fade: it starts held if
// a pedal holds it, else it's dropped.
void noteOffPending(Voice& v) {
    const ChannelState& c = g_chan[v.pendChan & 0x0F];
    if (c.sustain || (c.sostenuto && (v.pendChan & PEND_SOST_LATCHED))) v.pendChan |= PEND_OFF;
    else v.pendVel = 0;
}

void noteOffVoice(Voice& v) {
    const ChannelState& c = g_chan[v.channel];
    if (v.stage == ENV_RELEASE || v.held) return;
    if (c.sustain || (c.sostenuto && v.sostLatched)) v.held = true;
    else startRelease(v);
}

// After a pedal comes up: releases held voices nothing holds any more.
void releaseUnheld(uint8_t channel) {
    const ChannelState& c = g_chan[channel];
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        Voice& v = g_melodicVoices[i];
        if (v.pendVel && (v.pendChan & 0x0F) == channel && (v.pendChan & PEND_OFF) &&
            !(c.sustain || (c.sostenuto && (v.pendChan & PEND_SOST_LATCHED)))) {
            v.pendVel = 0; // its note-off came during the fade, nothing holds it now
        }
        if (!v.active || v.channel != channel || !v.held) continue;
        if (c.sustain || (c.sostenuto && v.sostLatched)) continue;
        startRelease(v);
    }
}

void setSostenuto(uint8_t channel, bool on) {
    ChannelState& c = g_chan[channel];
    if (on == c.sostenuto) return;
    c.sostenuto = on;
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        Voice& v = g_melodicVoices[i];
        if (v.pendVel && (v.pendChan & 0x0F) == channel) {
            // A queued note's key is down unless its note-off came.
            if (on && !(v.pendChan & PEND_OFF)) v.pendChan |= PEND_SOST_LATCHED;
        }
        if (!v.active || v.channel != channel) continue;
        if (on) v.sostLatched = v.stage != ENV_RELEASE && !v.held; // key still down
    }
    if (!on) {
        releaseUnheld(channel);
        for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
            Voice& v = g_melodicVoices[i];
            if (v.channel == channel) v.sostLatched = false;
            if ((v.pendChan & 0x0F) == channel) v.pendChan &= ~PEND_SOST_LATCHED;
        }
    }
}

// CC121 (full = false) resets what RP-015 lists: expression, mod,
// pressure, bend, pedals and the RPN select, leaving volume, pan, reverb
// send and bend range. A full reset (setup, new song, mode changes) also
// puts those back: volume 100, pan to Stereo Spread, range 2, send 40.
void resetChannel(uint8_t channel, bool full) {
    ChannelState& c = g_chan[channel];
    if (full) {
        c.volume = 100;
        c.pan = 64;
        c.panSet = false;
        c.bendRange = 2;
        c.bendCents = 0;
        c.reverbSend = 40;
    }
    c.expression = 127;
    c.mod = 0;
    c.pressure = 0;
    c.bend = 0;
    c.rpnMsb = c.rpnLsb = 127;
    c.soft = false;
    c.sustain = false;
    setSostenuto(channel, false);
    releaseUnheld(channel);
    g_chanBendQ28[channel] = BEND_UNITY_Q28;
    updateChannelVoices(channel, true, true);
}

void handleControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
    ChannelState& c = g_chan[channel];
    switch (cc) {
        case 1: c.mod = value; updateChannelVoices(channel, false, true); break;
        case 7: c.volume = value; updateChannelVoices(channel, true, false); break;
        case 10: c.pan = value; c.panSet = true; updateChannelVoices(channel, true, false); break;
        case 11: c.expression = value; updateChannelVoices(channel, true, false); break;
        case 91: c.reverbSend = value; updateChannelVoices(channel, true, false); break;
        case 64: {
            bool on = value >= 64;
            if (on == c.sustain) break;
            c.sustain = on;
            if (!on) releaseUnheld(channel);
            break;
        }
        case 66: setSostenuto(channel, value >= 64); break;
        case 67: c.soft = value >= 64; break;
        case 101: c.rpnMsb = value; break;
        case 100: c.rpnLsb = value; break;
        case 99: case 98: c.rpnMsb = c.rpnLsb = 127; break; // NRPN: data entry isn't ours
        case 6:
            if (c.rpnMsb == 0 && c.rpnLsb == 0) {
                c.bendRange = value > BEND_RANGE_MAX ? BEND_RANGE_MAX : value;
                c.bendCents = 0;
                updateBendRatio(channel);
                updateChannelVoices(channel, false, true);
            }
            break;
        case 38:
            if (c.rpnMsb == 0 && c.rpnLsb == 0) {
                c.bendCents = value > 99 ? 99 : value;
                updateBendRatio(channel);
                updateChannelVoices(channel, false, true);
            }
            break;
        case 120: // All Sound Off: fast release, pedals or not
            for (int i = 0; i < NUM_VOICES; i++) {
                Voice& v = g_voices[i];
                if (v.pendVel && (v.pendChan & 0x0F) == channel) v.pendVel = 0;
                if (v.channel != channel || !v.active) continue;
                // A note queued for another channel starts now, in place of
                // the fade it was waiting for.
                if (v.pendVel) { startPending(v); continue; }
                v.held = false;
                v.sostLatched = false;
                releaseVoice(v);
            }
            break;
        case 121: resetChannel(channel, false); break;
        case 123: case 124: case 125: case 126: case 127: // All Notes Off, and the mode messages that imply it
            for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
                Voice& v = g_melodicVoices[i];
                if (v.pendVel && (v.pendChan & 0x0F) == channel) noteOffPending(v);
                if (v.active && v.channel == channel) noteOffVoice(v); // a fading voice is already in release
            }
            break;
        default: break;
    }
}

void handlePitchBend(uint8_t channel, uint16_t bend14) {
    int16_t bend = (int16_t)((int32_t)(bend14 & 0x3FFF) - 8192);
    if (bend == g_chan[channel].bend) return;
    g_chan[channel].bend = bend;
    updateBendRatio(channel);
    updateChannelVoices(channel, false, true);
}

void handleChannelPressure(uint8_t channel, uint8_t pressure) {
    if (pressure == g_chan[channel].pressure) return;
    g_chan[channel].pressure = pressure;
    updateChannelVoices(channel, false, true);
}

// program -1 takes the channel's current program.
void startMelodicVoice(Voice& v, uint8_t channel, uint8_t note, uint8_t velocity, int program = -1) {
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

    if (program < 0) program = g_channelProgram[channel];
    program &= 0x7F;
    uint8_t family = (uint8_t)program >> 3;
    const InstrumentPreset& preset = PRESETS[family];
    uint8_t tweak = PROGRAM_TWEAK[program];

    v.active = true;
    v.note = note;
    v.channel = channel;
    v.family = family;
    v.held = false;
    v.sostLatched = false;
    v.startOrder = ++g_voiceStartCounter;
    v.waveform = (tweak >> 5) ? (Waveform)((tweak >> 5) - 1) : preset.waveform;
    if (!wasActive) { v.phase = 0; v.filterState = 0; }
    v.pitchDrop = 0;
    v.pitchDropStep = 0;
    v.transpose = (tweak & 3) == 1 ? 12 : (tweak & 3) == 2 ? -12 : 0;
    applyPitch(v); // phaseInc and vibratoRange, with the channel's bend and mod
    v.tremoloDepthQ16 = g_presetTremoloDepthQ16[family];
    // PWM only means something on a square.
    v.pwmDepthQ16 = v.waveform == WAVE_SQUARE ? g_presetPwmDepthQ16[family] : 0;
    // See g_maxModulatedVoices' own comment: cap how many voices wobble at
    // once, not which families are allowed to. A voice that already has
    // neither (the common case -- most families use neither effect) skips
    // the scan entirely, since there's nothing to cap.
    if ((v.tremoloDepthQ16 != 0 || v.pwmDepthQ16 != 0) &&
        activeModulatedVoiceCount(&v) >= g_maxModulatedVoices) {
        v.tremoloDepthQ16 = 0;
        v.pwmDepthQ16 = 0;
    }
    setModFlags(v);
    applyChannelGains(v, false);

    // CC67 soft pedal: about 0.7 of the velocity.
    if (g_chan[channel].soft) velocity = (uint8_t)((velocity * 179) >> 8);

    // Cutoff: the program's offset, plus half key tracking around middle C
    // and velocity (+3 semitones at 127, -12 at 1) unless the preset's
    // filter is off. With no change the preset's exact alpha is kept.
    int32_t delta = ((int32_t)(int8_t)(uint8_t)(tweak << 3) >> 5) * 4;
    uint8_t semi = g_presetCutoffSemi[family];
    if (semi != CUTOFF_UNTRACKED) {
        delta += ((int32_t)(v.note & 0x7F) + v.transpose - 60) / 2 + (((int32_t)(velocity & 0x7F) - 100) >> 3);
    }
    if (delta == 0) {
        v.filterAlpha = (uint16_t)g_presetFilterAlpha[family];
    } else {
        int32_t idx = (semi == CUTOFF_UNTRACKED ? CUTOFF_SEMI_COUNT - 1 : semi) + delta;
        if (idx < 0) idx = 0;
        if (idx > CUTOFF_SEMI_COUNT - 1) idx = CUTOFF_SEMI_COUNT - 1;
        v.filterAlpha = CUTOFF_ALPHA_Q14[idx];
    }

    int32_t peak = ((int32_t)VOICE_PEAK * GM_CURVE_Q15[velocity & 0x7F]) >> 15;
    // A program's own waveform plays at the family waveform's RMS level.
    if (v.waveform != preset.waveform) peak = peak * WAVE_RMS[preset.waveform] / WAVE_RMS[v.waveform];
    if (peak < 1) peak = 1;
    v.peakLevel = peak << 16;
    v.sustainLevel = ((peak * preset.sustainPercent) / 100) << 16;
    v.decayK = g_presetDecayK[family];
    v.releaseK = g_presetReleaseK[family];

    // Linear attack from wherever the voice is.
    v.stage = ENV_ATTACK;
    v.amplitude = startAmplitude;
    uint16_t atk = preset.attackSamples > 0 ? preset.attackSamples : 1;
    v.envStep = (v.peakLevel - startAmplitude) / atk;
    if (v.envStep == 0) v.envStep = (v.peakLevel >= startAmplitude) ? 1 : -1;
}

void startPercussionVoice(Voice& v, uint8_t note, uint8_t velocity) {
    DrumNote dn = drumNote(note);
    DrumType drumType = (DrumType)dn.type;
    const DrumPreset& p = DRUM_PRESETS[drumType];

    // A retriggered or stolen voice keeps its phase and filter state and
    // ramps from its current amplitude, so the cut-off hit doesn't click.
    int32_t startAmplitude = v.active ? v.amplitude : 0;
    if (!v.active) { v.phase = 0; v.filterState = 0; }
    v.active = true;
    v.note = note;
    v.channel = PERCUSSION_CHANNEL;
    v.startOrder = ++g_voiceStartCounter;
    Waveform wave = (p.waveform == WAVE_NOISE && DRUM_TYPE_HIGH_PASS[drumType]) ? WAVE_NOISE_HP : p.waveform;
    // The high-passed noise keeps its last value in phase, so a change of
    // waveform on a retriggered or stolen voice starts phase from 0.
    if (wave != v.waveform) v.phase = 0;
    v.waveform = wave;
    v.transpose = 0;
    v.filterAlpha = (uint16_t)g_drumFilterAlpha[drumType];
    v.held = false;
    v.sostLatched = false;
    applyChannelGains(v, true); // centred unless channel 10 sent CC10
    v.phaseInc = g_drumPhaseInc[drumType];
    v.pitchDrop = g_drumPitchDrop[drumType];
    v.pitchDropStep = g_drumPitchDropStep[drumType];
    if (dn.ratioQ8 != 256) {
        // >> 8 first keeps it in 32 bits; the ratio is under 8.
        v.phaseInc = (v.phaseInc >> 8) * dn.ratioQ8;
        v.pitchDrop = (v.pitchDrop >> 8) * dn.ratioQ8;
        v.pitchDropStep = (v.pitchDropStep * (int32_t)dn.ratioQ8) >> 8;
        if (v.pitchDrop > 0 && v.pitchDropStep == 0) v.pitchDropStep = -1;
    }
    setBlepInv(v);
    setModFlags(v);

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
    int32_t peak = ((int32_t)VOICE_PEAK * GM_CURVE_Q15[velocity & 0x7F]) >> 15;
    if (peak < 1) peak = 1;
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
    v.peakLevel = peak << 16;
    v.sustainLevel = 0; // one-shot: decays fully to silence, ignores note-off
    uint32_t k = g_drumDecayK[drumType];
    if (dn.decayShift > 0) k <<= dn.decayShift;
    else if (dn.decayShift < 0) k >>= -dn.decayShift;
    if (g_drumDecayK[drumType] != 0 && k == 0) k = 1;
    v.decayK = (uint16_t)(k > 65535 ? 65535 : k);
    v.releaseK = 0; // unused, see handleNoteOff(); releaseVoice() sets its own

    v.stage = ENV_ATTACK;
    v.amplitude = startAmplitude;
    // Fast but not instantaneous attack: real percussion transients are
    // near-instant, but a true single-sample jump to full amplitude is
    // exactly the kind of discontinuity that clicks, especially at high
    // velocity. ~10 samples (0.2ms) is imperceptible as a ramp. Can be a
    // downward ramp when a loud hit is retriggered softer.
    v.envStep = (v.peakLevel - startAmplitude) / 10;
    if (v.envStep == 0) v.envStep = (v.peakLevel >= startAmplitude) ? 1 : -1;
}

// Voice to steal when a pool is full: the quietest voice already in
// release, else the oldest. Note-on only, so a linear scan is fine.
// Voices already fading for a steal are skipped unless every one is.
Voice& pickStealVoice(Voice* pool, int count) {
    Voice* quietestRelease = nullptr;
    Voice* oldest = nullptr;
    Voice* quietestPending = nullptr;
    uint32_t oldestAge = 0;
    for (int i = 0; i < count; i++) {
        Voice& v = pool[i];
        if (v.pendVel) {
            if (!quietestPending || v.amplitude < quietestPending->amplitude) quietestPending = &v;
            continue;
        }
        if (v.stage == ENV_RELEASE &&
            (!quietestRelease || v.amplitude < quietestRelease->amplitude)) {
            quietestRelease = &v;
        }
        uint32_t age = g_voiceStartCounter - v.startOrder;
        if (!oldest || age > oldestAge) { oldestAge = age; oldest = &v; }
    }
    if (quietestRelease) return *quietestRelease;
    if (oldest) return *oldest;
    // Every voice already has a note queued: one of them has to go, so the
    // newest note takes the queue of the voice furthest through its fade.
    return *quietestPending;
}

// envTimeToK(32): 40 dB down in 32 samples, -60 dB (the release floor) in about 48.
const uint16_t STEAL_FADE_K = 8784;

// Fades a stolen voice out quickly and queues the new note on it, so the
// old note's waveform and filter never jump. renderVoice() calls
// startPending() when the fade ends.
void startStealFade(Voice& v, uint8_t channel, uint8_t note, uint8_t velocity) {
    PROF_WHERE1(Synth::PW_STEAL);
    v.pendNote = note;
    v.pendVel = velocity;
    v.pendChan = channel;
    v.pendProg = g_channelProgram[channel];
    v.held = false;
    v.sostLatched = false;
    v.stage = ENV_RELEASE;
    v.releaseK = STEAL_FADE_K;
    v.envFloor = envStageFloor(v.amplitude);
}

void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    PROF_WHERE1(Synth::PW_NOTE_ON);
    if (channel == PERCUSSION_CHANNEL) {
        // Closed and pedal hi-hat choke the open hi-hat.
        if (note == 42 || note == 44) {
            for (int i = 0; i < NUM_DRUM_VOICES; i++) {
                Voice& d = g_drumVoices[i];
                if (d.active && d.note == 46 && d.stage != ENV_RELEASE) {
                    d.stage = ENV_RELEASE;
                    d.releaseK = CHOKE_RELEASE_K;
                    d.envFloor = envStageFloor(d.amplitude);
                }
            }
        }
        for (int i = 0; i < NUM_DRUM_VOICES; i++) {
            if (g_drumVoices[i].active && g_drumVoices[i].note == note && !g_drumVoices[i].pendVel) {
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
        // Drum voices only enter release when choked, so this mostly takes the oldest hit.
        startStealFade(pickStealVoice(g_drumVoices, NUM_DRUM_VOICES), channel, note, velocity);
        return;
    }

    // Retrigger if this note is already sounding on this channel, else
    // take a free voice, else steal one (see pickStealVoice()).
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        Voice& v = g_melodicVoices[i];
        if (v.active && v.note == note && v.channel == channel && !v.pendVel) {
            // A retriggered note keeps its sostenuto latch while the pedal is down.
            bool latch = v.sostLatched && g_chan[channel].sostenuto;
            startMelodicVoice(v, channel, note, velocity);
            v.sostLatched = latch;
            return;
        }
    }
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        if (!g_melodicVoices[i].active) {
            startMelodicVoice(g_melodicVoices[i], channel, note, velocity);
            return;
        }
    }
    startStealFade(pickStealVoice(g_melodicVoices, NUM_MELODIC_VOICES), channel, note, velocity);
}

// Releases `note` on `channel` only, so two parts sharing a pitch don't
// cut each other off. Percussion voices are never searched: they ignore
// note-off (see startPercussionVoice()) and live in a separate pool.
void handleNoteOff(uint8_t channel, uint8_t note) {
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) {
        Voice& v = g_melodicVoices[i];
        if (v.pendVel && v.pendNote == note && (v.pendChan & 0x0F) == channel) noteOffPending(v);
        else if (v.active && v.note == note && v.channel == channel) noteOffVoice(v);
    }
}

// Starts the note a steal queued once the old one has faded (see
// startStealFade()), from silence. Called from renderVoice(), so it stays
// out of line; it only runs once per steal.
__attribute__((noinline)) void startPending(Voice& v) {
    PROF_WHERE1(Synth::PW_START_PENDING);
    uint8_t channel = v.pendChan & 0x0F;
    bool off = (v.pendChan & PEND_OFF) != 0;
    bool latched = (v.pendChan & PEND_SOST_LATCHED) != 0;
    uint8_t note = v.pendNote, velocity = v.pendVel;
    v.pendVel = 0;
    v.active = false;
    v.amplitude = 0;
    if (channel == PERCUSSION_CHANNEL) {
        startPercussionVoice(v, note, velocity);
    } else {
        startMelodicVoice(v, channel, note, velocity, v.pendProg);
        v.sostLatched = latched && g_chan[channel].sostenuto;
        if (off) noteOffVoice(v); // a pedal held it, so it goes to held
    }
}

void releaseVoice(Voice& v) {
    v.held = false;
    v.pendVel = 0;
    if (!v.active) return;
    v.stage = ENV_RELEASE;
    v.releaseK = PANIC_RELEASE_K;
    v.envFloor = envStageFloor(v.amplitude);
}

void handlePanic() {
    for (int i = 0; i < NUM_MELODIC_VOICES; i++) releaseVoice(g_melodicVoices[i]);
    for (int i = 0; i < NUM_DRUM_VOICES; i++) releaseVoice(g_drumVoices[i]);
}

// Renders one voice's current sample and advances its phase/envelope by
// one sample. Returns its contribution to the mix (0 if inactive). Shared
// by both voice pools so the envelope state machine only exists once.
ALWAYS_INLINE void __not_in_flash_func(renderVoice)(Voice& v, int16_t lfoValue, int32_t& outL, int32_t& outR, int32_t& outSend) {
    if (!v.active) { outL = 0; outR = 0; outSend = 0; return; }

    // One flag test skips vibrato, PWM, tremolo and pitch drop for a voice
    // that has none. Tremolo and PWM are on at most g_maxModulatedVoices
    // voices; vibrato can be on every voice (the mod wheel), and so can
    // pitch drop on the drums.
    uint32_t effectivePhaseInc = v.phaseInc;
    uint16_t pulseWidth = 32768;
    uint8_t mod = v.modFlags;
    if (mod) {
        // pitchDrop (percussion pitch sweep, >= 0) and the shared vibrato
        // LFO (either sign) nudge phaseInc. Plain uint32 adds: a negative
        // nudge wraps to the right value since the sum itself stays in range.
        if (mod & VOICE_MOD_PITCH_DROP) {
            effectivePhaseInc += (uint32_t)v.pitchDrop;
            v.pitchDrop += v.pitchDropStep;
            if (v.pitchDrop <= 0) {
                v.pitchDrop = 0;
                v.pitchDropStep = 0;
                v.modFlags = mod & ~VOICE_MOD_PITCH_DROP;
            }
        }
        if (mod & VOICE_MOD_VIBRATO) {
            // lfoValue is g_sineTable's output, ~-32000..32000. vibratoRange
            // already lost 8 bits at note-start, so the remaining >>7 is
            // split into >>5 on lfoValue before the multiply and >>2 after,
            // keeping it a plain int32 multiply: at note 127 and the
            // editor's 30% cap vibratoRange is ~1.43e6, times |lfo>>5| <=
            // 1000 is ~1.43e9.
            effectivePhaseInc += (uint32_t)(((int32_t)v.vibratoRange * (lfoValue >> 5)) >> 2);
        }
        // PWM: the shared LFO swings the square's duty cycle around 50%.
        // pwmDepthQ16 is at most ~15%, so the multiply fits int32.
        if (mod & VOICE_MOD_PWM) {
            int32_t swing = ((int32_t)v.pwmDepthQ16 * lfoValue) >> 15;
            pulseWidth = (uint16_t)(int32_t)(32768 + swing);
        }
    }
    // The phase moves on before the waveform is worked out, which frees a
    // register for the rest. Noise voices (phaseInc 0) keep their last
    // value in phase, see waveformSample().
    uint32_t phase = v.phase;
    v.phase = phase + effectivePhaseInc;
    int32_t raw = waveformSample(v, phase, pulseWidth);

    // One-pole low-pass, Q14 (see cutoffHzToAlphaQ14()), on the raw
    // oscillator before the envelope. diff up to 65535 times alpha up to
    // 16384 fits int32. A fully open filter just passes raw through.
    int32_t alpha = v.filterAlpha;
    int32_t filtered = raw;
    if (alpha != 16384) filtered = v.filterState + (((raw - v.filterState) * alpha) >> 14);
    v.filterState = filtered;

    // amplitude is Q16: >>13 here and >>14 after is the same >>(16 +
    // VOICE_PEAK_SHIFT) as one shift, kept in int32 (32767 * 23040 max).
    int32_t contribution = (filtered * (v.amplitude >> 13)) >> (VOICE_PEAK_SHIFT + 3);
    // Tremolo: a gain wobble around unity after the envelope. contribution
    // is shifted down 2 bits first so the Q16 gain (up to ~77332) multiply
    // stays int32, and the final shift is 14 to make up for it.
    if (mod & VOICE_MOD_TREMOLO) {
        int32_t tremMod = ((int32_t)v.tremoloDepthQ16 * lfoValue) >> 15;
        contribution = ((contribution >> 2) * (65536 + tremMod)) >> 14;
    }
    // Channel gains (see applyChannelGains()). contribution is under ~46100
    // (drum peak 2880), so Q15 gains and the Q14 send (<= 36782) fit int32.
    outL = (contribution * (int32_t)v.gainL) >> 15;
    outR = (contribution * (int32_t)v.gainR) >> 15;
    outSend = (contribution * (int32_t)v.sendGain) >> 14;

    switch (v.stage) {
        case ENV_ATTACK:
            v.amplitude += v.envStep;
            if ((v.envStep >= 0 && v.amplitude >= v.peakLevel) ||
                (v.envStep < 0 && v.amplitude <= v.peakLevel)) {
                v.amplitude = v.peakLevel;
                if (v.decayK == 0) {
                    v.amplitude = v.sustainLevel;
                    if (v.sustainLevel <= 0) { v.active = false; }
                    else { v.stage = ENV_SUSTAIN; }
                } else {
                    v.stage = ENV_DECAY;
                    v.envFloor = envStageFloor(v.peakLevel - v.sustainLevel);
                }
            }
            break;
        case ENV_DECAY: {
            // Exponential towards sustain, ends at envFloor short of it.
            int32_t dist = envExpStep(v.amplitude - v.sustainLevel, v.decayK);
            if (dist <= v.envFloor) {
                v.amplitude = v.sustainLevel;
                if (v.sustainLevel <= 0) { v.active = false; }
                else { v.stage = ENV_SUSTAIN; }
            } else {
                v.amplitude = v.sustainLevel + dist;
            }
            break;
        }
        case ENV_SUSTAIN:
            break; // held
        case ENV_RELEASE:
            v.amplitude = envExpStep(v.amplitude, v.releaseK);
            if (v.amplitude <= v.envFloor) {
                v.amplitude = 0;
                v.active = false;
                if (v.pendVel) startPending(v); // a steal's fade is done
            }
            break;
    }
}

// Soft knee above SOFT_KNEE_START: an unusually loud/dense mix (many
// voices summing near full scale) compresses gently instead of hard-
// clipping, which is audible as harsh crackling/popping -- exactly what
// too little headroom caused before. Below the knee, level is untouched.
int32_t __not_in_flash_func(softLimit)(int32_t mix) {
    int32_t sign = (mix < 0) ? -1 : 1;
    int32_t mag = mix * sign;
    if (mag > SOFT_KNEE_START) {
        mag = SOFT_KNEE_START + (mag - SOFT_KNEE_START) / 4;
    }
    if (mag > HARD_LIMIT) mag = HARD_LIMIT;
    return mag * sign;
}

// -- Synth output -------------------------------------------------------------
// The synth's mix goes straight to the output through one gain (volume,
// output level and SYNTH_MAKEUP_Q8) and is then soft-limited against a
// ceiling in output units: the DAC's full scale, or SYNTH_HEADROOM times
// what the old pre-volume limiter allowed at this volume and level, if
// that's lower. So at the default Headphone Low and volume 80 the synth
// can peak at -12 dBFS (it was held at -24 dBFS), and a typical file never
// reaches the knee. Past about volume 63 on Line or 80 on Headphone High
// the ceiling is full scale itself and loudness has nowhere to go, so a
// peak limiter (synthLevelerStep()) runs on the synth then and rides the
// gain down instead of the soft limit flattening the peaks. WAV and tracker
// playback keep their own leveler and limiter before the volume (see
// renderSample()).
const int32_t SYNTH_MAKEUP_Q8 = 256;
const int32_t SYNTH_HEADROOM = 4;
int32_t g_synthGainHi = 0, g_synthGainLo = 0; // Q16 gain split in bytes
int32_t g_synthKnee = SOFT_KNEE_START, g_synthCeiling = HARD_LIMIT;
bool g_synthCapped = false; // ceiling at full scale, the synth leveler runs

// Attenuate-only gain riding for the synth while its ceiling is at full
// scale, see synthLevelerStep().
struct Leveler {
    int32_t envelope;
    uint32_t gainQ16;
    int counter;
};
Leveler g_synthLeveler = {0, 65536, 0};

// The synth's leveler is a peak limiter: the follower jumps to each new
// peak and falls back in ~186 ms, and the gain, worked out every
// SYNTH_LEVELER_PERIOD samples, holds peaks at the limiter's knee (the
// soft limit only catches what gets past between updates).
const int SYNTH_LEVELER_PERIOD = 16;
ALWAYS_INLINE void __not_in_flash_func(synthLevelerStep)(Leveler& l, int32_t magnitude) {
    if (magnitude > l.envelope) l.envelope = magnitude;
    else l.envelope -= l.envelope >> 13;
    if (++l.counter >= SYNTH_LEVELER_PERIOD) {
        l.counter = 0;
        // A knee of at most 24576, << 16, fits uint32.
        l.gainQ16 = l.envelope > g_synthKnee ? ((uint32_t)g_synthKnee << 16) / (uint32_t)l.envelope : 65536;
    }
}

// x times the leveler's gain (< 65536), split in bytes to stay in int32.
ALWAYS_INLINE int32_t levelerApply(const Leveler& l, int32_t x) {
    int32_t hi = (int32_t)(l.gainQ16 >> 8), lo = (int32_t)(l.gainQ16 & 0xFF);
    return ((x * hi) >> 8) + ((x * lo) >> 16);
}

// After a volume or output level change (core 1, not per sample). While
// core 0 renders offline the synth takes Synth::OFFLINE_VOLUME on Line
// instead; the settings stay in g_masterGainQ16 and g_outputLevelShift.
void updateSynthOutput() {
    uint32_t master = g_masterGainQ16;
    uint8_t shift = g_outputLevelShift;
    if (g_offlineOwned) {
        master = volumePercentToGainQ16(Synth::OFFLINE_VOLUME);
        shift = OUTPUT_LEVEL_SHIFT[OUTPUT_LEVEL_LINE];
    }
    uint32_t g = ((master * (uint32_t)SYNTH_MAKEUP_Q8) >> 8) >> shift;
    g_synthGainHi = (int32_t)(g >> 8);
    g_synthGainLo = (int32_t)(g & 0xFF);
    uint32_t ceiling = (((uint32_t)(SYNTH_HEADROOM * HARD_LIMIT) * (master >> 4)) >> 12) >> shift;
    if (ceiling > (uint32_t)HARD_LIMIT) ceiling = HARD_LIMIT;
    g_synthCeiling = (int32_t)ceiling;
    g_synthKnee = (int32_t)((ceiling * (uint32_t)SOFT_KNEE_START) / (uint32_t)HARD_LIMIT);
    bool capped = ceiling == (uint32_t)HARD_LIMIT;
    if (capped != g_synthCapped) g_synthLeveler = {0, 65536, 0};
    g_synthCapped = capped;
}

// softLimit()'s curve, at g_synthKnee and g_synthCeiling.
ALWAYS_INLINE int32_t __not_in_flash_func(synthLimit)(int32_t x) {
    int32_t mag = x < 0 ? -x : x;
    if (mag <= g_synthKnee) return x;
#ifdef HOST_MIDI
    g_hostLimitSamples++;
#endif
    mag = g_synthKnee + ((mag - g_synthKnee) >> 2);
    if (mag > g_synthCeiling) mag = g_synthCeiling;
    return x < 0 ? -mag : mag;
}

// -- Lo-fi reverb -----------------------------------------------------------
// A small Freeverb-style tank (4 parallel comb filters, two of them into
// an allpass for each side) fed from the onboard MIDI synth's own voices only --
// see renderSample()'s sendSum, built from the voices before WAV/tracker
// content is mixed in -- so a WAV or tracker file playing back never reaches the
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
// Positions (combs 0-3, then allpasses 0-1) count down, and the comb
// low-pass states are Q4. One struct so the tank reaches them all from one
// base address.
struct ReverbState {
    uint32_t pos[6];
    int32_t damp[4];
};
ReverbState g_reverbState = {};

// Q16 fixed point. FEEDBACK sets decay time (higher = longer tail);
// DAMP_ALPHA is a one-pole lowpass *inside* each comb's feedback path,
// same technique as the synth voices' own filterAlpha -- real rooms
// absorb high frequencies fastest, so damping the feedback (not the
// output) is what gives the tail a warm, closing-in-on-itself decay
// instead of ringing brightly forever.
const int32_t REVERB_COMB_FEEDBACK_Q16 = 53740;  // ~0.82
const int32_t REVERB_DAMP_ALPHA_Q16 = 13107;      // ~0.2
const int32_t REVERB_COMB_FEEDBACK_Q12 = REVERB_COMB_FEEDBACK_Q16 >> 4;
const int32_t REVERB_DAMP_ALPHA_Q12 = REVERB_DAMP_ALPHA_Q16 >> 4;
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
// ring message, same convention as g_masterGainQ16/g_outputLevelShift.
// Reverb defaults on (this whole effect exists to be heard) at a mix
// roughly matching what shipped before these became adjustable.
bool g_reverbEnabled = true;
uint32_t g_reverbMixQ16 = 0; // set from the default percent in setup1()

// Which reverb algorithm renderSample()'s reverb block runs, set from the
// Settings "Reverb Type" control (see Synth::setReverbType()) -- same
// write-only-from-loop1()-via-ring convention as g_reverbEnabled above.
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
// A multiply by a constant that GCC would otherwise expand into a chain of
// shifts and adds; the M0+'s muls takes one cycle.
ALWAYS_INLINE int32_t mulConst(int32_t x, int32_t k) {
    asm("" : "+r"(k));
    return x * k;
}

ALWAYS_INLINE int32_t __not_in_flash_func(reverbComb)(int16_t* buf, uint32_t& pos, int len, int32_t input, int32_t& dampState) {
    if (pos == 0) pos = len;
    pos--;
    int32_t delayed = buf[pos];
    // dampState is Q4 and the feedback multiply rounds towards zero, so the
    // loop decays to 0 instead of sticking on an offset (the wet path's
    // gain makes that audible). Q12 coefficients keep the products in int32
    // (|dampState| < 2^19.1).
    dampState += mulConst((delayed << 4) - dampState, REVERB_DAMP_ALPHA_Q12) >> 12;
    int32_t f = mulConst(dampState, REVERB_COMB_FEEDBACK_Q12);
    int32_t fedBack = input + ((f + ((f >> 31) & 0xFFFF)) >> 16);
#ifdef HOST_MIDI
    if (fedBack > 32767 || fedBack < -32768) g_hostTankClips++;
#endif
    buf[pos] = (int16_t)clampInt16(fedBack);
    return delayed;
}

ALWAYS_INLINE int32_t __not_in_flash_func(reverbAllpass)(int16_t* buf, uint32_t& pos, int len, int32_t input) {
    if (pos == 0) pos = len;
    pos--;
    int32_t bufOut = buf[pos];
    int32_t output = bufOut - input;
    static_assert(REVERB_ALLPASS_FEEDBACK_Q16 == 32768, "allpass feedback is a halving");
    int32_t fedBack = input + bufOut / 2; // towards zero, see reverbComb()
    buf[pos] = (int16_t)clampInt16(fedBack);
    return output;
}

// Runs the tank every sample (see this block's header comment on why) and
// gives the full-rate wet signal in stereo: combs 0 and 2 through the first
// allpass for the left, 1 and 3 through the second for the right. The
// caller applies the Lo-fi, Lush or Shimmer treatment.
void __not_in_flash_func(reverbProcess)(int32_t input, int32_t& outL, int32_t& outR) {
    ReverbState& r = g_reverbState;
    int32_t c0 = reverbComb(g_reverbCombBuf0, r.pos[0], REVERB_COMB_LEN0, input, r.damp[0]);
    int32_t c1 = reverbComb(g_reverbCombBuf1, r.pos[1], REVERB_COMB_LEN1, input, r.damp[1]);
    int32_t c2 = reverbComb(g_reverbCombBuf2, r.pos[2], REVERB_COMB_LEN2, input, r.damp[2]);
    int32_t c3 = reverbComb(g_reverbCombBuf3, r.pos[3], REVERB_COMB_LEN3, input, r.damp[3]);
    outL = reverbAllpass(g_reverbAllpassBuf0, r.pos[4], REVERB_ALLPASS_LEN0, (c0 + c2) >> 1);
    outR = reverbAllpass(g_reverbAllpassBuf1, r.pos[5], REVERB_ALLPASS_LEN1, (c1 + c3) >> 1);
}

// The tank runs this many bits below the send sum, see renderSample().
const int REVERB_SEND_SHIFT = 5;
const int REVERB_DECIMATE = 4; // hold the wet output for this many samples -- ~11kHz effective
// Keeps roughly the top 10 bits of the tank (0x3F at the send's own scale),
// crude bitcrush grit.
const int32_t REVERB_BIT_MASK = ~(0x3F >> REVERB_SEND_SHIFT);
int32_t g_reverbHeldL = 0, g_reverbHeldR = 0;
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
// Points into g_reverbDelayBuf only while Reverb Type is Lush, nullptr
// otherwise (see reverbTypeChanged()).
int16_t* g_reverbChorusBuf = nullptr;
size_t g_reverbChorusPos = 0;
const int32_t REVERB_CHORUS_BASE_Q8 = 300 << 8;  // ~6.8ms center delay
const int32_t REVERB_CHORUS_DEPTH_Q8 = 100 << 8; // +-~2.3ms sweep

int32_t __not_in_flash_func(chorusProcess)(int32_t tankWet) {
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

// Lush and Shimmer never run together, so their delay lines share one
// static buffer sized for the larger. Static so core 1 never allocates.
union ReverbDelayBuf {
    int16_t chorus[REVERB_CHORUS_BUF_LEN];
    int16_t shimmer[SHIMMER_BUF_LEN];
};
ReverbDelayBuf g_reverbDelayBuf;

// Points into g_reverbDelayBuf only while Reverb Type is Shimmer.
int16_t* g_shimmerBuf = nullptr;
size_t g_shimmerWritePos = 0;
int32_t g_shimmerReadPosQ16[2] = {0, 0};
int32_t g_shimmerGrainPos[2] = {0, SHIMMER_GRAIN_LEN / 2}; // half a grain apart, so their crossfades interleave
int32_t g_shimmerFeedbackHeld = 0;
int32_t g_shimmerDcQ8 = 0; // DC in the feedback, see renderSample()

int32_t __not_in_flash_func(shimmerReadTap)(int tapIdx) {
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

int32_t __not_in_flash_func(shimmerProcess)(int32_t tankWet) {
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

// Points g_reverbChorusBuf/g_shimmerBuf at the shared g_reverbDelayBuf for
// the new type and clears the buffer and that type's cursors, so it starts
// silent. Called only from loop1()'s MSG_TYPE_REVERB_TYPE handler on core
// 1, between samples, so it never races renderSample().
void reverbTypeChanged(uint8_t newType) {
    g_reverbChorusBuf = nullptr;
    g_shimmerBuf = nullptr;
    if (newType == REVERB_TYPE_LUSH) {
        memset(g_reverbDelayBuf.chorus, 0, sizeof(g_reverbDelayBuf.chorus));
        g_reverbChorusBuf = g_reverbDelayBuf.chorus;
        g_reverbChorusPos = 0;
        g_reverbChorusLfoPhase = 0;
    } else if (newType == REVERB_TYPE_SHIMMER) {
        memset(g_reverbDelayBuf.shimmer, 0, sizeof(g_reverbDelayBuf.shimmer));
        g_shimmerBuf = g_reverbDelayBuf.shimmer;
        g_shimmerWritePos = 0;
        g_shimmerReadPosQ16[0] = 0;
        g_shimmerReadPosQ16[1] = 0;
        g_shimmerGrainPos[0] = 0;
        g_shimmerGrainPos[1] = SHIMMER_GRAIN_LEN / 2;
    }
    // Cleared on the way in and out of Shimmer: only Shimmer reads it, and
    // the MP3 block output's reverb sleep check needs it at 0.
    g_shimmerFeedbackHeld = 0;
    g_shimmerDcQ8 = 0;
    g_reverbType = newType;
}

// Fills outLeft/outRight with this sample's mixed output. Each synth
// voice contributes to mixL/mixR according to its own gainL/gainR (its
// channel's CC7, CC11 and CC10, see applyChannelGains()), and a WAV stream
// (genuinely stereo -- see popWavFrame()) adds in on top of that; softLimit()/
// the leveler/volume then apply per-channel.
#ifdef MP3_PLAYER
// Core 1, while an MP3 is loaded (see loop1Mp3()). g_mp3Block: renderSample()
// runs in block output. g_mp3NoVoices: no voice was active when the block
// started, so the voice loop is skipped (voices only start from messages,
// which are drained at the block's start).
// The reverb sleeps after REVERB_SLEEP_SAMPLES samples with no send, all
// comb damp states 0 and tank and wet outputs 0. That long with no input
// means every comb and allpass slot was read as 0 and written back 0, and
// the Lush and Shimmer buffers (2048 at most) hold only zeros, so the
// reverb would go on giving 0. Only the Lush LFO and Lo-fi decimation
// phases stop while it sleeps.
bool g_mp3Block = false;
bool g_mp3NoVoices = false;
bool g_reverbAsleep = false;
uint32_t g_reverbQuiet = 0;
const uint32_t REVERB_SLEEP_SAMPLES = 8192;
#endif

// One WAV leveler step with no WAV playing (magnitude 0), the same maths
// as in renderSample(). Out of line: it only runs for a few seconds after
// a WAV or tracker file ends.
__attribute__((noinline)) void __not_in_flash_func(wavLevelerIdle)() {
    g_levelerEnvelope += (0 - g_levelerEnvelope) >> 17;
    if (++g_levelerUpdateCounter >= LEVELER_UPDATE_PERIOD) {
        g_levelerUpdateCounter = 0;
        if (g_levelerEnvelope > LEVELER_TARGET) {
            g_levelerGainQ16 = ((uint32_t)LEVELER_TARGET << 16) / (uint32_t)g_levelerEnvelope;
        } else {
            g_levelerGainQ16 = 65536;
        }
    }
}

void __not_in_flash_func(renderSample)(int16_t& outLeft, int16_t& outRight) {
    // One shared LFO lookup for this whole sample -- see g_lfoPhase's
    // comment -- rather than each voice maintaining (and advancing) its
    // own phase. g_lfoEnabled false (Settings "LFO Rate" == Off) leaves
    // lfoValue at a constant 0 instead -- see g_lfoEnabled's own comment
    // for why that, not freezing the phase, is what actually disables
    // vibrato/tremolo/PWM.
    wavStreamApplyReset();

    int16_t lfoValue = 0;
    if (g_lfoEnabled) {
        lfoValue = g_sineTable[(g_lfoPhase >> 24) & (SINE_TABLE_SIZE - 1)];
        g_lfoPhase += g_lfoPhaseInc;
    }

    // sendSum is the reverb send, each voice at its channel's CC91. Only
    // synth voices feed it, so WAV and tracker playback never reach the tank.
    int32_t mixL = 0, mixR = 0, sendSum = 0;
#ifdef MP3_PLAYER
    if (!g_mp3NoVoices)
#endif
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!g_voices[i].active) continue;
        PROF_WHERE1(Synth::PW_VOICE | i);
        int32_t l, r, snd;
        renderVoice(g_voices[i], lfoValue, l, r, snd);
#ifdef __thumb__
        // The sums live in high registers (add to one is a single cycle on
        // the M0+); left to itself GCC spills two of them to the stack.
        asm("add %0, %1" : "+h"(mixL) : "l"(l));
        asm("add %0, %1" : "+h"(mixR) : "l"(r));
        asm("add %0, %1" : "+h"(sendSum) : "l"(snd));
#else
        mixL += l; mixR += r; sendSum += snd;
#endif
    }

    PROF_WHERE1(Synth::PW_MIX);
    // WAV and tracker frames keep their own path below.
    bool wavLive = g_wavActive;
    int32_t wavL = 0, wavR = 0;
    if (wavLive) {
        int16_t wl, wr;
        if (popWavFrame(wl, wr)) {
            wavL = wl;
            wavR = wr;
        } else if (g_wavEnded) {
            // Core 0 may have written its last frames just before the end
            // flag, so look at head again after seeing it.
            __dmb(); // acquire: g_wavEnded read before head
            if (g_wavHead == g_wavTail) g_wavActive = false; // fully drained after EOF -- stop checking every sample
        } else {
            g_wavUnderrun = true;
            g_wavUnderrunSamples++;
        }
    }

    // Reverb: the per-channel send sum (see sendSum above) feeds the tank
    // REVERB_SEND_SHIFT down, so CC91 127 on many voices stays inside the
    // int16 delay lines, and the wet signal comes back up by the same at
    // the mix. Which post-processing runs depends on Reverb Type (see
    // g_reverbType's own comment for why all three share one tank). Lush
    // and Shimmer run on the mid of the stereo tank and add its side back.
    // Skipped entirely (tank included) when off, see Synth::setReverbEnabled().
#ifdef MP3_PLAYER
    // In MP3 block output it also sleeps once the tank has gone silent, see
    // g_reverbAsleep, and wakes on the next send.
    bool reverbOn = g_reverbEnabled;
    if (g_mp3Block) {
        if (sendSum != 0) {
            g_reverbAsleep = false;
            g_reverbQuiet = 0;
        } else if (g_reverbAsleep) {
            reverbOn = false;
        }
    }
    if (reverbOn) {
#else
    if (g_reverbEnabled) {
#endif
        int32_t tankIn = sendSum >> REVERB_SEND_SHIFT;
        int32_t tankL, tankR, wetL, wetR;

        if (g_reverbType == REVERB_TYPE_SHIMMER) {
            // Regenerative: last sample's (already-decaying, see
            // SHIMMER_FEEDBACK_Q16) pitch-shifted output feeds back into
            // this sample's tank input, on top of the dry send.
            reverbProcess(tankIn + g_shimmerFeedbackHeld, tankL, tankR);
            int32_t shimmerOut = shimmerProcess((tankL + tankR) >> 1);
            // The loop's gain at DC is over 1 (combs 5.5x, feedback 0.4, and
            // an octave up of DC is still DC), so the feedback is DC-blocked
            // (one-pole, ~27 Hz, state in Q8).
            int32_t fb = (shimmerOut * SHIMMER_FEEDBACK_Q16) >> 16;
            g_shimmerDcQ8 += fb - (g_shimmerDcQ8 >> 8);
            g_shimmerFeedbackHeld = fb - (g_shimmerDcQ8 >> 8);
            // Tank halved so the pitched wash (the actual point of this
            // mode) leads rather than just riding underneath a full-level
            // plain reverb.
            wetL = (tankL >> 1) + shimmerOut;
            wetR = (tankR >> 1) + shimmerOut;
        } else {
            reverbProcess(tankIn, tankL, tankR);
            if (g_reverbType == REVERB_TYPE_LUSH) {
                int32_t mid = chorusProcess((tankL + tankR) >> 1);
                int32_t side = (tankL - tankR) >> 1;
                wetL = mid + side;
                wetR = mid - side;
            } else { // REVERB_TYPE_LOFI
                if (g_reverbDecimateCounter == 0) {
                    g_reverbHeldL = tankL & REVERB_BIT_MASK;
                    g_reverbHeldR = tankR & REVERB_BIT_MASK;
                }
                g_reverbDecimateCounter++;
                if (g_reverbDecimateCounter >= REVERB_DECIMATE) g_reverbDecimateCounter = 0;
                wetL = g_reverbHeldL;
                wetR = g_reverbHeldR;
            }
        }

        // wet is within a few x +-32767 and g_reverbMixQ16 at most 14417, so
        // the product fits int32; the shift puts back REVERB_SEND_SHIFT.
        mixL += (wetL * (int32_t)g_reverbMixQ16) >> (16 - REVERB_SEND_SHIFT);
        mixR += (wetR * (int32_t)g_reverbMixQ16) >> (16 - REVERB_SEND_SHIFT);
#ifdef MP3_PLAYER
        if (g_mp3Block) {
            const ReverbState& rs = g_reverbState;
            bool quiet = sendSum == 0 && (tankL | tankR | wetL | wetR | g_shimmerFeedbackHeld) == 0 &&
                         (rs.damp[0] | rs.damp[1] | rs.damp[2] | rs.damp[3]) == 0;
            g_reverbQuiet = quiet ? g_reverbQuiet + 1 : 0;
            if (g_reverbQuiet >= REVERB_SLEEP_SAMPLES) g_reverbAsleep = true;
        }
#endif
    }

    // Synth: scaled straight to the output (volume, output level and
    // makeup in one gain, split in bytes so a mix up to ~8M stays int32),
    // then limited against the output's own ceiling, see updateSynthOutput().
    int32_t gHi = g_synthGainHi, gLo = g_synthGainLo;
    int32_t synL = ((mixL * gHi) >> 8) + ((mixL * gLo) >> 16);
    int32_t synR = ((mixR * gHi) >> 8) + ((mixR * gLo) >> 16);
    if (g_synthCapped) {
        Leveler& sl = g_synthLeveler;
        int32_t magL = synL < 0 ? -synL : synL;
        int32_t magR = synR < 0 ? -synR : synR;
        synthLevelerStep(sl, magL > magR ? magL : magR);
        if (sl.gainQ16 != 65536) {
            synL = levelerApply(sl, synL);
            synR = levelerApply(sl, synR);
#ifdef HOST_MIDI
            g_hostLevelSamples++;
#endif
        }
    }
    synL = synthLimit(synL);
    synR = synthLimit(synR);

    if (!wavLive) {
        // The WAV leveler goes on releasing towards silence.
        if (g_levelerEnvelope != 0 || g_levelerGainQ16 != 65536) wavLevelerIdle();
        outLeft = (int16_t)synL;
        outRight = (int16_t)synR;
        return;
    }

    // WAV and tracker: attenuate-only leveler, soft limit, then volume and
    // output level, as before the synth had its own path.
    int32_t magL = wavL < 0 ? -wavL : wavL;
    int32_t magR = wavR < 0 ? -wavR : wavR;
    int32_t magnitude = magL > magR ? magL : magR;
    if (magnitude > g_levelerEnvelope) {
        g_levelerEnvelope += (magnitude - g_levelerEnvelope) >> 13; // ~186ms attack
    } else {
        g_levelerEnvelope += (magnitude - g_levelerEnvelope) >> 17; // ~3s release
    }
    if (++g_levelerUpdateCounter >= LEVELER_UPDATE_PERIOD) {
        g_levelerUpdateCounter = 0;
        if (g_levelerEnvelope > LEVELER_TARGET) {
            g_levelerGainQ16 = ((uint32_t)LEVELER_TARGET << 16) / (uint32_t)g_levelerEnvelope; // 26000 << 16 fits uint32
        } else {
            g_levelerGainQ16 = 65536; // unity -- never boosts past this
        }
    }
    // Unity (the usual case) skips this. The gain (< 65536) is split into
    // its high and low bytes so both multiplies stay in int32.
    if (g_levelerGainQ16 != 65536) {
        int32_t lHi = (int32_t)(g_levelerGainQ16 >> 8), lLo = (int32_t)(g_levelerGainQ16 & 0xFF);
        wavL = ((wavL * lHi) >> 8) + ((wavL * lLo) >> 16);
        wavR = ((wavR * lHi) >> 8) + ((wavR * lLo) >> 16);
    }
#ifdef HOST_MIDI
    if (g_levelerGainQ16 != 65536) g_hostLevelSamples++;
    if (wavL > SOFT_KNEE_START || -wavL > SOFT_KNEE_START || wavR > SOFT_KNEE_START || -wavR > SOFT_KNEE_START) g_hostLimitSamples++;
#endif
    // softLimit() bounds to +-32000, times g_masterGainQ16 <= 65536 fits int32.
    int32_t outL = (((softLimit(wavL) * (int32_t)g_masterGainQ16) >> 16) >> g_outputLevelShift) + synL;
    int32_t outR = (((softLimit(wavR) * (int32_t)g_masterGainQ16) >> 16) >> g_outputLevelShift) + synR;
    if (outL > 32767) outL = 32767;
    if (outL < -32768) outL = -32768;
    if (outR > 32767) outR = 32767;
    if (outR < -32768) outR = -32768;
    outLeft = (int16_t)outL;
    outRight = (int16_t)outR;
}

} // namespace

void Synth::begin() {
    // Seed the live-editable preset tables here on core 0, before core 1
    // starts, so a bank loaded right after begin() can't be overwritten by
    // the defaults.
    for (int i = 0; i < 16; i++) PRESETS[i] = DEFAULT_PRESETS[i];
    for (int i = 0; i < DRUM_TYPE_COUNT; i++) DRUM_PRESETS[i] = DEFAULT_DRUM_PRESETS[i];
    __dmb(); // tables written before core 1 sees the ready flag
    g_readyForPioInit = true;
}

void Synth::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0) { noteOff(channel, note); return; }
    // pushMsg() waits rather than drops when the ring is full, so a note is
    // never lost.
    pushMsg(packNoteMsg(channel, note, velocity));
}

void Synth::noteOff(uint8_t channel, uint8_t note) {
    pushMsg(packNoteMsg(channel, note, 0));
}

void Synth::programChange(uint8_t channel, uint8_t program) {
    // Written here immediately, not just via the ring message below --
    // Synth::getChannelProgram() reads g_channelProgram[] directly, and a
    // caller that reads it back on the very same tick (e.g. pariSynth's
    // manual EDIT+LEFT/RIGHT handler, which calls
    // Ui::updatePariSynthChannelCell() right after this) would otherwise
    // often see the stale value -- core1 only applies the ring message
    // whenever it next drains its queue, not synchronously. Plain uint8_t
    // writes are already treated as safely atomic across cores in this
    // codebase (see synth.h's comment on the live-editable preset arrays);
    // core1's own write of the identical value when it later drains the
    // ring is redundant but harmless.
    g_channelProgram[channel] = program;
    pushMsg(packProgramMsg(channel, program));
}

void Synth::controlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    pushMsg(packCcMsg(channel, controller, value));
}

void Synth::pitchBend(uint8_t channel, int16_t bend) {
    int32_t b = (int32_t)bend + 8192;
    if (b < 0) b = 0;
    if (b > 16383) b = 16383;
    pushMsg(packBendMsg(channel, (uint16_t)b));
}

void Synth::channelPressure(uint8_t channel, uint8_t pressure) {
    pushMsg(packPressureMsg(channel, pressure));
}

void Synth::resetPrograms() {
    pushMsg(RESET_PROGRAMS_MSG);
}

void Synth::resetControllers() {
    pushMsg(RESET_CONTROLLERS_MSG);
}

void Synth::allNotesOff() {
    pushMsg(PANIC_MSG);
}

void Synth::setVolume(uint8_t percent) {
    if (percent > 100) percent = 100;
    pushMsg(packVolumeMsg(percent));
}

void Synth::setOutputLevel(uint8_t level) {
    if (level >= OUTPUT_LEVEL_COUNT) level = OUTPUT_LEVEL_HP_LOW;
    pushMsg(packOutputLevelMsg(level));
}

void Synth::setReverbEnabled(bool enabled) {
    pushMsg(packReverbEnabledMsg(enabled));
}

void Synth::setReverbMix(uint8_t percent) {
    if (percent > 100) percent = 100;
    pushMsg(packReverbMixMsg(percent));
}

void Synth::setReverbType(uint8_t type) {
    if (type > 2) type = 0;
    pushMsg(packReverbTypeMsg(type));
}

void Synth::setLfoRateTenthsHz(uint16_t tenthsHz) {
    pushMsg(packLfoRateMsg(tenthsHz));
}

void Synth::setLfoVoices(uint8_t maxVoices) {
    if (maxVoices > NUM_MELODIC_VOICES) maxVoices = NUM_MELODIC_VOICES;
    pushMsg(packLfoVoicesMsg(maxVoices));
}

void Synth::setStereoSpread(uint8_t percent) {
    if (percent > 100) percent = 100;
    pushMsg(packStereoSpreadMsg(percent));
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
    pushMsg(packPresetDirtyMsg(family));
}

void Synth::resetInstrumentPresetToDefault(uint8_t family) {
    if (family >= 16) return;
    PRESETS[family] = DEFAULT_PRESETS[family];
    pushMsg(packPresetDirtyMsg(family));
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
    pushMsg(packDrumPresetDirtyMsg(drumType));
}

void Synth::resetDrumPresetToDefault(uint8_t drumType) {
    if (drumType >= DRUM_TYPE_COUNT) return;
    DRUM_PRESETS[drumType] = DEFAULT_DRUM_PRESETS[drumType];
    pushMsg(packDrumPresetDirtyMsg(drumType));
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
    g_wavActive = false; // consumer stops mixing before the reset lands
    g_wavEnded = false;
    g_wavUnderrun = false;
    // Core 1 owns tail, so post where to cut back to and let it apply that
    // at its next sample (see wavStreamApplyReset()). Until then
    // wavStreamProducerTail() treats the tail as already cut back, so a
    // prefill right after this sees the full free space.
    g_wavResetTo = g_wavHead;
    __dmb(); // release: g_wavResetTo written before the seq bump
    g_wavResetSeq = g_wavResetSeq + 1;
}

void Synth::wavStreamSetActive(bool active) {
    __dmb(); // any reset or frames written before this land first
    g_wavActive = active;
}

// Core 0's view of the tail. While a reset is pending, frames before
// g_wavResetTo are already discarded, so their slots count as free. Core 1
// isn't popping then (g_wavActive went false before the seq bump), and it
// cuts its tail to g_wavResetTo before its next pop.
static size_t wavStreamProducerTail() {
    if (g_wavResetSeen != g_wavResetSeq) return g_wavResetTo;
    __dmb(); // acquire: ack read before the tail core 1 wrote ahead of it
    return g_wavTail;
}

size_t Synth::wavStreamWrite(const int16_t* frames, size_t count) {
    size_t head = g_wavHead;
    size_t free = WAV_RING_FRAMES - (head - wavStreamProducerTail());
    size_t n = count < free ? count : free;
    if (n == 0) return 0;
    __dmb(); // acquire: tail read before writing into the slots it freed
    for (size_t i = 0; i < n; i++) {
        size_t idx = (head + i) & WAV_RING_MASK;
        g_wavRing[idx * 2 + 0] = frames[i * 2 + 0];
        g_wavRing[idx * 2 + 1] = frames[i * 2 + 1];
    }
    __dmb(); // release: frame data written before head covers it
    g_wavHead = head + n;
    return n;
}

size_t Synth::wavStreamFree() {
    return WAV_RING_FRAMES - (g_wavHead - wavStreamProducerTail());
}

size_t Synth::wavStreamCapacity() { return WAV_RING_FRAMES; }

uint32_t Synth::wavStreamWritten() { return (uint32_t)g_wavHead; }

uint32_t Synth::wavStreamPlayed() { return (uint32_t)wavStreamProducerTail(); }

// Core 0 is the only writer, so the slots between tail and head stay put
// while they're copied, even if core 1 moves tail on meanwhile.
size_t Synth::wavStreamPeek(int16_t* out, size_t frames) {
    if (g_wavResetSeen != g_wavResetSeq) return 0;
    size_t tail = g_wavTail;
    __dmb(); // acquire: tail read before the data behind it
    size_t head = g_wavHead;
    size_t have = head - tail;
    size_t n = frames < have ? frames : have;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (tail + i) & WAV_RING_MASK;
        out[i * 2 + 0] = g_wavRing[idx * 2 + 0];
        out[i * 2 + 1] = g_wavRing[idx * 2 + 1];
    }
    return n;
}

bool Synth::wavStreamIsActive() { return g_wavActive; }

bool Synth::wavStreamIsEnded() { return g_wavEnded; }

void Synth::wavStreamEnd() {
    __dmb(); // release: the last head published before the end flag
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
void setupGovernor(); // see the render governor above loop1()

void setup1() {
#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
    // Paint SCRATCH_X below this stack for the STACK line in tracker_profile.cpp.
    {
        uint32_t sp;
        asm volatile("mov %0, sp" : "=r"(sp));
        for (uint32_t a = SRAM4_BASE; a < sp - 64; a += 4) *(volatile uint32_t*)a = Synth::PROFILE_STACK_PAINT;
    }
#endif
    while (!g_readyForPioInit) {
        // Busy-wait for core 0 to finish claiming PIO resources for the SD
        // card/display before this core claims one for I2S.
    }

    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        g_sineTable[i] = (int16_t)(32000.0f * sinf(2.0f * (float)M_PI * i / SINE_TABLE_SIZE));
    }
    g_sineTable[SINE_TABLE_SIZE] = g_sineTable[0];
    setupGovernor();

    // PRESETS/DRUM_PRESETS were seeded by Synth::begin() before the flag
    // above. Core 0 may already be editing them; its dirty doorbells
    // recompute any record these loops read mid-edit.
    for (int i = 0; i < 16; i++) recomputeInstrumentPresetCache(i);
    for (int i = 0; i < DRUM_TYPE_COUNT; i++) recomputeDrumPresetCache(i);
    g_lfoPhaseInc = hzToPhaseInc(LFO_RATE_HZ);
    g_reverbChorusLfoPhaseInc = hzToPhaseInc(REVERB_CHORUS_LFO_RATE_HZ);
    g_masterGainQ16 = volumePercentToGainQ16(80); // matches the previous default percent
    updateSynthOutput();
    g_reverbMixQ16 = reverbMixPercentToQ16(70); // matches the level this effect first shipped at
    for (int i = 0; i < 16; i++) resetChannel((uint8_t)i, true);

    // 8 x 256 words, ~46 ms. The per-sample render path (loop1(),
    // renderSample() and what they call) runs from RAM. What's left in flash
    // is the I2S library's write16() per sample, the leveler's divide every
    // 256 samples and message handling (note-on etc.); the margin covers
    // those stalling on core 0's flash and SD traffic.
    g_i2s.setBuffers(8, 256);
    g_i2s.setBitsPerSample(16);
    g_i2s.begin(SAMPLE_RATE);
}

namespace {

// Core 1: applies one ring message. Stays in flash, it only runs when a
// message arrives.
__attribute__((noinline)) void handleMessage(uint32_t msg) {
    if (msg == PANIC_MSG) {
        handlePanic();
        return;
    }
    if (msg == RESET_PROGRAMS_MSG) {
        for (int i = 0; i < 16; i++) {
            g_channelProgram[i] = 0;
            resetChannel((uint8_t)i, true);
        }
        return;
    }
    if (msg == RESET_CONTROLLERS_MSG) {
        for (int i = 0; i < 16; i++) resetChannel((uint8_t)i, true);
        return;
    }
    uint32_t type = msg >> 28;
    if (type == MSG_TYPE_VOLUME) {
        g_masterGainQ16 = volumePercentToGainQ16((uint8_t)(msg & 0xFF));
        updateSynthOutput();
        return;
    }
    if (type == MSG_TYPE_OUTPUT_LEVEL) {
        uint8_t level = (uint8_t)(msg & 0xFF);
        if (level < OUTPUT_LEVEL_COUNT) g_outputLevelShift = OUTPUT_LEVEL_SHIFT[level];
        updateSynthOutput();
        return;
    }
    if (type == MSG_TYPE_REVERB_ENABLED) {
        g_reverbEnabled = (msg & 0xFF) != 0;
        return;
    }
    if (type == MSG_TYPE_REVERB_MIX) {
        g_reverbMixQ16 = reverbMixPercentToQ16((uint8_t)(msg & 0xFF));
        return;
    }
    if (type == MSG_TYPE_REVERB_TYPE) {
        uint8_t t = (uint8_t)(msg & 0xFF);
        if (t <= REVERB_TYPE_SHIMMER && t != g_reverbType) reverbTypeChanged(t);
        return;
    }
    if (type == MSG_TYPE_PRESET_DIRTY) {
        uint8_t family = (uint8_t)(msg & 0xFF);
        if (family < 16) recomputeInstrumentPresetCache(family);
        return;
    }
    if (type == MSG_TYPE_DRUM_PRESET_DIRTY) {
        uint8_t drumType = (uint8_t)(msg & 0xFF);
        if (drumType < DRUM_TYPE_COUNT) recomputeDrumPresetCache(drumType);
        return;
    }
    if (type == MSG_TYPE_LFO_RATE) {
        uint16_t tenthsHz = (uint16_t)(msg & 0xFFFF);
        if (tenthsHz == 0) {
            g_lfoEnabled = false;
        } else {
            g_lfoEnabled = true;
            g_lfoPhaseInc = hzToPhaseInc((float)tenthsHz / 10.0f);
        }
        return;
    }
    if (type == MSG_TYPE_LFO_VOICES) {
        g_maxModulatedVoices = (int)(uint8_t)(msg & 0xFF);
        return;
    }
    if (type == MSG_TYPE_STEREO_SPREAD) {
        g_stereoSpreadPercent = (int)(uint8_t)(msg & 0xFF);
        return;
    }
    uint8_t channel = (msg >> 16) & 0x0F;
    if (type == MSG_TYPE_PROGRAM) {
        g_channelProgram[channel] = (uint8_t)(msg & 0xFF);
        return;
    }
    if (type == MSG_TYPE_CC) {
        handleControlChange(channel, (uint8_t)((msg >> 8) & 0x7F), (uint8_t)(msg & 0x7F));
        return;
    }
    if (type == MSG_TYPE_BEND) {
        handlePitchBend(channel, (uint16_t)(msg & 0x3FFF));
        return;
    }
    if (type == MSG_TYPE_PRESSURE) {
        handleChannelPressure(channel, (uint8_t)(msg & 0x7F));
        return;
    }
    uint8_t note = msg & 0xFF;
    uint8_t velocity = (msg >> 8) & 0xFF;
    if (velocity > 0) handleNoteOn(channel, note, velocity);
    else handleNoteOff(channel, note);
}

// Core 1: applies up to maxMessages waiting messages.
void drainMessages(int maxMessages) {
    uint32_t msg;
    while (maxMessages-- > 0 && popMsg(msg)) {
#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
        g_trace.where1 = Synth::PW_MESSAGE;
        g_trace.lastMsg1 = msg;
        g_trace.msgs1++;
#endif
        handleMessage(msg);
    }
}

} // namespace

// -- Offline rendering (core 0, see Synth::offlineBegin()) ---------------------
namespace {

// Silence and a fresh start, as setup1() leaves things: no voices, empty
// reverb, LFO and noise from the top, levelers at rest.
void clearRenderState() {
    for (int i = 0; i < NUM_VOICES; i++) g_voices[i] = Voice();
    g_voiceStartCounter = 0;
    g_lfoPhase = 0;
    g_noiseState = 0xACE1u;
    memset(g_reverbCombBuf0, 0, sizeof(g_reverbCombBuf0));
    memset(g_reverbCombBuf1, 0, sizeof(g_reverbCombBuf1));
    memset(g_reverbCombBuf2, 0, sizeof(g_reverbCombBuf2));
    memset(g_reverbCombBuf3, 0, sizeof(g_reverbCombBuf3));
    memset(g_reverbAllpassBuf0, 0, sizeof(g_reverbAllpassBuf0));
    memset(g_reverbAllpassBuf1, 0, sizeof(g_reverbAllpassBuf1));
    g_reverbState = {};
    g_reverbHeldL = g_reverbHeldR = 0;
    g_reverbDecimateCounter = 0;
    g_reverbChorusPos = 0;
    g_reverbChorusLfoPhase = 0;
    reverbTypeChanged(g_reverbType); // clears the Lush or Shimmer delay line
#ifdef MP3_PLAYER
    g_mp3Block = false;
    g_mp3NoVoices = false;
    g_reverbAsleep = false;
    g_reverbQuiet = 0;
#endif
    g_levelerEnvelope = 0;
    g_levelerGainQ16 = 65536;
    g_levelerUpdateCounter = 0;
    g_synthLeveler = {0, 65536, 0};
}

} // namespace

void Synth::offlineBegin() {
    if (g_offlineOwned) return;
    g_offlineRequest = true;
#ifndef HOST_MIDI
    while (!g_offlineParked) {
        // Core 1 parks at its next sample.
    }
#endif
    __dmb(); // core 1's last writes seen before this core takes over
    g_offlineOwned = true;
    drainMessages(MSG_RING_SIZE);
    clearRenderState();
    updateSynthOutput();
}

void Synth::offlineRender(int16_t* out, size_t frames) {
    if (!g_offlineOwned) return;
    // Per frame as loop1() does it, without the governor: there's no deadline.
    for (size_t i = 0; i < frames; i++) {
        drainMessages(32);
        renderSample(out[i * 2], out[i * 2 + 1]);
    }
}

bool Synth::offlineVoicesActive() {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (g_voices[i].active) return true;
    }
    return false;
}

void Synth::offlineEnd() {
    if (!g_offlineOwned) return;
    drainMessages(MSG_RING_SIZE);
    clearRenderState();
    for (int i = 0; i < 16; i++) {
        g_channelProgram[i] = 0;
        resetChannel((uint8_t)i, true);
    }
    g_offlineOwned = false;
    updateSynthOutput();
    __dmb(); // all of the above lands before core 1 takes over again
    g_offlineRequest = false;
#ifndef HOST_MIDI
    while (g_offlineParked) {
        // Core 1 is back, so a new offlineBegin() can't see a stale park.
    }
#endif
}

// -- Render governor ----------------------------------------------------------
// Core 1 times each sample's render with SysTick (the
// Arduino core runs it on core 1 at the CPU clock, 24 bits, counting down).
// Over a block of GOVERNOR_BLOCK samples, if that passes 90% of the block's
// time, one voice is faded out fast: the quietest one in release, or if
// none is, the quietest one. The block after a shed is skipped so the fade
// has time to land before the next decision.
const int GOVERNOR_BLOCK = 32;
uint32_t g_governorLimit = 0xFFFFFFFF; // cycles per block, set in setup1()
uint32_t g_governorUsed = 0;
uint32_t g_governorCount = 0;
bool g_governorHold = false;
uint32_t g_governorSheds = 0;     // voices shed since boot, for debugging
uint32_t g_governorPeakUsed = 0;  // busiest block since boot, in cycles

#ifdef TRACKER_PROFILE
// Profile build only: render cycles per sample on core 1, read by core 0.
uint32_t g_profPeakSample = 0, g_profPeakBlock = 0, g_profPeakVoices = 0;
uint64_t g_profSumCycles = 0;
uint32_t g_profSamples = 0, g_profSheds0 = 0;
// Cycles core 1 spent inside g_i2s.write16(), which blocks while the I2S
// buffers are full; it wraps, read with Synth::profileCore1IdleCycles().
volatile uint32_t g_profIdleCycles = 0;
#endif

void setupGovernor() {
#ifndef HOST_MIDI
    uint32_t perSample = clock_get_hz(clk_sys) / SAMPLE_RATE;
#else
    uint32_t perSample = 200000000u / SAMPLE_RATE;
#endif
    g_governorLimit = perSample * GOVERNOR_BLOCK / 10 * 9;
}

void __not_in_flash_func(governorShed)() {
    Voice* best = nullptr;
    bool bestReleasing = false;
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice& v = g_voices[i];
        // Skip voices already fading fast and ones queued for a steal.
        if (!v.active || v.pendVel || (v.stage == ENV_RELEASE && v.releaseK >= STEAL_FADE_K)) continue;
        bool releasing = v.stage == ENV_RELEASE;
        if (!best || (releasing && !bestReleasing) ||
            (releasing == bestReleasing && v.amplitude < best->amplitude)) {
            best = &v;
            bestReleasing = releasing;
        }
    }
    if (!best) return;
    best->held = false;
    best->sostLatched = false;
    best->stage = ENV_RELEASE;
    best->releaseK = STEAL_FADE_K;
    best->envFloor = envStageFloor(best->amplitude);
    g_governorSheds++;
#ifdef HOST_MIDI
    g_hostGovernorSheds++;
#endif
}

ALWAYS_INLINE void __not_in_flash_func(governorTick)(uint32_t used) {
    g_governorUsed += used;
    if (++g_governorCount < (uint32_t)GOVERNOR_BLOCK) return;
    if (g_governorUsed > g_governorPeakUsed) g_governorPeakUsed = g_governorUsed;
#ifdef TRACKER_PROFILE
    if (g_governorUsed > g_profPeakBlock) g_profPeakBlock = g_governorUsed;
    {
        uint32_t n = 0;
        for (int i = 0; i < NUM_VOICES; i++) n += g_voices[i].active;
        if (n > g_profPeakVoices) g_profPeakVoices = n;
    }
#endif
    if (g_governorUsed > g_governorLimit && !g_governorHold) {
        PROF_WHERE1(Synth::PW_SHED);
        governorShed();
        g_governorHold = true;
    } else {
        g_governorHold = false;
    }
    g_governorCount = 0;
    g_governorUsed = 0;
}

#if defined(MP3_PLAYER) && !defined(HOST_MIDI)
// Core 1 while an MP3 is loaded: renders MP3_BLOCK samples, writes them to
// I2S in one go, then moves decoded audio into the WAV ring and decodes the
// next frame when it's due. Messages are drained once per block, so notes
// start on a block boundary (0.7 ms).
const int MP3_BLOCK = 32;
uint32_t g_mp3Out[MP3_BLOCK]; // core 1 only, L in the top half as write16() packs it

#ifdef TRACKER_PROFILE
// The per-sample profile counters and voice snapshot, as loop1() keeps them.
ALWAYS_INLINE void profileSample(uint32_t cyc) {
    g_trace.count1++;
    if (cyc > g_profPeakSample) g_profPeakSample = cyc;
    g_profSumCycles += cyc;
    g_profSamples++;
    if ((g_trace.count1 & 1023) == 0) {
        for (int i = 0; i < NUM_VOICES && i < 32; i++) {
            const Voice& v = g_voices[i];
            g_trace.vstate[i] = (uint8_t)(v.active | ((v.stage & 3) << 1) | ((v.pendVel != 0) << 3) | (v.held << 4));
            g_trace.vamp[i] = (int16_t)(v.amplitude >> 16);
        }
        g_trace.snapAt = g_trace.count1;
        g_trace.msgHead = g_msgHead;
        g_trace.msgTail = g_msgTail;
    }
}
#endif

void __not_in_flash_func(loop1Mp3)() {
    if (!g_mp3Block) {
        g_mp3Block = true;
        g_reverbAsleep = false;
        g_reverbQuiet = 0;
    }
    PROF_WHERE1(Synth::PW_DRAIN);
    drainMessages(64);
    bool anyVoice = false;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (g_voices[i].active) {
            anyVoice = true;
            break;
        }
    }
    g_mp3NoVoices = !anyVoice;
    for (int n = 0; n < MP3_BLOCK; n++) {
        PROF_WHERE1(Synth::PW_RENDER);
        uint32_t start = systick_hw->cvr;
        int16_t left, right;
        renderSample(left, right);
        uint32_t cyc = (start - systick_hw->cvr) & 0x00FFFFFF;
#ifdef TRACKER_PROFILE
        PROF_WHERE1(Synth::PW_GOVERNOR);
        profileSample(cyc);
#endif
        governorTick(cyc);
        g_mp3Out[n] = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
    }
    PROF_WHERE1(Synth::PW_I2S);
#ifdef TRACKER_PROFILE
    uint32_t idleStart = systick_hw->cvr;
#endif
    // write() doesn't wait for room, so this loop does, which paces core 1.
    const uint8_t* p = (const uint8_t*)g_mp3Out;
    size_t left = sizeof(g_mp3Out);
    while (left) {
        size_t n = g_i2s.write(p, left);
        p += n;
        left -= n;
    }
#ifdef TRACKER_PROFILE
    g_profIdleCycles = g_profIdleCycles + ((idleStart - systick_hw->cvr) & 0x00FFFFFF);
#endif
    // A decode only starts with at most one 256-frame I2S buffer free, so
    // 7 of the 8 (40 ms) are queued; a frame takes about 10 ms.
    PROF_WHERE1(Synth::PW_MP3_PUSH);
    if (Mp3Player::core1Push() && g_i2s.availableForWrite() <= 256 * 4) {
        PROF_WHERE1(Synth::PW_MP3_DECODE);
        Mp3Player::core1Decode();
    }
}
#endif

void __not_in_flash_func(loop1)() {
#ifndef HOST_MIDI
    // Core 0 renders offline: silence, and the synth's state is left alone.
    if (g_offlineRequest) {
        g_offlineParked = true;
        g_i2s.write16(0, 0);
        return;
    }
    if (g_offlineParked) g_offlineParked = false;
#endif
#if defined(MP3_PLAYER) && !defined(HOST_MIDI)
    if (Mp3Player::core1Active()) {
        loop1Mp3();
        return;
    }
    g_mp3Block = false;
    g_mp3NoVoices = false;
#endif
    // A cap per sample keeps a long burst from holding up one sample; the
    // rest wait for the next sample.
#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
    g_trace.count1++;
    g_trace.where1 = Synth::PW_DRAIN;
#endif
    drainMessages(32);
#ifndef HOST_MIDI
    PROF_WHERE1(Synth::PW_RENDER);
    uint32_t start = systick_hw->cvr; // render time only, see governorTick()
#endif

    int16_t left, right;
    renderSample(left, right);
#ifndef HOST_MIDI
    uint32_t cyc = (start - systick_hw->cvr) & 0x00FFFFFF;
#ifdef TRACKER_PROFILE
    if (cyc > g_profPeakSample) g_profPeakSample = cyc;
    g_profSumCycles += cyc;
    g_profSamples++;
    g_trace.where1 = Synth::PW_GOVERNOR;
    // Voice snapshot every 1024 samples, for the trace after a hang.
    if ((g_trace.count1 & 1023) == 0) {
        for (int i = 0; i < NUM_VOICES && i < 32; i++) {
            const Voice& v = g_voices[i];
            g_trace.vstate[i] = (uint8_t)(v.active | ((v.stage & 3) << 1) | ((v.pendVel != 0) << 3) | (v.held << 4));
            g_trace.vamp[i] = (int16_t)(v.amplitude >> 16);
        }
        g_trace.snapAt = g_trace.count1;
        g_trace.msgHead = g_msgHead;
        g_trace.msgTail = g_msgTail;
    }
#endif
    governorTick(cyc);
#else
    if (g_hostGovernorModel) {
        // About what the disassembly gives: 125 cycles a voice, 150 with
        // vibrato or another modulation on, the fixed costs with reverb,
        // and the synth leveler while it runs.
        uint32_t used = g_synthCapped ? 680 : 650;
        for (int i = 0; i < NUM_VOICES; i++) {
            const Voice& v = g_voices[i];
            used += !v.active ? 9 : v.modFlags ? 150 : 125;
        }
        governorTick(used);
    }
#endif
    PROF_WHERE1(Synth::PW_I2S);
#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
    uint32_t idleStart = systick_hw->cvr;
#endif
    g_i2s.write16(left, right); // blocks until DMA buffer space is free, paces this loop
#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
    g_profIdleCycles = g_profIdleCycles + ((idleStart - systick_hw->cvr) & 0x00FFFFFF);
#endif
}

#ifdef TRACKER_PROFILE
void Synth::profileReset() {
    g_profPeakSample = g_profPeakBlock = g_profPeakVoices = 0;
    g_profSumCycles = 0;
    g_profSamples = 0;
    g_profSheds0 = g_governorSheds;
}
void Synth::profileGet(SynthProfile& p) {
    p.peakSampleCycles = g_profPeakSample;
    p.avgSampleCycles = g_profSamples ? (uint32_t)(g_profSumCycles / g_profSamples) : 0;
    p.peakBlockCycles = g_profPeakBlock;
    p.peakVoices = g_profPeakVoices;
    p.sheds = g_governorSheds - g_profSheds0;
    p.shedsTotal = g_governorSheds;
    p.peakUsedTotal = g_governorPeakUsed;
}
#ifndef HOST_MIDI
uint32_t Synth::profileCore1IdleCycles() { return g_profIdleCycles; }
#endif
#endif

#if defined(TRACKER_PROFILE) && !defined(HOST_MIDI)
Synth::ProfileTrace& Synth::profileTrace() { return g_trace; }

static const char* whereName(uint32_t w) {
    if (w & Synth::PW_VOICE && w < 0x1000) return "renderVoice";
    switch (w) {
        case Synth::PW_DRAIN: return "loop1 drain";
        case Synth::PW_MESSAGE: return "handleMessage";
        case Synth::PW_NOTE_ON: return "handleNoteOn";
        case Synth::PW_STEAL: return "startStealFade";
        case Synth::PW_START_PENDING: return "startPending";
        case Synth::PW_RENDER: return "renderSample";
        case Synth::PW_MIX: return "renderSample mix/reverb";
        case Synth::PW_GOVERNOR: return "governorTick";
        case Synth::PW_SHED: return "governorShed";
        case Synth::PW_I2S: return "i2s write";
        case Synth::PW_MP3_PUSH: return "Mp3Player core1Push";
        case Synth::PW_MP3_DECODE: return "Mp3Player decode";
        case Synth::PW0_IDLE: return "idle";
        case Synth::PW0_FIND: return "findFile";
        case Synth::PW0_LOAD: return "MidiPlayer load";
        case Synth::PW0_PLAY: return "MidiPlayer play";
        case Synth::PW0_UPDATE: return "MidiPlayer update";
        case Synth::PW0_PRINT: return "profile print";
        case Synth::PW0_STOP: return "MidiPlayer stop";
        case Synth::PW0_PUSH_WAIT: return "pushMsg wait";
        default: return "?";
    }
}

void Synth::profilePrintTrace(const Synth::ProfileTrace& t, const char* why) {
    Serial.printf("TRACE %s\n", why);
    Serial.printf("TRACE core1 where 0x%lx (%s) voice %ld, loops %lu, msgs %lu, last msg 0x%08lx\n",
                  (unsigned long)t.where1, whereName(t.where1),
                  (t.where1 & PW_VOICE) && t.where1 < 0x1000 ? (long)(t.where1 & 0xFF) : -1L,
                  (unsigned long)t.count1, (unsigned long)t.msgs1, (unsigned long)t.lastMsg1);
    Serial.printf("TRACE core0 where 0x%lx (%s), pushes %lu, last msg 0x%08lx, push wait %lu us, at %lu ms\n",
                  (unsigned long)t.where0, whereName(t.where0), (unsigned long)t.pushes0,
                  (unsigned long)t.lastMsg0, (unsigned long)t.pushWaitUs, (unsigned long)t.ms0);
    Serial.printf("TRACE ring head %lu tail %lu at loop %lu\n", (unsigned long)t.msgHead,
                  (unsigned long)t.msgTail, (unsigned long)t.snapAt);
    Serial.print("TRACE voices (active,stage,queued,held:amp)");
    for (int i = 0; i < NUM_VOICES && i < 32; i++) {
        uint8_t s = t.vstate[i];
        Serial.printf(" %d:%d%d%d%d:%d", i, s & 1, (s >> 1) & 3, (s >> 3) & 1, (s >> 4) & 1, (int)t.vamp[i]);
    }
    Serial.println();
    if (t.faultMagic == PROFILE_TRACE_MAGIC) {
        Serial.printf("TRACE hard fault on core %lu: pc 0x%08lx lr 0x%08lx sp 0x%08lx\n", (unsigned long)t.faultCore,
                      (unsigned long)t.faultPc, (unsigned long)t.faultLr, (unsigned long)t.faultSp);
    }
    Serial.flush();
}

void Synth::profileDump(const char* why) {
    for (int i = 0; i < NUM_VOICES && i < 32; i++) {
        const Voice& v = g_voices[i];
        g_trace.vstate[i] = (uint8_t)(v.active | ((v.stage & 3) << 1) | ((v.pendVel != 0) << 3) | (v.held << 4));
        g_trace.vamp[i] = (int16_t)(v.amplitude >> 16);
    }
    g_trace.snapAt = g_trace.count1;
    g_trace.msgHead = g_msgHead;
    g_trace.msgTail = g_msgTail;
    profilePrintTrace(g_trace, why);
}

void Synth::profileHeartbeat() {
    int n = 0;
    for (int i = 0; i < NUM_VOICES; i++) n += g_voices[i].active;
    Serial.printf("BEAT %lu ms: core1 loops %lu where 0x%lx (%s), core0 where 0x%lx (%s), ring %lu, voices %d, sheds %lu\n",
                  (unsigned long)millis(), (unsigned long)g_trace.count1, (unsigned long)g_trace.where1,
                  whereName(g_trace.where1), (unsigned long)g_trace.where0, whereName(g_trace.where0),
                  (unsigned long)(g_msgHead - g_msgTail), n, (unsigned long)g_governorSheds);
}

// Profile build: a hard fault on either core records its pc, lr and sp in
// the trace and spins; the watchdog in tracker_profile.cpp then reboots.
extern "C" __attribute__((used)) void profileHardFault(uint32_t* frame) {
    g_trace.faultCore = get_core_num();
    g_trace.faultPc = frame[6];
    g_trace.faultLr = frame[5];
    g_trace.faultSp = (uint32_t)frame;
    g_trace.faultMagic = Synth::PROFILE_TRACE_MAGIC;
    for (;;) {}
}

extern "C" __attribute__((naked)) void isr_hardfault() {
    asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b 2f\n"
        "1: mrs r0, msp\n"
        "2: ldr r1, =profileHardFault\n"
        "bx r1\n"
        ".ltorg\n");
}
#endif
