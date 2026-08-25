#pragma once

// The pariSynth 16-channel live-play grid's input handling and cursor
// state, shared by both places it's reachable: PariSynthMode's own play
// screen (main menu -> pariSynth) and FilePlayerMode's ALT+EDIT overlay
// during MIDI file playback (see file_player_mode.cpp's APP_SYNTH_EDIT).
// Extracted for the same reason SynthEditor was: two hosts need the exact
// same on-demand overlay behavior, and before this existed they'd each
// hand-written their own ~130-line copy of it that quietly drifted apart
// (missing manual EDIT+LEFT/RIGHT reassignment, wrong ENTER/PLAY targets,
// LEFT not jumping columns) until a user noticed the two screens didn't
// actually match. Not a TopMode -- a peer component both hosts include
// directly, same "modes don't include each other" reasoning looper_mode.h
// documents, and the exact same shape synth_editor.h already uses (this
// component itself calls into SynthEditor to open the field editor/bank
// menu/picker, the same way PariSynthMode used to call it directly).
//
// Deliberately does NOT own two things that must stay host-specific:
//   - MIDI input-handler registration. PariSynthMode registers its own
//     live-input handler (driving the synth directly from external MIDI);
//     FilePlayerMode leaves its own recorder-feed handler in place the
//     whole time a file plays. Swapping PariSynthMode's handler in here
//     would let live external Program Change messages overwrite the
//     instrument the *currently-playing file* has assigned to a channel --
//     audibly corrupting playback -- and would bypass MidiOutput's audio-
//     output-enabled gating entirely. Each host keeps its own handler.
//   - What "exiting the grid" means. update() returning RESULT_EXIT is
//     silent on purpose -- PariSynthMode calls MidiOutput::
//     allNotesOffAllChannels() afterward because it's leaving the live jam
//     engine entirely; FilePlayerMode's overlay does NOT, because the file
//     is still playing underneath and closing this overlay must not kill
//     its notes.
namespace PariSynthGrid {

void begin(); // one-time setup, call once from the top-level setup() -- no-op today, kept for symmetry with SynthEditor::begin()

// Resets the channel cursor to 0, clears the EDIT tap-vs-hold flag, and
// hides any volume overlay left over from a previous visit. Call once
// each time a host is about to start showing this grid.
void enter();

enum Result {
    RESULT_NONE,        // nothing to report, keep showing the grid
    RESULT_OPEN_EDITOR, // ENTER/PLAY/EDIT-tap opened SynthEditor -- host should delegate subsequent update()s to SynthEditor::update() until it returns true, then resume this grid
    RESULT_EXIT,         // NAV, or LEFT already on the left column -- host resumes its own previous screen; see this header's own comment on why no side effect happens here
};

// Per-tick: input handling for the grid (UP/DOWN cursor with hold-repeat,
// RIGHT/LEFT column jump, EDIT tap opens the field editor for the
// selected channel, EDIT+LEFT/RIGHT manually cycles its instrument family
// (accelerating hold, skipped on the percussion channel), EDIT+UP/DOWN
// adjusts `volume` (flat hold-repeat, 5%/step, 0-100) and shows the
// volume overlay, ENTER opens the Save/Load/Reset bank menu, PLAY opens
// the flat instrument picker). `volume` is the host's own 0-100 storage,
// passed by reference so this is the only code that ever writes it -- no
// second copy to drift. Also expires the volume overlay if its fade timer
// has elapsed.
Result update(int& volume);

int selectedChannel(); // 0-15, current cursor position

// True while the volume overlay (see Ui::updatePariSynthVolumeOverlay())
// is showing. A host must call Ui::updatePariSynthVolumeOverlay(volume)
// again immediately after any full Ui::drawPariSynthPlay() redraw when
// this is true, since that redraw repaints every cell including the one
// the overlay sits on top of.
bool volumeOverlayVisible();

} // namespace PariSynthGrid
