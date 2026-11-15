#include "usb_drive_mode.h"
#include <Adafruit_TinyUSB.h>
#include <hardware/sync.h>
#include <string.h>
#include "sd_card.h"
#include "input.h"
#include "ui.h"
#include "midi_output.h"
#include "file_player_mode.h"

namespace {

Adafruit_USBD_MSC usbMsc;

const uint32_t SECTOR_SIZE = 512;

// The Pico's VBUS sense input, high while USB power is present. The RP2040
// sees no disconnect when the cable is pulled, only a suspend, so this is
// how an unplug shows up.

// How long USB has to stay gone before it counts as unplugged. Long enough
// to ride out a host resetting and re-enumerating the device.
const uint32_t UNPLUG_MS = 1000;

// Longest update() spends serving the host's reads and writes in one go.
const uint32_t IO_BUDGET_US = 10000;
const uint32_t IO_IDLE_US = 1000;

const uint32_t NO_USB_MS = 1500;

// The MSC callbacks below run inside tud_task(), from the USB task IRQ on
// core 0 or from TinyUSB_Device_Task() in the main loop. They never touch
// the card: a read or write is stored here and done by update(), which
// then reports it with tud_msc_async_io_done(). So all card access stays
// in the main loop, and the IRQ never blocks on the card.
volatile bool g_offering = false; // drive ready and taking reads/writes
volatile bool g_ejected = false;  // host sent an eject
volatile bool g_syncWanted = false;
// Set when the drive is offered; the next TEST UNIT READY fails once and
// the sense that follows says UNIT ATTENTION 28h (medium may have changed).
volatile bool g_mediumChanged = false;
volatile bool g_attentionSense = false;

enum : uint8_t { IO_NONE, IO_READ, IO_WRITE };
volatile uint8_t g_ioKind = IO_NONE;
volatile uint32_t g_ioLba = 0;
uint8_t* volatile g_ioBuf = nullptr;
volatile uint32_t g_ioSize = 0;
// Bumped by every new request. A USB reset can drop the request update()
// is working on and start another, so a result is only reported, and the
// stack's buffer only touched, if the sequence still matches.
volatile uint32_t g_ioSeq = 0;

// The card reads into and writes from this, never the stack's buffer.
uint8_t sectorBuf[SECTOR_SIZE] __attribute__((aligned(4)));

enum State { STATE_NO_USB, STATE_OFFERING, STATE_WAIT_USB };
State state = STATE_NO_USB;
bool mountFailed = false;
uint32_t stateSinceMs = 0;
uint32_t usbGoneSinceMs = 0;
bool usbGone = false;

int32_t queueIo(uint8_t kind, uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
    if (!g_offering) return -1;
    g_ioLba = lba;
    g_ioBuf = buffer;
    g_ioSize = bufsize;
    g_ioSeq = g_ioSeq + 1;
    g_ioKind = kind;
    return TUD_MSC_RET_ASYNC;
}

int32_t onRead(uint32_t lba, void* buffer, uint32_t bufsize) {
    return queueIo(IO_READ, lba, (uint8_t*)buffer, bufsize);
}

int32_t onWrite(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
    return queueIo(IO_WRITE, lba, buffer, bufsize);
}

void onFlush() { g_syncWanted = true; }

bool onReady() {
    if (!g_offering) return false;
    if (g_mediumChanged) {
        g_mediumChanged = false;
        g_attentionSense = true;
        return false;
    }
    g_attentionSense = false;
    return true;
}

bool onStartStop(uint8_t powerCondition, bool start, bool loadEject) {
    (void)powerCondition;
    if (loadEject && !start) {
        g_offering = false;
        g_ejected = true;
    }
    return true;
}

// Reports the request with sequence `seq` as done, unless a newer one has
// replaced it. For a read, copies the sector into the stack's buffer first.
void finishIo(uint32_t seq, bool ok, bool copyOut) {
    uint32_t irq = save_and_disable_interrupts();
    if (g_ioKind != IO_NONE && g_ioSeq == seq) {
        uint32_t size = g_ioSize;
        if (ok && copyOut) memcpy(g_ioBuf, sectorBuf, size);
        g_ioKind = IO_NONE;
        tud_msc_async_io_done(ok ? (int32_t)size : -1, false);
    }
    restore_interrupts(irq);
    TinyUSB_Device_Task();
}

// Does the host's pending read or write, if any. Returns true if there was one.
bool serviceIo() {
    uint32_t irq = save_and_disable_interrupts();
    uint8_t kind = g_ioKind;
    uint32_t seq = g_ioSeq;
    uint32_t lba = g_ioLba;
    uint32_t size = g_ioSize;
    bool ok = g_offering && size == SECTOR_SIZE;
    if (kind == IO_WRITE && ok) memcpy(sectorBuf, g_ioBuf, SECTOR_SIZE);
    restore_interrupts(irq);
    if (kind == IO_NONE) return false;

    if (ok) {
        ok = (kind == IO_READ) ? sd.card()->readSectors(lba, sectorBuf, 1)
                               : sd.card()->writeSectors(lba, sectorBuf, 1);
    }
    finishIo(seq, ok, kind == IO_READ);
    return true;
}

// Mounted and not suspended. An unplugged cable shows as a suspend, since
// the RP2040 forces VBUS detect on and not every board wires GPIO24 to VBUS.
bool usbPresent() {
    return TinyUSBDevice.mounted() && !TinyUSBDevice.suspended();
}

void drawScreen() {
    if (state == STATE_OFFERING) {
        if (mountFailed) Ui::drawMessage("Can't read the SD card", "Fix it over USB, then eject");
        else Ui::drawMessage("SD card in use by the computer", "Eject it, or LEFT to leave");
    } else if (state == STATE_WAIT_USB) {
        Ui::drawMessage("Can't read the SD card", "Connect USB to fix it");
    } else {
        Ui::drawMessage("No USB connection");
    }
}

void setState(State s) {
    state = s;
    stateSinceMs = millis();
    drawScreen();
}

void startOffering() {
    usbMsc.setCapacity((uint32_t)sd.card()->sectorCount(), SECTOR_SIZE);
    g_ejected = false;
    g_syncWanted = false;
    g_attentionSense = false;
    g_mediumChanged = true;
    usbGone = false;
    g_offering = true;
    setState(STATE_OFFERING);
}

// Stops the drive, fails any request still waiting and syncs the card.
void stopOffering() {
    g_offering = false;
    finishIo(g_ioSeq, false, false);
    usbMsc.setCapacity(0, SECTOR_SIZE);
    sd.card()->syncDevice();
    g_syncWanted = false;
}

// Re-mounts the volume, since the computer may have changed the FAT.
// Returns true when the mode can hand back. If the card doesn't mount, the
// drive is offered again so it can be fixed from the computer.
bool tryMount() {
    if (sd.volumeBegin()) {
        mountFailed = false;
        FilePlayerMode::resetBrowser();
        return true;
    }
    mountFailed = true;
    if (usbPresent()) startOffering();
    else setState(STATE_WAIT_USB);
    return false;
}

void clearMidiHandlers() {
    // No handler at all means nothing from MIDI input can reach the card.
    MidiOutput::setInputHandler(nullptr);
    MidiOutput::setSysExHandler(nullptr);
    MidiOutput::setRealtimeHandler(nullptr);
}

} // namespace

