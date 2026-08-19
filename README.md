The follwoing was written by Claude (also almost all of the code, so it goes), I will rewrite and fix it soon as it's only a pretty good generalization of what's going on here and I am uploading this late.

Cheers. 

# midiTracker

Alternate firmware for the picotracker v2 (RP2040/Pico, 240×320 ST7789
TFT, PlatformIO + Arduino/earlephilhower core). It turns the device into
a standalone MIDI workstation: an SD-card file player/recorder, a
4-track live looper, an onboard synth for monitoring, and MIDI
routing/clock/transport utilities — all driven by 9 buttons and the TFT.

MIDI moves over two transports simultaneously: hardware DIN/TRS (UART,
31250 baud) and USB-MIDI, both always available regardless of mode.

## What it does

At boot you land on a **mode-select** screen (UP/DOWN, ENTER/PLAY/RIGHT
to choose) with three destinations:

### Play / Record Files

Browses `.mid` and `.syx` files on the SD card and:

- **Plays back** Standard MIDI Files (format 0/1, tempo-map changes,
  running status, up to 24 tracks) straight off the card — no
  whole-file RAM buffering. Pause/resume, restart when finished,
  tempo-scale ±%, volume, and a live note-activity display.
- **Records** incoming MIDI (from either transport) straight to a new
  `.mid` file as it arrives, with live elapsed time and event count.
- **Captures raw SysEx dumps** to `.syx` files (e.g. patch backups from a
  synth), and sends a captured dump back out on demand.
- Lets you rename/delete files and folders, create folders, and hand a
  `.mid` file off to the Looper (**Load to Looper**, picks a destination
  track).
- MIDI output target (Hardware / USB / Both) and onboard-synth audio
  monitoring are both adjustable from this screen and apply globally.

### MIDI Looper

A live 4-track looper, each track independently:

- Armed and recorded from incoming MIDI (filtered to one channel, or
  any channel via OMNI), with mute, overdub, pause/resume, and
  stop/restart.
- Either **freeform** (records until you stop it) or **bar-quantized**
  to one of several preset lengths against the shared BPM/time
  signature.
- Saved to and loaded from the SD card as its own small session folder.

Shared session settings, all live-adjustable from the BPM row: **BPM**
(20–300), **time signature** (a handful of common presets), **Sync**
(all tracks locked to one shared clock) vs **Independent** (each track
its own clock), a **metronome** (adjustable volume, its own wood-block
synth voice, louder downbeat) with an optional **count-in**, and a
header strip that visually blinks out the current beat.

A loop menu (ENTER) covers Save, Load Saved Loop, Erase Track, Erase All
Tracks, and Delete Saved Loop.

The Looper can also be driven externally: **Clock Source** can follow
incoming MIDI Clock instead of the manually-set BPM, and **MIDI
Transport** can let incoming Start/Stop/Continue start, freeze, and
resume the tracks (see Settings, and MIDI Thru/Clock/Transport below).

### Settings

A scrollable list of device-wide defaults and behaviors, adjusted with
EDIT+LEFT/RIGHT and persisted to `/settings.txt`:

| Setting | Default | Controls |
|---|---|---|
| Default BPM | 120 | Looper's starting tempo |
| Time Sig | 4/4 | Looper's starting time signature |
| Clock Source | Internal | Internal (manual BPM) vs External (follow incoming MIDI clock) |
| Loop Length | Free | Looper's starting per-track bar length |
| Sync Mode | Sync | Looper's starting Sync/Independent mode |
| Metronome | Off | Metronome starting on/off |
| Metro Volume | 80% | Metronome click volume |
| Count In | On | Count-in starting on/off |
| Count In Bars | 1 | Count-in length |
| MIDI Transport | Off | React to incoming MIDI Start/Stop/Continue |
| MIDI Thru | Off | Thru routing mode (see below) |

Settings a Looper session has already picked up aren't yanked out from
under it — a change here takes effect the next time you (re)enter the
Looper with no track content yet, except MIDI Thru, which applies
immediately since it isn't a Looper-specific concept.

## MIDI Thru, Clock, and Transport

- **MIDI Thru**: `Off` / `On` (mirror every input to every output,
  including a transport back to itself — the same self-echo a
  dedicated hardware Thru port performs) / `TRS>USB` / `USB>TRS` /
  `TRS>TRS` / `USB>USB` (each of the four directional modes enables
  exactly one input→output pairing). Covers channel-voice messages,
  SysEx, and realtime bytes (Clock/Start/Stop/Continue/Active
  Sensing/System Reset) — all raw-forwarded, bypassing the onboard
  synth and note-activity display entirely.
- **Clock Source**: `Internal` uses the manually-set BPM as usual;
  `External` derives a smoothed tempo estimate from incoming MIDI Clock
  (24 pulses per quarter note) and drives the Looper's BPM live. This is
  tempo-follow, not sample-accurate phase lock — loop position still
  runs on the device's own timer at the currently-tracked tempo.
- **MIDI Transport**: when on, incoming Start/Stop/Continue drive the
  Looper directly — Start jumps every track with content to the
  beginning and plays it, Stop freezes everything in place, Continue
  resumes every paused track immediately.

## Onboard synth

A polyphonic synth (24 melodic + 8 drum voices) rendered on the RP2040's
second core to the I2S DAC, purely so file/looper content is audible
without external gear — not intended to sound realistic, just to make
different parts distinguishable. One algorithmic preset per GM
instrument family (16 families) on the melodic side, and a small set of
archetypal drum voices (kick, snare, closed/open hat, tom, cymbal, wood
block) on channel 10. Audio monitoring is opt-in and purely additive: it
never affects what goes out over hardware/USB MIDI.

## Controls

9 momentary buttons, physical layout:

```
      UP    PLAY  EDIT
LEFT  DOWN  RIGHT  ENTER
      ALT   NAV
```

Every screen shows a two-line button hint in the footer for whatever's
currently relevant. The general conventions held throughout the
firmware: **ALT** and **EDIT** are modifiers held alongside another
button to reach a secondary function (cycling a value, editing a
number, etc.), **ENTER** opens a menu, and **NAV**/**LEFT** back out or
stop.

## Hardware

- RP2040 (Pico), 240×320 ST7789 TFT over hardware SPI1, up to 40MHz.
- SD card over native SDIO (not SPI) via SdFat's RP2040 SDIO driver.
- MIDI: hardware UART (TX/RX, 31250 baud) and USB-MIDI (composite USB
  device alongside a CDC debug console), both active simultaneously.
- I2S DAC for the onboard synth.
- 9 buttons, active-low with internal pull-ups, software-debounced.

See `include/pins.h` for the exact pin map.

## Known limitations

- `MIDI_MAX_TRACKS` (24, in `config.h`) and `SdBrowser::MAX_DIR_ENTRIES`
  (96) are RAM-bounded caps — raise them if you hit real files/folders
  that exceed them.
- Clock Source's "External" mode tracks tempo only; it does not
  phase-lock loop playback pulse-for-pulse to the incoming clock.

## Building

```
pio run -e pico -t upload
pio device monitor
```
