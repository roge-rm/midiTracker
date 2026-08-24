#include "midi_output.h"
#include "pins.h"
#include "synth.h"

#include <Adafruit_TinyUSB.h>
#include <MIDI.h>

// ---- transports ----
Adafruit_USBD_MIDI usb_midi_transport;

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDIHW);
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi_transport, MIDIUSB);

namespace {
MidiOutTarget g_target = MIDI_OUT_BOTH;
bool g_audioOutput = false;
MidiThruMode g_thruMode = MIDI_THRU_OFF;

inline bool hwEnabled()  { return g_target & MIDI_OUT_HARDWARE; }
inline bool usbEnabled() { return g_target & MIDI_OUT_USB; }
inline bool audioOutEnabled() { return g_audioOutput; }

// Which outgoing transport(s) a message arriving on `fromHw` should be
// echoed to under the current thru mode -- see MidiThruMode's doc comment
// in midi_output.h for what each mode means.
inline void thruDestinations(bool fromHw, bool& toHw, bool& toUsb) {
    switch (g_thruMode) {
        case MIDI_THRU_OFF:     toHw = false;    toUsb = false;   break;
        case MIDI_THRU_ON:      toHw = true;     toUsb = true;    break;
        case MIDI_THRU_TRS2USB: toHw = false;    toUsb = fromHw;  break;
        case MIDI_THRU_USB2TRS: toHw = !fromHw;  toUsb = false;   break;
        case MIDI_THRU_TRS2TRS: toHw = fromHw;   toUsb = false;   break;
        case MIDI_THRU_USB2USB: toHw = false;    toUsb = !fromHw; break;
    }
}

// Raw-sends one channel-voice message to a specific transport, bypassing
// sendNoteOn()/etc entirely -- thru'd input isn't this device "playing"
// anything, so it must never touch the note-activity visualizer or the
// onboard synth. Templated for the same reason pumpIncoming() is.
template <typename Interface>
void sendChannelVoiceTo(Interface& iface, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t type = status & 0xF0;
    uint8_t channel1based = (uint8_t)((status & 0x0F) + 1);
    switch (type) {
        case 0x80: iface.sendNoteOff(data1, data2, channel1based); break;
        case 0x90: iface.sendNoteOn(data1, data2, channel1based); break;
        case 0xA0: iface.sendPolyPressure(data1, data2, channel1based); break;
        case 0xB0: iface.sendControlChange(data1, data2, channel1based); break;
        case 0xC0: iface.sendProgramChange(data1, channel1based); break;
        case 0xD0: iface.sendAfterTouch(data1, channel1based); break;
        case 0xE0: {
            int16_t bend = (int16_t)(((uint16_t)data2 << 7) | data1) - 8192;
            iface.sendPitchBend(bend, channel1based);
            break;
        }
        default: break;
    }
}

void routeThruChannelVoice(bool fromHw, uint8_t status, uint8_t data1, uint8_t data2) {
    bool toHw, toUsb;
    thruDestinations(fromHw, toHw, toUsb);
    if (toHw)  sendChannelVoiceTo(MIDIHW, status, data1, data2);
    if (toUsb) sendChannelVoiceTo(MIDIUSB, status, data1, data2);
}

void routeThruSysEx(bool fromHw, const uint8_t* data, size_t len) {
    if (len < 2) return; // smaller than a bare F0 F7 -- not a real message
    bool toHw, toUsb;
    thruDestinations(fromHw, toHw, toUsb);
    if (toHw)  MIDIHW.sendSysEx((unsigned)len, data, true);
    if (toUsb) MIDIUSB.sendSysEx((unsigned)len, data, true);
}

void routeThruRealtime(bool fromHw, midi::MidiType type) {
    bool toHw, toUsb;
    thruDestinations(fromHw, toHw, toUsb);
    if (toHw)  MIDIHW.sendRealTime(type);
    if (toUsb) MIDIUSB.sendRealTime(type);
}

// Per-note bitmask of which of the 16 channels currently hold that note
// down, plus the velocity it was last struck with. Feeds the UI's live
// note-activity visualizer; updated unconditionally (regardless of
// hw/usb enablement) so it always reflects what the player is doing.
uint16_t g_noteChannelMask[128] = {0};
uint8_t  g_noteVelocity[128] = {0};

MidiOutput::InputHandler g_inputHandler = nullptr;
MidiOutput::SysExHandler g_sysExHandler = nullptr;
MidiOutput::RealtimeHandler g_realtimeHandler = nullptr;

// Reads one message off `iface` if available, thru-routes it (see
// routeThruChannelVoice()/routeThruSysEx()/routeThruRealtime()) and
// forwards it to whichever handler fits: System Exclusive to
// g_sysExHandler (its own callback -- see midi_output.h's comment on why
// it can't share InputHandler's shape), Clock/Start/Stop/Continue to
// g_realtimeHandler, other channel-voice types to g_inputHandler.
// `fromHw` says which physical transport `iface` is, since thru routing
// depends on source as well as destination. Templated since MIDIHW and
// MIDIUSB are distinct MidiInterface<Transport> instantiations with the
// same API.
template <typename Interface>
void pumpIncoming(Interface& iface, bool fromHw) {
    if (!iface.read()) return;
    bool thruActive = g_thruMode != MIDI_THRU_OFF;
    if (!g_inputHandler && !g_sysExHandler && !g_realtimeHandler && !thruActive) return;

    midi::MidiType type = iface.getType();

    if (type == midi::SystemExclusive) {
        const uint8_t* data = iface.getSysExArray();
        size_t len = iface.getSysExArrayLength();
        if (thruActive) routeThruSysEx(fromHw, data, len);
        if (g_sysExHandler) g_sysExHandler(data, len);
        return;
    }

    // Active Sensing/System Reset are thru'd like the rest of this group
    // but have no MidiRealtimeEvent counterpart -- nothing in this app
    // reacts to them (see MidiRealtimeEvent's doc comment).
    if (type == midi::Clock || type == midi::Start || type == midi::Continue ||
        type == midi::Stop || type == midi::ActiveSensing || type == midi::SystemReset) {
        if (thruActive) routeThruRealtime(fromHw, type);
        if (g_realtimeHandler) {
            switch (type) {
                case midi::Clock:    g_realtimeHandler(MIDI_RT_CLOCK); break;
                case midi::Start:    g_realtimeHandler(MIDI_RT_START); break;
                case midi::Continue: g_realtimeHandler(MIDI_RT_CONTINUE); break;
                case midi::Stop:     g_realtimeHandler(MIDI_RT_STOP); break;
                default: break;
            }
        }
        return;
    }

    uint8_t high = (uint8_t)type & 0xF0;
    if (high < 0x80 || high > 0xE0) return; // channel voice messages only

    uint8_t channel = iface.getChannel(); // 1..16
    if (channel < 1 || channel > 16) return;

    uint8_t status = high | (uint8_t)(channel - 1);
    uint8_t len = (high == 0xC0 || high == 0xD0) ? 1 : 2;
    if (thruActive) routeThruChannelVoice(fromHw, status, iface.getData1(), iface.getData2());
    if (g_inputHandler) g_inputHandler(status, iface.getData1(), iface.getData2(), len);
}
} // namespace