// Rewrites the sense after the TEST UNIT READY that onReady() failed for a
// medium change. Runs inside tud_task().
extern "C" int32_t tud_msc_request_sense_cb(uint8_t lun, void* buffer, uint16_t bufsize) {
    (void)lun;
    (void)bufsize;
    if (g_attentionSense) {
        g_attentionSense = false;
        scsi_sense_fixed_resp_t* sense = (scsi_sense_fixed_resp_t*)buffer;
        sense->sense_key = SCSI_SENSE_UNIT_ATTENTION;
        sense->add_sense_code = 0x28;
        sense->add_sense_qualifier = 0x00;
    }
    return sizeof(scsi_sense_fixed_resp_t);
}

namespace UsbDriveMode {

void begin() {
    usbMsc.setID("midiTrkr", "SD card", "1.0");
    usbMsc.setReadWriteCallback(onRead, onWrite, onFlush);
    usbMsc.setReadyCallback(onReady);
    usbMsc.setStartStopCallback(onStartStop);
    usbMsc.setCapacity(0, SECTOR_SIZE);
    usbMsc.begin();
}

void enter() {
    clearMidiHandlers();
    mountFailed = false;
    if (usbPresent()) startOffering();
    else setState(STATE_NO_USB);
}

void enterMountFailed() {
    clearMidiHandlers();
    mountFailed = true;
    if (usbPresent()) startOffering();
    else setState(STATE_WAIT_USB);
}

bool update() {
    switch (state) {
        case STATE_NO_USB:
            return Input::justPressed(BTN_LEFT) || millis() - stateSinceMs >= NO_USB_MS;

        case STATE_WAIT_USB:
            if (Input::justPressed(BTN_LEFT)) return tryMount();
            if (usbPresent()) startOffering();
            return false;

        case STATE_OFFERING: {
            // The next sector of a transfer arrives a fraction of a ms
            // after the last one is done, so keep waiting for it a little.
            if (serviceIo()) {
                uint32_t start = micros();
                uint32_t last = start;
                while (micros() - start < IO_BUDGET_US) {
                    if (serviceIo()) last = micros();
                    else if (micros() - last >= IO_IDLE_US) break;
                }
            }
            if (g_syncWanted) {
                g_syncWanted = false;
                sd.card()->syncDevice();
            }

            if (!usbPresent()) {
                if (!usbGone) {
                    usbGone = true;
                    usbGoneSinceMs = millis();
                }
            } else {
                usbGone = false;
            }
            bool unplugged = usbGone && millis() - usbGoneSinceMs >= UNPLUG_MS;

            if (g_ejected || unplugged || Input::justPressed(BTN_LEFT)) {
                stopOffering();
                return tryMount();
            }
            return false;
        }
    }
    return false;
}

} // namespace UsbDriveMode
