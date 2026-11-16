#include "midi_output.h"
#include "pins.h"
#include "config.h"
#include "synth.h"

#include <Adafruit_TinyUSB.h>
#include <MIDI.h>

// ---- transports ----
Adafruit_USBD_MIDI usb_midi_transport;

// SysExMaxSize matches MIDI_SYSEX_MAX_LEN, so a message up to that size
// arrives whole. Built by hand since MIDI_CREATE_CUSTOM_INSTANCE only
// passes its settings to the transport, not to MidiInterface. Each
// instance holds one receive buffer of this size.
struct MidiInSettings : public midi::DefaultSettings {
    static const unsigned SysExMaxSize = MIDI_SYSEX_MAX_LEN;
};

midi::SerialMIDI<HardwareSerial> serialMIDIHW(Serial1);
midi::MidiInterface<midi::SerialMIDI<HardwareSerial>, MidiInSettings> MIDIHW(serialMIDIHW);
midi::SerialMIDI<Adafruit_USBD_MIDI> serialMIDIUSB(usb_midi_transport);
midi::MidiInterface<midi::SerialMIDI<Adafruit_USBD_MIDI>, MidiInSettings> MIDIUSB(serialMIDIUSB);

// Most bytes pumpIncoming() parses per transport per update(). The
// library parses one byte per read() call.
const int MAX_BYTES_PER_UPDATE = 64;

// Shared by the SysEx senders (SMF player, .syx player). Each fills and
// sends it within one call on core 0, so they never overlap.
uint8_t g_sysExScratch[MIDI_SYSEX_MAX_LEN + 1];

namespace {
MidiOutTarget g_target = MIDI_OUT_BOTH;
bool g_audioOutput = false;
MidiThruMode g_thruMode = MIDI_THRU_OFF;

inline bool hwEnabled()  { return g_target & MIDI_OUT_HARDWARE; }
inline bool usbEnabled() { return g_target & MIDI_OUT_USB; }

// Messages in and out on DIN or USB, for the status strip LEDs. Core 0 only.
uint32_t g_inActivity = 0;
uint32_t g_outActivity = 0;

// hwEnabled() and usbEnabled() for a send, counting it for the LED.
inline bool hwOut()  { bool on = hwEnabled(); g_outActivity += on; return on; }
inline bool usbOut() { bool on = usbEnabled(); g_outActivity += on; return on; }
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
        case 0xA0: iface.sendAfterTouch(data1, data2, channel1based); break;
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
    g_outActivity += toHw || toUsb;
    if (toHw)  sendChannelVoiceTo(MIDIHW, status, data1, data2);
    if (toUsb) sendChannelVoiceTo(MIDIUSB, status, data1, data2);
}

