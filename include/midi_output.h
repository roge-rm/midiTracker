#pragma once
#include <Arduino.h>

enum MidiOutTarget {
    MIDI_OUT_HARDWARE = 0x01,
    MIDI_OUT_USB      = 0x02,
    MIDI_OUT_BOTH      = 0x03,
};

// Which incoming transport(s) get echoed to which outgoing transport(s),
// independent of everything else in this file: setTarget() controls where
// THIS DEVICE's own outgoing playback/synth MIDI goes, and thru controls
// whether/how EXTERNAL MIDI arriving on either transport gets passed back
// out. OFF disables thru entirely (the default). ON mirrors every input to
// every output, including a transport back to itself (e.g. hardware in ->
// hardware out), which is the same self-echo a dedicated hardware MIDI
// Thru port performs. The four directional modes each enable exactly one
// of the four possible (from, to) pairs and nothing else.
enum MidiThruMode {
    MIDI_THRU_OFF = 0,
    MIDI_THRU_ON,
    MIDI_THRU_TRS2USB,  // hardware in -> USB out only
    MIDI_THRU_USB2TRS,  // USB in -> hardware out only
    MIDI_THRU_TRS2TRS,  // hardware in -> hardware out only
    MIDI_THRU_USB2USB,  // USB in -> USB out only
};

// Real-time transport/clock events -- MIDI Clock (24 pulses per quarter
// note), Start, Stop, Continue. Active Sensing and System Reset are
// thru'd the same as these (see MidiThruMode above) but aren't surfaced
// here: nothing in this app reacts to them.
enum MidiRealtimeEvent {
    MIDI_RT_CLOCK,
    MIDI_RT_START,
    MIDI_RT_STOP,
    MIDI_RT_CONTINUE,
};

