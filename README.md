# midiTracker
##### MIDI player, recorder, and looper

midiTracker is an alternate firmware for the picoTracker v2 PCB.
You can order your own <a href=https://xiphonics.com/products/picotracker-pcb-kit>from xiphonics here</a>.

It turns the device into a MIDI/SysEx player, MIDI/SysEx recorder, and 4 track MIDI looper. 

### Player

midiTracker plays standard '.mid' MIDI files, type 0 or 1, out over TRS or USB MIDI. It will also play standard '.syx' SysEx files (eg patch backups from a synth). File are streamed from the SD card and MIDI files can be paused/resumed, scrubbed through forwards and backwards and played at an adjustable tempo. A piano-style display lights up with the played back notes to provide visual feedback while playing.

Additionally there is a simple onboard synth, supporting 24 melodic notes and 8 drum notes simultaneously, that you can use to listen to MIDI files without driving external gear. This is intended as a way to verify the contents of recordings and sounds very chiptune-y but it can be a lot of fun to play various MIDI files from the internet and enjoy the not-so-realistic sounds it makes.

### Recorder

midiTracker records both MIDI and SysEx straight to the SD card through a menu option that lets you pick whether you want to save MIDI data (will write to a type 0 .mid file) or SysEx dumps (will write to a standard .syx file).

MIDI recording has no set limit and will record from either the 3.5mm TRS port or a connected USB host. Note data, CCs, inline SysEx commands (eg for patch changes) are all recorded as they are received.

SysEx recording is similar and can be used to back up memory banks, individual patches, system configuration, etc.

### Looper

midiTracker has a 4 track MIDI looper. Each loop can be freeform in length or set to a certain number of bars. There is a hard limit of 4096 events per loop as loops are held in the Pico's limited RAM until you choose to save them. You can mute, overdub, pause/resume, and stop/restart each loop.

Loops can be saved as a group and loaded, again as a group. Each track can also be loaded with an individual MIDI file of your choice from the browser - not just prerecorded loops (you can loop your favourite Rick Astley song and play along).

The looper supports variable BPM (set internally or driven by MIDI) as well as a number of common time signatures. Loops can run synced to each other, tied to BPM/bars (if using set bar limits), or run independantly. MIDI transport can be enabled and the looper with respond to START/STOP/CONTINUE messages to help automate loop recording and playback.

A metronome, with optional count in, is included and features a visual indicator on the top of the screen to go along with the audio signal (the audio out is required for audio metronome, of course).

### Settings

Various settings can be setchanged and will be saved to the SD card to be loaded on boot. Currently the following items can be set:

BPM, time signature, clock source, loop length, sync mode, metronome, metronome volume, count in, count in bars, MIDI transport, MIDI thru

---

#### Controls

```
      UP    PLAY   EDIT
LEFT  DOWN  RIGHT  ENTER
      ALT   NAV
```

Every screen has a two line UI hint footer that explains what buttons (or combination) do what. The dpad buttons navigate through menus (generally right enters a menu and left backs out of it) and most modifications to settings are done while holding ALT or EDIT and changing the values with the direction arrows.

#### Hardware

- RP2040 (Pico), 240×320 ST7789 TFT over hardware SPI1
- SD card over native SDIO (not SPI) via SdFat's RP2040 SDIO driver.
- MIDI: hardware UART (TX/RX, 31250 baud) and USB-MIDI (composite USB
  device alongside a CDC debug console), both active simultaneously.
- I2S DAC for the onboard synth.
- 9 buttons, active-low with internal pull-ups, software-debounced.

See `include/pins.h` for the exact pin map.

#### Known limitations

- `MIDI_MAX_TRACKS` (24, in `config.h`) and `SdBrowser::MAX_DIR_ENTRIES`
  (96) are RAM-bounded caps — raise them if you hit real files/folders
  that exceed them.
- Clock Source's "External" mode tracks tempo only; it does not
  phase-lock loop playback pulse-for-pulse to the incoming clock.

### Installation

Mount your picoTracker in firmware update mode (through the menu if you are on the picoTracker firmware - or by holding the boot pin on the bottom and then connecting the USB cable) and copy the .uf2 file from the latest release to your SD card.

You can also pull the source and build it yourself - this was made mostly by Claude in platformio on VS Code.

---

While most of my intended functionality has been built in I need to do a lot more testing to make sure the UI and operation are rock solid and ready for use. Please submit issues you find here or find me on the picoTracker discord to tell me about them there.

Cheers,
rm