void routeThruSysEx(bool fromHw, const uint8_t* data, size_t len) {
    if (len == 1) {
        // A one-byte F7 escape can carry a real-time byte (e.g. F8).
        if (data[0] >= 0xF8) {
            if (hwEnabled())  MIDIHW.sendRealTime((midi::MidiType)data[0]);
            if (usbEnabled()) MIDIUSB.sendRealTime((midi::MidiType)data[0]);
        }
        return;
    }
    if (len < 2) return; // empty
    bool toHw, toUsb;
    thruDestinations(fromHw, toHw, toUsb);
    g_outActivity += toHw || toUsb;
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
void pumpOne(Interface& iface, bool fromHw) {
    if (!iface.read()) return;
    midi::MidiType t = iface.getType();
    if (t != midi::Clock && t != midi::ActiveSensing) g_inActivity++;
    bool thruActive = g_thruMode != MIDI_THRU_OFF;
    if (!g_inputHandler && !g_sysExHandler && !g_realtimeHandler && !thruActive) return;

    midi::MidiType type = iface.getType();

    if (type == midi::SystemExclusive) {
        const uint8_t* data = iface.getSysExArray();
        size_t len = iface.getSysExArrayLength();
        // A message longer than MIDI_SYSEX_MAX_LEN comes back as its last
        // chunk, starting with F7. Drop it.
        if (len == 0 || data[0] != 0xF0) return;
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

// Parses whatever `port` has waiting, up to MAX_BYTES_PER_UPDATE bytes.
template <typename Interface, typename Port>
void pumpIncoming(Interface& iface, Port& port, bool fromHw) {
    for (int n = 0; n < MAX_BYTES_PER_UPDATE && port.available() > 0; n++) pumpOne(iface, fromHw);
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
    Serial1.setFIFOSize(256); // default RX FIFO is 32 bytes, about 10 ms at 31250 baud
    MIDIHW.begin(MIDI_CHANNEL_OMNI);
    MIDIHW.turnThruOff();

    // USB MIDI
    MIDIUSB.begin(MIDI_CHANNEL_OMNI);
    MIDIUSB.turnThruOff();
    // main.cpp re-enumerates USB once the drive interface is added too.
}

void update() {
    // Always pumps both transports so the TinyUSB stack + libmidi
    // internal buffers don't stall, even with no input handler registered.
    pumpIncoming(MIDIHW, Serial1, true);
    pumpIncoming(MIDIUSB, usb_midi_transport, false);
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
    if (hwOut())  MIDIHW.sendNoteOn(note, velocity, channel + 1);
    if (usbOut()) MIDIUSB.sendNoteOn(note, velocity, channel + 1);
}

void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note < 128) g_noteChannelMask[note] &= (uint16_t)~(1u << channel);
    if (audioOutEnabled()) Synth::noteOff(channel, note);
    if (hwOut())  MIDIHW.sendNoteOff(note, velocity, channel + 1);
    if (usbOut()) MIDIUSB.sendNoteOff(note, velocity, channel + 1);
}

void sendControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    // Controllers always reach the synth so its channel state stays current
    // while audio is off; only notes are gated.
    Synth::controlChange(channel, controller, value);
    if (hwOut())  MIDIHW.sendControlChange(controller, value, channel + 1);
    if (usbOut()) MIDIUSB.sendControlChange(controller, value, channel + 1);
}

void sendProgramChange(uint8_t channel, uint8_t program) {
    // Selects which instrument-family preset the synth uses for
    // subsequent notes on this channel (see Synth::programChange()).
    if (audioOutEnabled()) Synth::programChange(channel, program);
    if (hwOut())  MIDIHW.sendProgramChange(program, channel + 1);
    if (usbOut()) MIDIUSB.sendProgramChange(program, channel + 1);
}

void sendPitchBend(uint8_t channel, int16_t bend) {
    Synth::pitchBend(channel, bend); // ungated, see sendControlChange()
    if (hwOut())  MIDIHW.sendPitchBend(bend, channel + 1);
    if (usbOut()) MIDIUSB.sendPitchBend(bend, channel + 1);
}

void sendAfterTouch(uint8_t channel, uint8_t pressure) {
    Synth::channelPressure(channel, pressure); // ungated, see sendControlChange()
    if (hwOut())  MIDIHW.sendAfterTouch(pressure, channel + 1);
    if (usbOut()) MIDIUSB.sendAfterTouch(pressure, channel + 1);
}

void sendPolyAfterTouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    if (hwOut())  MIDIHW.sendAfterTouch(note, pressure, channel + 1);
    if (usbOut()) MIDIUSB.sendAfterTouch(note, pressure, channel + 1);
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
    if (hwOut())  MIDIHW.sendSysEx((unsigned)len, data, true);
    if (usbOut()) MIDIUSB.sendSysEx((unsigned)len, data, true);
}

uint8_t* sysExScratch() { return g_sysExScratch; }

void allNotesOffAllChannels() {
    for (int note = 0; note < 128; note++) g_noteChannelMask[note] = 0;
    // CC123 below respects the sustain pedal, so the onboard synth gets its
    // own panic first, which releases every voice whatever the pedals say.
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

uint32_t inActivity() { return g_inActivity; }
uint32_t outActivity() { return g_outActivity; }

void setInputHandler(InputHandler handler) { g_inputHandler = handler; }
void setSysExHandler(SysExHandler handler) { g_sysExHandler = handler; }
void setRealtimeHandler(RealtimeHandler handler) { g_realtimeHandler = handler; }

} // namespace MidiOutput