// Thin wrapper so the player doesn't care which physical transport(s)
// are active. Call begin() once at startup, then use send*() during
// playback. update() must be called regularly (pumps USB MIDI).
namespace MidiOutput {

void begin();
void update();

// Which transport(s) are currently enabled. Defaults to MIDI_OUT_BOTH.
void setTarget(MidiOutTarget target);
MidiOutTarget getTarget();

// See MidiThruMode above. Defaults to MIDI_THRU_OFF. Thru'd messages are
// raw-forwarded directly to the target transport(s) -- they never pass
// through sendNoteOn()/etc, so they don't touch the onboard synth, the
// note-activity visualizer, or setTarget()'s hw/usb enablement, all of
// which are about this device's own outgoing MIDI, not passthrough.
void setThruMode(MidiThruMode mode);
MidiThruMode getThruMode();

// Whether the onboard synth (see synth.h) receives note on/off (and the
// panic response). Defaults to false -- it requires an explicit opt-in
// (EDIT on the now-playing screen) rather than playing automatically,
// since it's meant as an on-demand way to verify a file's content, not a
// default playback path. Purely additive: it has no effect on MIDI
// out (HW/USB), which always follows setTarget() alone regardless of this
// setting -- actual MIDI wire/USB output was never a "sound" in its own
// right, so there was nothing for this to usefully gate there.
void setAudioOutput(bool enabled);
bool audioOutputEnabled();

void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
void sendControlChange(uint8_t channel, uint8_t controller, uint8_t value);
void sendProgramChange(uint8_t channel, uint8_t program);
void sendPitchBend(uint8_t channel, int16_t bend); // -8192..8191
void sendAfterTouch(uint8_t channel, uint8_t pressure);
void sendPolyAfterTouch(uint8_t channel, uint8_t note, uint8_t pressure);

// Generic 1-3 byte raw channel voice message, used by the SMF player so
// it doesn't need a case for every status type.
void sendRaw(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len);

// Sends one complete System Exclusive message. `data` must be boundary-
// inclusive: data[0] == 0xF0, data[len-1] == 0xF7 -- the same shape the
// MIDI library's own receive side hands back (see setSysExHandler()) and
// what's stored on disk (embedded SMF SysEx events and raw .syx dumps
// both keep the trailing 0xF7; only the leading 0xF0 is ever stripped,
// when SMF's own event-type byte already carries that role).
void sendSysExRaw(const uint8_t* data, size_t len);

// Sends All Notes Off (CC 123) + All Sound Off (CC 120) on every channel
// on whichever transport(s) are active. Call this on stop/panic.
void allNotesOffAllChannels();

// Live note-activity query for the UI's now-playing visualizer. Reflects
// notes currently held (merged across all 16 channels) as observed by
// sendNoteOn()/sendNoteOff(), independent of which transport(s) are
// enabled -- so the visualizer still shows activity even if, say, only
// USB output is selected.
bool isNoteActive(uint8_t note);
uint8_t noteVelocity(uint8_t note); // last note-on velocity; meaningful while isNoteActive() is true

// True if `note` is currently held on the percussion channel (MIDI
// channel 10, 0-indexed channel 9 -- same GM convention Synth uses for
// its drum kit) specifically, regardless of whether it's also held on
// some other channel at the same time. Lets the UI's note-activity
// visualizer color drum hits differently from melodic notes even though
// they share the same note-number range.
bool isNotePercussion(uint8_t note);

// True if any note is currently held on `channel` (0-15) specifically --
// derived from the same per-note channel bitmask isNoteActive() reads, so
// it reflects both real outgoing notes (sendNoteOn()/sendNoteOff(), e.g.
// file playback) and visualization-only ones (noteActivityIn(), e.g.
// pariSynth's own live MIDI input) with no extra bookkeeping of its own --
// a caller doesn't need to know or care which source lit a channel up.
// Drives pariSynth's per-channel "receiving notes" glow (see
// Ui::drawPariSynthPlay()'s activeMask parameter).
bool isChannelActive(uint8_t channel);

// All 16 channels' isChannelActive() at once, as a bitmask (bit N =
// channel N) -- one pass over the same per-note data instead of 16
// separate scans, for callers that want the whole picture at once (a
// full-grid redraw) rather than checking one channel at a time (a single
// Note On/Off event).
uint16_t activeChannelMask();

// Visualization-only bookkeeping for a note this device did NOT play
// itself but wants shown on the note-activity strip anyway -- namely
// PariSynthMode's live incoming-MIDI input, which drives Synth::noteOn()/
// noteOff() directly rather than through sendNoteOn()/sendNoteOff() (that
// pair would additionally transmit the note back out over HW/USB, double-
// sending on top of whatever MIDI Thru already independently forwards).
// Same channel-bitmask/velocity bookkeeping as sendNoteOn()/sendNoteOff(),
// just without the Synth::/HW/USB side effects; `velocity` 0 means note-
// off, same convention used everywhere else in this file.
void noteActivityIn(uint8_t channel, uint8_t note, uint8_t velocity);

// Called for every incoming channel-voice message read from either
// transport (merged into one stream), e.g. to feed a MidiRecorder.
// `len` is 1 for Program Change/Channel Pressure, 2 otherwise.
typedef void (*InputHandler)(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len);

// Registers the handler above; pass nullptr to disable. update() always
// pumps both transports' incoming data regardless of whether a handler
// is registered (needed to keep the USB/serial stacks from stalling).
void setInputHandler(InputHandler handler);

// Called for every incoming System Exclusive message read from either
// transport, separately from InputHandler above -- SysEx has no channel
// and no fixed data-byte count, so it doesn't fit that callback's shape.
// `data` is boundary-inclusive (data[0]==0xF0, data[len-1]==0xF7) and
// only valid for the duration of the call -- it points at the MIDI
// library's own internal receive buffer (up to 1024 bytes, its
// SysExMaxSize), which the next incoming message overwrites.
typedef void (*SysExHandler)(const uint8_t* data, size_t len);

// Registers the handler above; pass nullptr to disable. Independent of
// setInputHandler() -- a mode can register one, both, or neither (e.g.
// a SysEx-only capture screen registers just this one, so it never
// reacts to ordinary channel-voice input).
void setSysExHandler(SysExHandler handler);

// Called for every incoming Clock/Start/Stop/Continue message read from
// either transport, separately from InputHandler/SysExHandler above --
// see MidiRealtimeEvent's doc comment for scope.
typedef void (*RealtimeHandler)(MidiRealtimeEvent event);

// Registers the handler above; pass nullptr to disable. Same "whichever
// mode is currently active claims this in its own enter()" convention as
// setInputHandler()/setSysExHandler() -- see those for why.
void setRealtimeHandler(RealtimeHandler handler);

} // namespace MidiOutput
