#include "midi_output.h"
#include "pins.h"
#include "config.h"
#include "synth.h"
#include "ui_tap.h"
#include "sysex_stream.h"
#include "thru_hold.h"

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

// The library's serial transport, keeping the last byte the library read
// so pumpOne() can pass it to the SysEx stream parser too.
template <class Port>
class TapSerialMIDI : public midi::SerialMIDI<Port> {
public:
    explicit TapSerialMIDI(Port& port) : midi::SerialMIDI<Port>(port) {}
    byte read() {
        byte b = midi::SerialMIDI<Port>::read();
        tapped = b;
        return b;
    }
    int tapped = -1; // -1 = nothing read
};

TapSerialMIDI<HardwareSerial> serialMIDIHW(Serial1);
midi::MidiInterface<TapSerialMIDI<HardwareSerial>, MidiInSettings> MIDIHW(serialMIDIHW);
TapSerialMIDI<Adafruit_USBD_MIDI> serialMIDIUSB(usb_midi_transport);
midi::MidiInterface<TapSerialMIDI<Adafruit_USBD_MIDI>, MidiInSettings> MIDIUSB(serialMIDIUSB);

// SysEx of any length from each transport, see setSysExStreamHandler().
SysExInParser g_sysExInHw(0);
SysExInParser g_sysExInUsb(1);

// Most bytes pumpIncoming() parses per transport per update(). The
// library parses one byte per read() call.
const int MAX_BYTES_PER_UPDATE = 64;

// For the SMF player's SysEx, filled and sent within one call on core 0.
uint8_t g_sysExScratch[MIDI_SYSEX_MAX_LEN + 1];

