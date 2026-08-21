#pragma once
#include <Arduino.h>

// Onboard polyphonic synth: renders MIDI file playback directly to the
// board's I2S DAC (AUDIO_SDATA/AUDIO_BCLK/AUDIO_LRCLK in pins.h), so a
// file's musical content is audible without needing an external synth on
// the other end of the MIDI cable/USB port.
//
// Scope: a small set of algorithmic waveforms (sine/triangle/saw/square)
// and a per-GM-instrument-family envelope/waveform preset (16 families,
// selected by Program Change), plus a basic synthesized drum kit on the
// percussion channel (MIDI channel 10). This is meant to make different
// parts/instruments in a file distinguishable from each other for
// verification purposes -- not to sound like real instruments. See
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
void setVolume(uint8_t percent);

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
