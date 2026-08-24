#pragma once
#include <Arduino.h>

// Onboard polyphonic synth: renders MIDI file playback directly to the
// board's I2S DAC (AUDIO_SDATA/AUDIO_BCLK/AUDIO_LRCLK in pins.h), so a
// file's musical content is audible without needing an external synth on
// the other end of the MIDI cable/USB port.
//
// Scope: a small set of algorithmic waveforms (sine/triangle/saw/square)
// run through a fixed one-pole low-pass filter and a shared LFO (vibrato,
// tremolo, and square-wave PWM, all off the same LFO phase -- see
// g_lfoPhase in synth.cpp), and a per-GM-instrument-family envelope/
// waveform/cutoff/vibrato/tremolo/PWM preset (16 families, selected by
// Program Change), plus a basic synthesized drum kit on the percussion
// channel (MIDI channel 10). Melodic voices are spread across the stereo
// field by MIDI channel (drums stay centered) -- see Voice::pan in
// synth.cpp. This is meant to make different parts/instruments in a file
// distinguishable from each other for verification purposes -- not to
// sound like real instruments. See
// MidiOutput::sendNoteOn/sendNoteOff/sendProgramChange in midi_output.cpp,
// which is what actually drives this (every outgoing note/program change
// from SMF playback reaches the synth the same way it reaches the HW/USB
// transports).
//
// Runs entirely on the RP2040's second core (setup1()/loop1() in
// synth.cpp) so audio rendering never competes with the UI/SD/USB work on
// core 0; noteOn()/noteOff()/programChange()/allNotesOff() just push a
// message across the inter-core FIFO and return immediately.
namespace Synth {

// Signals core 1 that it's safe to claim a PIO state machine for I2S. The
// RP2040 has only 8 PIO state machines total; SdFat's SDIO driver claims
// an entire 4-SM PIO block for the SD card, and core 1's setup1()
// otherwise starts essentially in parallel with core 0's setup(), so
// without this gate the I2S claim could race ahead of and starve the SD
// card driver's own claim, breaking SD card init. (TFT_eSPI uses
// hardware SPI here, not PIO -- see the platformio.ini comment by
// RP2040_PIO_SPI for why that must stay unset.) MUST be called from
// core 0's setup() strictly after sdCardBegin() has run, whether or not
// it succeeded -- the PIO claim attempt happens either way.
void begin();

// `channel` is 0-15 (MIDI channel 10 == channel 9 here) and selects
// between the melodic instrument-preset path (via the program last set
// on that channel, see programChange()) and the drum-kit path (channel 9).
void noteOn(uint8_t channel, uint8_t note, uint8_t velocity);
void noteOff(uint8_t note); // channel-blind by design, see synth.cpp

// Selects which of the 16 GM instrument-family presets `channel` uses for
// subsequent noteOn() calls (program / 8 = family index). Percussion
// (channel 9) ignores this entirely -- its sound is chosen by note number
// instead, per the GM drum key map.
void programChange(uint8_t channel, uint8_t program);

// Resets every channel back to program 0 (Acoustic Grand Piano family).
// Call this whenever a genuinely new playback session starts (a fresh
// load(), not a pause/resume) so a previous file's instrument choices
// don't bleed into the next one before it sends its own Program Change.
void resetPrograms();

// Fast-releases every currently-sounding voice. Called alongside
// MidiOutput::allNotesOffAllChannels() (stop/pause/panic) so the synth
// doesn't leave notes stuck on when playback halts -- unlike a real MIDI
// receiver, it has no CC 120/123 parser to catch those messages otherwise.
void allNotesOff();

// Master output volume, 0-100. Applied as a final attenuation after the
// mix's soft limiter (see renderSample() in synth.cpp), so it's a pure
// output level control -- it doesn't change limiting/clipping behavior at
// a given polyphony, just how loud the result comes out. Defaults to 80.
// `percent` is a UI-facing slider position, not a linear gain: internally
// it's mapped through a cubic audio taper (see volumePercentToGainQ16() in
// synth.cpp) so the perceived loudness scales roughly evenly across the
// range, rather than a plain linear percent/100 multiply cramming nearly
// all the audible range into the low end (confirmed on real hardware --
// 0% was correctly silent, but 5% was already uncomfortably loud, and not
// just for synth voices: WAV and .mod playback too, at the same 5%,
// despite sharing no mixing code with each other or with the synth. Only
// this final volume step is common to all three, which is what pointed at
// a linear-taper fader rather than any one engine's mixing math).
void setVolume(uint8_t percent);

// Coarse output-level attenuation on top of setVolume() above, matching
// the picoTracker v2 reference firmware's own Headphone Low / Headphone
// High / Line Level choice (0/1/2) -- see g_outputLevelShift's long
// comment in synth.cpp for why this exists at all: this board's DAC has
// no gain control of its own and its headphone amp is already hardware-
// strapped to its lowest gain step, so digital attenuation (this, plus
// the Volume taper) is the only lever available, on this firmware or the
// original. Defaults to Headphone Low (0, the safest/quietest) until
// this is called.
void setOutputLevel(uint8_t level);

// On/off for the lo-fi reverb applied to the onboard MIDI synth's own
// voices (not WAV/tracker playback -- see reverbProcess()'s long comment
// in synth.cpp for why) -- see that same comment for what it actually is
// and why it's deliberately lo-fi/chiptune-flavored rather than a generic
// "nice hall" sound. Defaults on. Off costs nothing extra (the tank
// itself is skipped, not just muted), and re-enabling starts from a
// cold/silent tank rather than resuming a stale one.
void setReverbEnabled(bool enabled);

// Reverb wet/dry mix, 0-100%. Not a literal 0-100% wet blend -- it scales
// within the ceiling this effect was actually tuned/reasoned within (see
// REVERB_WET_MAX_Q16 in synth.cpp), so 100% means "as strong as this
// reverb was designed to go," not "fully wet." Defaults to 70.
void setReverbMix(uint8_t percent);

// Which reverb algorithm runs, 0-2: 0 = Lo-fi/chiptune (bitcrushed/
// decimated tank, the original), 1 = Lush (the tank's tail run through a
// slow LFO-wobbled chorus instead), 2 = Shimmer (the tail pitch-shifted up
// an octave and fed back into itself for an ascending, evolving wash --
// see synth.cpp's g_reverbType/chorusProcess()/shimmerProcess() comments
// for how each actually works). Only matters while setReverbEnabled(true)
// -- the value is remembered either way, so switching Reverb back on
// resumes whichever type was last selected, same as Reverb Mix already
// does. An out-of-range value is treated as 0. Defaults to 0.
void setReverbType(uint8_t type);

// -- Instrument/drum preset editing (pariSynth) ----------------------------
//
// Live editing of the same per-GM-family/per-drum-type presets described
// at the top of this file -- shared with file/tracker playback, not a
// separate patch bank, so editing e.g. the Piano family here also changes
// how a .mid using Piano sounds afterwards. Safe to call at any time,
// including while notes are currently sounding on the record being
// edited: every preset field is only ever read at note-start (see
// startMelodicVoice()/startPercussionVoice() in synth.cpp), never per-
// sample, so an edit can only affect the *next* note-on for that family/
// drum type -- already-sounding voices keep whatever they snapshotted at
// their own note-on, with no click or audible discontinuity. Both cores
// share plain SRAM (see Synth::begin()'s g_readyForPioInit precedent), so
// these setters write the shared preset tables directly from core 0; a
// one-word "dirty" doorbell over the same inter-core FIFO used elsewhere
// in this file then tells core 1 to recompute that one record's derived
// caches (filter alpha, vibrato/tremolo/PWM depth). Getters read the same
// shared tables directly -- no FIFO round trip needed for a read.

enum SynthWaveform : uint8_t { SYNTH_WAVE_SINE, SYNTH_WAVE_TRIANGLE, SYNTH_WAVE_SAW, SYNTH_WAVE_SQUARE, SYNTH_WAVE_NOISE };

const uint8_t INSTRUMENT_FAMILY_COUNT = 16; // matches PRESETS[16] in synth.cpp
const uint8_t DRUM_PRESET_COUNT = 8;        // matches DrumType/DRUM_PRESETS in synth.cpp

// Mirrors the TU-local InstrumentPreset/DrumPreset structs in synth.cpp --
// duplicated here (rather than shared) because those stay private to
// synth.cpp's anonymous namespace; this is the public field-for-field
// shape core 0 callers (the pariSynth editor) read/write through.
struct InstrumentPresetParams {
    SynthWaveform waveform;
    uint16_t attackSamples;
    uint16_t decaySamples;
    uint8_t sustainPercent;   // 0-100
    uint16_t releaseSamples;
    float cutoffHz;
    float vibratoDepthPercent;
    float tremoloDepthPercent;
    float pwmDepthPercent;    // only audible when waveform == SYNTH_WAVE_SQUARE
};

struct DrumPresetParams {
    SynthWaveform waveform;
    float basePitchHz;
    uint16_t decaySamples;
    float pitchDropStartHz;   // 0 = no pitch sweep
    uint16_t pitchDropSamples;
    float cutoffHz;
};

// `family` is 0-15 (program / 8 -- see programChange()'s own comment).
// Short display name for a melodic family, e.g. "Piano", "Strings".
const char* instrumentFamilyName(uint8_t family);
void getInstrumentPreset(uint8_t family, InstrumentPresetParams& out);
void setInstrumentPreset(uint8_t family, const InstrumentPresetParams& in);
void resetInstrumentPresetToDefault(uint8_t family);

// `drumType` is 0-7, one of the archetypes GM percussion notes classify
// into (see classifyDrum() in synth.cpp) -- not a GM note number.
const char* drumPresetName(uint8_t drumType);
void getDrumPreset(uint8_t drumType, DrumPresetParams& out);
void setDrumPreset(uint8_t drumType, const DrumPresetParams& in);
void resetDrumPresetToDefault(uint8_t drumType);

// Restores every melodic family and drum type to its shipped default in
// one call (the pariSynth editor's "Reset to Default" action).
void resetAllPresetsToDefault();

// Read-through accessor for g_channelProgram -- lets the UI show e.g.
// "channel 3 -> Strings" without duplicating channel-program tracking
// outside synth.cpp. Percussion (channel 9) always reads back whatever
// was last set even though programChange() ignores it for sound
// selection -- callers should special-case channel 9 as "Drums" rather
// than resolving its program through instrumentFamilyName().
uint8_t getChannelProgram(uint8_t channel);

// -- WAV playback stream --------------------------------------------------
//
// A second, independent audio source mixed into the same per-sample
// pipeline described above (renderSample() -> softLimit() -> g_i2s), since
// the RP2040 doesn't have PIO/DMA resources to spare for a second I2S
// output. Unlike noteOn()/etc, this is bulk continuous audio, not discrete
// events, so it's fed through a small ring buffer rather than the
// inter-core FIFO: WavPlayer (running on core 0, alongside SD/UI work)
// pushes already-converted 44.1kHz interleaved stereo frames in; loop1()
// (core 1) pops one frame per sample and mixes it in, distinctly per
// channel -- WavPlayer is responsible for all format conversion (bit
// depth, channel count, sample-rate resampling) before anything reaches
// here, so this stream is always exactly 44.1kHz/stereo/int16, and core 1
// never branches on source format.
//
// This ring buffer is the only cushion between a core-0 SD stall and an
// audible dropout in the WAV stream (much like the I2S DMA buffer already
// cushions core 1 itself against those same stalls -- see setup1()'s
// comment). WavPlayer deliberately keeps it topped up regardless of
// play/pause state (see wavStreamSetActive() below) rather than only
// filling it while actively playing -- that way there's no startup gap
// the moment playback (re)starts, since audibility and buffer-filling are
// two independent things.

// Full reset: empties the ring buffer, clears the ended/underrun flags,
// and silences output (same as wavStreamSetActive(false)). Call on a
// fresh load(), on close()/stop(), and on seekTo() -- anywhere the
// buffered content no longer corresponds to the current read position and
// needs to be discarded rather than continued from.
void wavStreamReset();

// Toggles whether the consumer actually mixes buffered frames into the
// output -- pause()/play() call this alone (not wavStreamReset()), so a
// brief pause doesn't throw away whatever's already buffered ahead.
void wavStreamSetActive(bool active);

// Pushes up to `count` interleaved stereo frames (2 int16 per frame).
// Returns how many were actually accepted (0..count) -- never blocks, so
// core 0 is never stalled waiting on core 1; a caller that gets back
// fewer than requested should carry the remainder over to its next call.
size_t wavStreamWrite(const int16_t* frames, size_t count);

// Free space in frames, so a producer can size its next read/resample
// batch instead of guessing or attempting a write it knows will be
// partially rejected.
size_t wavStreamFree();

// No more source data coming (EOF reached). Once the consumer drains
// what's already buffered, it stops mixing WAV audio in on its own --
// this is a normal, expected drain, not treated as an underrun.
void wavStreamEnd();

// True if the consumer wanted a frame (stream active, not yet
// wavStreamEnd()'d) and found the buffer empty since the last call to
// this function -- auto-clears on read. Meant for a small on-screen
// indicator; an underrun here means a momentary audio dropout, not a
// crash.
bool wavStreamTookUnderrun();

// Temporary real-hardware diagnostic -- total individual sample periods
// the consumer has ever found the buffer empty (monotonically
// increasing, never resets) -- a real duration/severity measure, unlike
// wavStreamTookUnderrun()'s one-shot boolean. Callers diff two readings
// to see how many samples were actually dropped in a given window.
uint32_t wavStreamUnderrunSamples();

} // namespace Synth