namespace {
MidiOutTarget g_target = MIDI_OUT_BOTH;
bool g_audioOutput = false;
MidiThruMode g_thruMode = MIDI_THRU_OFF;

inline bool hwEnabled()  { return g_target & MIDI_OUT_HARDWARE; }
inline bool usbEnabled() { return g_target & MIDI_OUT_USB; }

// Streamed SysEx out, see sysExOutBegin(). While `busy`, nothing but
// realtime bytes goes to the outputs it uses. Core 0 only.
struct SysExOut {
    bool busy = false;
    bool hw = false;
    bool usb = false;
    bool ended = false;       // last byte taken was F7
    uint8_t usbPend[2];       // bytes waiting for a full USB packet
    uint8_t usbPendN = 0;
    uint32_t usbStallMs = 0;  // when USB stopped taking packets, 0 = it isn't stalled
    bool usbSkip = false;     // USB stalled once this playback, see sysExOutNewPlayback()
};
SysExOut g_sxOut;

// Thru channel messages waiting for a streamed SysEx to finish, per output.
ThruHold g_holdHw, g_holdUsb;

// DIN pacing: the UART's TX FIFO holds 32 bytes and drains one byte per
// 320 us at 31250 baud. g_dinCredit is how many bytes it can take without
// write() blocking, worked out from the time since the last write.
const int DIN_FIFO_BYTES = 32;
const uint32_t DIN_BYTE_US = 320;
// With USB as the only output, it gives up after this long without taking
// a packet (no host reading).
const uint32_t USB_STALL_MS = 100;
int g_dinCredit = DIN_FIFO_BYTES;
uint32_t g_dinStampUs = 0;

void updateDinCredit() {
    uint32_t now = micros();
    uint32_t add = (now - g_dinStampUs) / DIN_BYTE_US;
    if (g_dinCredit + (int64_t)add >= DIN_FIFO_BYTES) {
        g_dinCredit = DIN_FIFO_BYTES;
        g_dinStampUs = now;
    } else if (add) {
        g_dinCredit += (int)add;
        g_dinStampUs += add * DIN_BYTE_US;
    }
}

inline bool hwFree()  { return !(g_sxOut.busy && g_sxOut.hw); }
inline bool usbFree() { return !(g_sxOut.busy && g_sxOut.usb); }

// Messages in and out on DIN or USB, for the status strip LEDs. Core 0 only.
uint32_t g_inActivity = 0;
uint32_t g_outActivity = 0;

// hwEnabled() and usbEnabled() for a send, counting it for the LED.
// Off on an output a streamed SysEx is using, so nothing splits it.
inline bool hwOut()  { bool on = hwEnabled() && hwFree(); g_outActivity += on; return on; }
inline bool usbOut() { bool on = usbEnabled() && usbFree(); g_outActivity += on; return on; }
inline bool audioOutEnabled() { return g_audioOutput; }

// Which outgoing transport(s) a message arriving on `fromHw` should be
// echoed to under the current thru mode -- see MidiThruMode's doc comment
// in midi_output.h for what each mode means.
// `realtime` messages may go out in the middle of a streamed SysEx,
// anything else is dropped on the outputs it's using (channel thru
// passes true and holds them instead).
inline void thruDestinations(bool fromHw, bool& toHw, bool& toUsb, bool realtime = false) {
    switch (g_thruMode) {
        case MIDI_THRU_OFF:     toHw = false;    toUsb = false;   break;
        case MIDI_THRU_ON:      toHw = true;     toUsb = true;    break;
        case MIDI_THRU_TRS2USB: toHw = false;    toUsb = fromHw;  break;
        case MIDI_THRU_USB2TRS: toHw = !fromHw;  toUsb = false;   break;
        case MIDI_THRU_TRS2TRS: toHw = fromHw;   toUsb = false;   break;
        case MIDI_THRU_USB2USB: toHw = false;    toUsb = !fromHw; break;
    }
    if (!realtime) {
        toHw = toHw && hwFree();
        toUsb = toUsb && usbFree();
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

// Held while a streamed SysEx uses the output, and after it until the
// held ones have gone, so the order stays the same. See flushHeld().
void routeThruChannelVoice(bool fromHw, uint8_t status, uint8_t data1, uint8_t data2) {
    bool toHw, toUsb;
    thruDestinations(fromHw, toHw, toUsb, true);
    g_outActivity += toHw || toUsb;
    if (toHw) {
        if (!hwFree() || !g_holdHw.empty()) g_holdHw.push(status, data1, data2);
        else sendChannelVoiceTo(MIDIHW, status, data1, data2);
    }
    if (toUsb) {
        if (!usbFree() || !g_holdUsb.empty()) g_holdUsb.push(status, data1, data2);
        else sendChannelVoiceTo(MIDIUSB, status, data1, data2);
    }
}

// Sends held thru messages once their output is free, DIN paced like the
// SysEx so this never blocks, then the panic for any channel that overflowed.
void flushHeld() {
    if (hwFree()) {
        while (!g_holdHw.empty()) {
            updateDinCredit();
            if (g_dinCredit < 6) break;
            if (g_holdHw.count()) {
                const ThruHold::Msg& m = g_holdHw.front();
                sendChannelVoiceTo(MIDIHW, m.status, m.data1, m.data2);
                g_holdHw.pop();
                g_dinCredit -= 3;
            } else {
                uint8_t ch = (uint8_t)g_holdHw.takePanicChannel();
                sendChannelVoiceTo(MIDIHW, 0xB0 | ch, 123, 0); // All Notes Off
                sendChannelVoiceTo(MIDIHW, 0xB0 | ch, 64, 0);  // sustain off
                g_dinCredit -= 6;
            }
        }
    }
    if (usbFree()) {
        while (!g_holdUsb.empty()) {
            if (g_holdUsb.count()) {
                const ThruHold::Msg& m = g_holdUsb.front();
                sendChannelVoiceTo(MIDIUSB, m.status, m.data1, m.data2);
                g_holdUsb.pop();
            } else {
                uint8_t ch = (uint8_t)g_holdUsb.takePanicChannel();
                sendChannelVoiceTo(MIDIUSB, 0xB0 | ch, 123, 0);
                sendChannelVoiceTo(MIDIUSB, 0xB0 | ch, 64, 0);
            }
        }
    }
}

void routeThruSysEx(bool fromHw, const uint8_t* data, size_t len) {
    if (len == 1) {
        // A one-byte F7 escape can carry a real-time byte (e.g. F8).
        if (data[0] >= 0xF8) {
            if (hwEnabled()) {
                MIDIHW.sendRealTime((midi::MidiType)data[0]);
                g_dinCredit--; // a FIFO byte the SysEx pacing doesn't know about
            }
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

// Realtime bytes may sit inside a SysEx on DIN. On USB they're packets of
// their own, which TinyUSB's stream writer sends apart from the SysEx
// packets sysExOutWrite() makes.
void routeThruRealtime(bool fromHw, midi::MidiType type) {
    bool toHw, toUsb;
    thruDestinations(fromHw, toHw, toUsb, true);
    if (toHw) {
        MIDIHW.sendRealTime(type);
        g_dinCredit--;
    }
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
SysExStreamHandler g_sysExStreamHandler = nullptr;

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
template <typename Interface, typename Tap>
void pumpOne(Interface& iface, Tap& tap, SysExInParser& sysExIn, bool fromHw) {
    tap.tapped = -1;
    bool got = iface.read();
    if (tap.tapped >= 0) sysExIn.byte((uint8_t)tap.tapped, g_sysExStreamHandler);
    if (!got) return;
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
// The library reads one byte per read() (Use1ByteParsing).
template <typename Interface, typename Port, typename Tap>
void pumpIncoming(Interface& iface, Port& port, Tap& tap, SysExInParser& sysExIn, bool fromHw) {
    for (int n = 0; n < MAX_BYTES_PER_UPDATE && port.available() > 0; n++) pumpOne(iface, tap, sysExIn, fromHw);
    sysExIn.flush(g_sysExStreamHandler);
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
    pumpIncoming(MIDIHW, Serial1, serialMIDIHW, g_sysExInHw, true);
    pumpIncoming(MIDIUSB, usb_midi_transport, serialMIDIUSB, g_sysExInUsb, false);
    flushHeld();
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
            UiTap::midiNoteOn(channel, note, velocity);
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

void sysExOutBegin() {
    SysExOut& o = g_sxOut;
    o.busy = true;
    o.hw = hwEnabled();
    o.usb = usbEnabled() && TinyUSBDevice.mounted() && !o.usbSkip;
    o.ended = false;
    o.usbPendN = 0;
    o.usbStallMs = 0;
    g_outActivity += o.hw + o.usb;
}

// USB-MIDI SysEx packets: CIN 4 for three bytes that don't end the
// message, CIN 5/6/7 for one/two/three bytes ending with F7.
static bool usbSysExPacket(const uint8_t* bytes, uint8_t n, bool last) {
    uint8_t p[4] = {(uint8_t)(last ? 0x04 + n : 0x04), 0, 0, 0};
    for (uint8_t i = 0; i < n; i++) p[1 + i] = bytes[i];
    return usb_midi_transport.writePacket(p);
}

size_t sysExOutWrite(const uint8_t* data, size_t len) {
    SysExOut& o = g_sxOut;
    if (!o.busy) return 0;
    size_t n = len;
    if (o.hw) {
        updateDinCredit();
        n = g_dinCredit > 0 ? ((size_t)g_dinCredit < n ? (size_t)g_dinCredit : n) : 0;
    }
    if (o.usb) {
        size_t i = 0;
        for (; i < n; i++) {
            uint8_t b = data[i];
            bool last = b == 0xF7;
            if (o.usbPendN < 2 && !last) {
                o.usbPend[o.usbPendN++] = b;
                continue;
            }
            uint8_t pkt[3] = {o.usbPend[0], o.usbPend[1], 0};
            pkt[o.usbPendN] = b;
            if (!usbSysExPacket(pkt, o.usbPendN + 1, last)) break;
            o.usbPendN = 0;
        }
        if (i < n && o.hw) {
            // USB full while DIN is the slow one means no host is reading.
            // DIN never waits for it: USB is left out from here to the end
            // of this playback.
            o.usb = false;
            o.usbSkip = true;
        } else if (i == 0 && n > 0) {
            uint32_t now = millis();
            if (o.usbStallMs == 0) o.usbStallMs = now | 1;
            else if (now - o.usbStallMs >= USB_STALL_MS) {
                o.usb = false;
                o.usbSkip = true;
            }
            if (o.usb) return 0;
            i = n; // nowhere to send these
        } else {
            if (i > 0) o.usbStallMs = 0;
            n = i;
        }
    }
    if (o.hw && n) {
        Serial1.write(data, n);
        g_dinCredit -= (int)n;
    }
    if (n) o.ended = data[n - 1] == 0xF7;
    return n;
}

void sysExOutEnd() {
    SysExOut& o = g_sxOut;
    if (!o.busy) return;
    if (!o.ended) {
        const uint8_t f7 = 0xF7;
        if (o.hw) {
            updateDinCredit();
            Serial1.write(&f7, 1);
            g_dinCredit--;
        }
        if (o.usb) {
            uint8_t pkt[3] = {o.usbPend[0], o.usbPend[1], 0};
            pkt[o.usbPendN] = f7;
            usbSysExPacket(pkt, o.usbPendN + 1, true); // dropped if USB is full
        }
    }
    o.busy = false;
}

bool sysExOutDrained() {
    flushHeld();
    if (!g_holdHw.empty() || !g_holdUsb.empty()) return false;
    if (!hwEnabled()) return true;
    updateDinCredit();
    return g_dinCredit >= DIN_FIFO_BYTES;
}

void sysExOutNewPlayback() { g_sxOut.usbSkip = false; }

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
        UiTap::midiNoteOn(channel, note, velocity);
    } else {
        g_noteChannelMask[note] &= (uint16_t)~(1u << channel);
    }
}

uint32_t inActivity() { return g_inActivity; }
uint32_t outActivity() { return g_outActivity; }

void setInputHandler(InputHandler handler) { g_inputHandler = handler; }
void setSysExHandler(SysExHandler handler) { g_sysExHandler = handler; }
void setSysExStreamHandler(SysExStreamHandler handler) { g_sysExStreamHandler = handler; }
void setRealtimeHandler(RealtimeHandler handler) { g_realtimeHandler = handler; }

} // namespace MidiOutput
