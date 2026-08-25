# midiTracker
##### MIDI/tracker player, recorder, and looper

midiTracker is an alternate firmware for the picoTracker v2 PCB.
You can order your own <a href=https://xiphonics.com/products/picotracker-pcb-kit>from xiphonics here</a>.

It turns the device into a MIDI/SysEx/tracker-file player, MIDI/SysEx recorder, and 4 track MIDI looper.

### Player

midiTracker plays standard '.mid' MIDI files, type 0 or 1, out over TRS or USB MIDI. It will also play standard '.syx' SysEx files (eg patch backups from a synth). File are streamed from the SD card and MIDI files can be paused/resumed, scrubbed through forwards and backwards and played at an adjustable tempo. A piano-style display lights up with the played back notes to provide visual feedback while playing.

It also plays .wav files, as well as a few tracker modules - .mod, .s3m, and .xm. These are experimental and not 100% tested, there may be slowdowns or audio glitches. Attempts have been made to make these sound as authentic as possible but of course there are hardware limitations that make this an interesting challenge.

Additionally there is a simple onboard synth, supporting 24 melodic notes and 8 drum notes simultaneously, that you can use to listen to MIDI files without driving external gear. This is intended as a way to verify the contents of recordings - and sounds very chiptuney - but it can be a lot of fun to play various MIDI files from the internet and enjoy the not-so-realistic sounds it makes. The synth's own voices can be run through a lo-fi/chiptune-flavored reverb, with a choice of three algorithms (Lo-fi, Lush, Shimmer) and adjustable wet/dry mix. Output level can also be matched to how you have the device hooked up (Headphone Low, Headphone High, or Line Level).

### Recorder

midiTracker records both MIDI and SysEx straight to the SD card through a menu option that lets you pick whether you want to save MIDI data (will write to a type 0 .mid file) or SysEx dumps (will write to a standard .syx file).

MIDI recording has no set limit and will record from either the 3.5mm TRS port or a connected USB host. Note data, CCs, inline SysEx commands (eg for patch changes) are all recorded as they are received.

SysEx recording is similar and can be used to back up memory banks, individual patches, system configuration, etc.

### Looper

midiTracker has a 4 track MIDI looper. Each loop can be freeform in length or set to a certain number of bars. There is a hard limit of 4096 events per loop as loops are held in the Pico's limited RAM until you choose to save them. You can mute, overdub, pause/resume, and stop/restart each loop.

Each track has independent input and output MIDI channels. The input channel filters which incoming channel gets recorded while the output channel controls playback - either remapped to a fixed channel or sent back out on whichever channel each event was originally recorded on.

Loops can be saved and loaded individually, as standalone .mid files, or as a full session bundle covering all 4 tracks at once. Each track can also be loaded with an individual MIDI file of your choice from the browser - not just prerecorded loops (you can loop your favourite Rick Astley MIDI and play along).

The looper supports variable BPM (set internally or driven by MIDI) as well as a number of common time signatures. Loops can run synced to each other, tied to BPM/bars (if using set bar limits), or run independantly. MIDI transport can be enabled and the looper will respond to START/STOP/CONTINUE messages to help automate loop recording and playback. It also includes a metronome, with both audio and visual cues, with configurable count in.

### pariSynth

The internal synth, named pariSynth, can be played over TRS or USB MIDI, multitimbral over 16 channels. Channels respond to PC messages to assign instruments or they can be changed manually in the pariSynth overview. You are able to edit the changeable parameters on each synth (waveform, ADSR, cutoff, vibrato/tremolo/PWM) and save your instruments as preset packs on the SD card. This will also affect how MIDI played through the internal synth sounds so you can customize your cheesy chiptune MIDI as much as you like!

### Themes

The UI's color scheme is fully customizable through a dedicated theme editor with 19 editable colour roles - UI chrome, looper track states, beat/rhythm accents, and the note-activity display's velocity tiers. Themes can be saved to the SD card (and loaded) or reset back to the default. The selected theme is remembered and reloaded automatically on boot.

### Settings

Various settings can be modified and will be saved to the SD card to be loaded on boot. Currently the following items can be set:

BPM, time signature, clock source, loop length, sync mode, metronome, metronome volume, count in, count in bars, MIDI transport, MIDI thru, default volume, output level, reverb on/off, reverb mix, reverb type, onboard synth audio on/off, display brightness, theme

---

#### Controls

```
      UP    PLAY   EDIT
LEFT  DOWN  RIGHT  ENTER
      ALT   NAV
```

Every screen has a two line UI hint footer that explains what buttons (or combination) do what. The dpad buttons navigate through menus (generally right enters a menu and left backs out of it) and most modifications to settings are done while holding ALT or EDIT and changing the values with the direction arrows. Menu cursors wrap top-to-bottom and back rather than stopping at either end.

#### Hardware

- RP2040 (Pico), 240×320 ST7789 TFT over hardware SPI1
- SD card over native SDIO (not SPI) via SdFat's RP2040 SDIO driver.
- MIDI: hardware UART (TX/RX, 31250 baud) and USB-MIDI (composite USB
  device alongside a CDC debug console), both active simultaneously.
- I2S DAC for the onboard synth.
- 9 buttons, active-low with internal pull-ups, software-debounced.
- Battery voltage sense pin, read out as a small gauge in the bottom-right
  corner of every screen (charge level and charging state).

See `include/pins.h` for the exact pin map.

#### Known limitations

- `MIDI_MAX_TRACKS` (64, in `config.h`) is a RAM-bounded cap — raise it if
  you hit a real file that exceeds it.
- The file browser (`SdBrowser`) indexes a folder's contents on demand
  rather than eagerly loading everything, so it scales to a couple
  thousand files per folder rather than the old fixed 96-entry cap.
  `SdBrowser::MAX_INDEX_ENTRIES` (4096) is a safety ceiling past which a
  folder stops indexing further entries; real available RAM (checked at
  scan time, since e.g. the looper's `tracks[]` can already be using
  128KB) can cap it lower still. Either way, a folder past what got
  indexed shows a "(+N)"/"(4096+)" note in the header rather than silently
  dropping entries. Folders past `SdBrowser::SORT_KEY_CEILING` (512
  entries) are listed in on-disk order instead of alphabetized, to avoid
  re-opening thousands of files just to compare their names.
- Clock Source's "External" mode tracks tempo only; it does not
  phase-lock loop playback pulse-for-pulse to the incoming clock.
- Very rarely, a busy tracker-file passage can trigger a brief (a few
  seconds, self-recovering) SD card read stall, heard as a momentary
  slowdown/crackle. Believed to be intrinsic SD card behavior rather
  than a firmware bug.

### Installation

Mount your picoTracker in firmware update mode - via Settings > MIDI/System > USB Bootloader if you're already running midiTracker, through the menu if you are on the picoTracker firmware, or by holding the boot pin on the bottom and then connecting the USB cable - and copy the .uf2 file from the latest release to your mounted device.

You can also pull the source and build it yourself - this was made mostly by Claude in platformio on VS Code.

---

While most of my intended functionality has been built in I need to do a lot more testing to make sure the UI and operation are rock solid and ready for use. Please submit issues you find here or find me on the picoTracker discord to tell me about them there.

Cheers,
rm