namespace MidiOutput {

void begin() {
    // Hardware UART MIDI. MIDI_OUT_PIN/MIDI_IN_PIN (GPIO0/1) are UART0's
    // default TX/RX on the earlephilhower core, so no pin reassignment
    // needed, but set them explicitly in case the core's default ever
    // changes underneath us.
    Serial1.setTX(MIDI_OUT_PIN);
    Serial1.setRX(MIDI_IN_PIN);
    MIDIHW.begin(MIDI_CHANNEL_OMNI);
    MIDIHW.turnThruOff();

    // USB MIDI
    MIDIUSB.begin(MIDI_CHANNEL_OMNI);
    MIDIUSB.turnThruOff();
}

void update() {
    // Always pumps both transports so the TinyUSB stack + libmidi
    // internal buffers don't stall, even with no input handler registered.
    pumpIncoming(MIDIHW, true);
    pumpIncoming(MIDIUSB, false);
}

void setTarget(MidiOutTarget target) { g_target = target; }
MidiOutTarget getTarget() { return g_target; }

void setThruMode(MidiThruMode mode) { g_thruMode = mode; }
MidiThruMode getThruMode() { return g_thruMode; }

void setAudioOutput(bool enabled) { g_audioOutput = enabled; }
bool audioOutputEnabled() { return g_audioOutput; }

void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note < 128) {
        // A NoteOn with velocity 0 is a NoteOff in disguise (see sendRaw).
        // Tracked unconditionally (regardless of audio/MIDI enablement) so
        // the UI's note-activity visualizer keeps working no matter what's
        // actually selected.
        if (velocity > 0) {
            g_noteChannelMask[note] |= (uint16_t)(1u << channel);
            g_noteVelocity[note] = velocity;
        } else {
            g_noteChannelMask[note] &= (uint16_t)~(1u << channel);
        }
    }
    if (audioOutEnabled()) Synth::noteOn(channel, note, velocity);
    if (hwEnabled())  MIDIHW.sendNoteOn(note, velocity, channel + 1);
    if (usbEnabled()) MIDIUSB.sendNoteOn(note, velocity, channel + 1);
}

void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note < 128) g_noteChannelMask[note] &= (uint16_t)~(1u << channel);
    if (audioOutEnabled()) Synth::noteOff(note);
    if (hwEnabled())  MIDIHW.sendNoteOff(note, velocity, channel + 1);
    if (usbEnabled()) MIDIUSB.sendNoteOff(note, velocity, channel + 1);
}

void sendControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (hwEnabled())  MIDIHW.sendControlChange(controller, value, channel + 1);
    if (usbEnabled()) MIDIUSB.sendControlChange(controller, value, channel + 1);
}

void sendProgramChange(uint8_t channel, uint8_t program) {
    // Selects which instrument-family preset the synth uses for
    // subsequent notes on this channel (see Synth::programChange()).
    if (audioOutEnabled()) Synth::programChange(channel, program);
    if (hwEnabled())  MIDIHW.sendProgramChange(program, channel + 1);
    if (usbEnabled()) MIDIUSB.sendProgramChange(program, channel + 1);
}

void sendPitchBend(uint8_t channel, int16_t bend) {
    if (hwEnabled())  MIDIHW.sendPitchBend(bend, channel + 1);
    if (usbEnabled()) MIDIUSB.sendPitchBend(bend, channel + 1);
}

void sendAfterTouch(uint8_t channel, uint8_t pressure) {
    if (hwEnabled())  MIDIHW.sendAfterTouch(pressure, channel + 1);
    if (usbEnabled()) MIDIUSB.sendAfterTouch(pressure, channel + 1);
}

void sendPolyAfterTouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    if (hwEnabled())  MIDIHW.sendPolyPressure(note, pressure, channel + 1);
    if (usbEnabled()) MIDIUSB.sendPolyPressure(note, pressure, channel + 1);
}

void sendRaw(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
    uint8_t type = status & 0xF0;
    uint8_t channel = status & 0x0F;

    switch (type) {
        case 0x80: sendNoteOff(channel, data1, data2); break;
        case 0x90:
            // A NoteOn with velocity 0 is a NoteOff in disguise; the
            // MIDI lib handles this fine either way via sendNoteOn.
            sendNoteOn(channel, data1, data2);
            break;
        case 0xA0: sendPolyAfterTouch(channel, data1, data2); break;
        case 0xB0: sendControlChange(channel, data1, data2); break;
        case 0xC0: sendProgramChange(channel, data1); break;
        case 0xD0: sendAfterTouch(channel, data1); break;
        case 0xE0: {
            int16_t bend = (int16_t)(((uint16_t)data2 << 7) | data1) - 8192;
            sendPitchBend(channel, bend);
            break;
        }
        default:
            // System messages etc: not needed for SMF playback, ignore.
            break;
    }
    (void)len;
}

void sendSysExRaw(const uint8_t* data, size_t len) {
    if (len < 2) return; // smaller than a bare F0 F7 -- not a real message
    // `true` = data already includes the F0/F7 boundaries -- see this
    // function's header comment.
    if (hwEnabled())  MIDIHW.sendSysEx((unsigned)len, data, true);
    if (usbEnabled()) MIDIUSB.sendSysEx((unsigned)len, data, true);
}

void allNotesOffAllChannels() {
    for (int note = 0; note < 128; note++) g_noteChannelMask[note] = 0;
    // The onboard synth doesn't parse CC 120/123 (it has no CC handling at
    // all yet), so it needs its own explicit panic -- otherwise stopping
    // playback would leave it sounding stuck notes indefinitely.
    Synth::allNotesOff();
    for (uint8_t ch = 0; ch < 16; ch++) {
        sendControlChange(ch, 120, 0); // All Sound Off
        sendControlChange(ch, 123, 0); // All Notes Off
    }
}

bool isNoteActive(uint8_t note) {
    return note < 128 && g_noteChannelMask[note] != 0;
}

bool isNotePercussion(uint8_t note) {
    return note < 128 && (g_noteChannelMask[note] & (1u << 9)) != 0;
}

uint8_t noteVelocity(uint8_t note) {
    return note < 128 ? g_noteVelocity[note] : 0;
}

bool isChannelActive(uint8_t channel) {
    if (channel >= 16) return false;
    uint16_t bit = (uint16_t)(1u << channel);
    for (int note = 0; note < 128; note++) {
        if (g_noteChannelMask[note] & bit) return true;
    }
    return false;
}

uint16_t activeChannelMask() {
    uint16_t mask = 0;
    for (int note = 0; note < 128; note++) mask |= g_noteChannelMask[note];
    return mask;
}

void noteActivityIn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note >= 128) return;
    if (velocity > 0) {
        g_noteChannelMask[note] |= (uint16_t)(1u << channel);
        g_noteVelocity[note] = velocity;
    } else {
        g_noteChannelMask[note] &= (uint16_t)~(1u << channel);
    }
}

void setInputHandler(InputHandler handler) { g_inputHandler = handler; }
void setSysExHandler(SysExHandler handler) { g_sysExHandler = handler; }
void setRealtimeHandler(RealtimeHandler handler) { g_realtimeHandler = handler; }

} // namespace MidiOutput
