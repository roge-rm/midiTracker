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

} // namespace Synth
