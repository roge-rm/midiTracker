#pragma once

// USB Drive mode: shows the SD card to the computer as a USB mass storage
// drive, next to the serial and MIDI interfaces. The MSC interface is
// always in the USB descriptor and reports "no medium" outside this mode,
// so entering or leaving never re-enumerates and MIDI stays connected.
//
// Entered from Settings, which is only reachable from mode select, where
// every other mode has already stopped its players and recorders and
// closed its files. While the mode is on, nothing but this mode touches
// the card.
namespace UsbDriveMode {

// Adds the MSC interface. Call in setup() before the USB re-enumeration.
void begin();

void enter();

// For setup() when the card initialised but its volume didn't mount:
// offers the drive so the card can be fixed from the computer.
void enterMountFailed();

// Returns true once the card is mounted again and the mode should hand
// back to whoever entered it.
bool update();

} // namespace UsbDriveMode
