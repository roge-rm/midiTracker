# Host render harness for the tracker players.
#   make -C test/host            build build/host/render and gen
#   make -C test/host baseline   render every test file into build/host/baseline/
#   make -C test/host check      render into build/host/current/ and compare with baseline
#   make -C test/host midi           build build/host/render_midi (real synth.cpp and midi_file.cpp)
#   make -C test/host midi-baseline  render midi-files.list into build/host/midi-baseline/ (wav + stats)
#   make -C test/host midi-check     render into build/host/midi-current/ and compare with the baseline
#   fluid_compare.sh                 compare the midi renders with fluidsynth's
#   openmpt_compare.sh <list> <outdir> [secs]   correlate .it renders with openmpt123 (uses build/host/corr)
#   make -C test/host look OUT_DIR=dir   render the M12 screens to PNGs in dir (default build/host/look)
#   make -C test/host players OUT_DIR=dir   the player screens, drawn from host renders of real songs
#   make -C test/host wav            build build/host/render_wav (WavPlayer over the host ring)
#   make -C test/host mp3            build build/host/render_mp3 (Mp3Player and lib/helix_mp3 over the host ring)
#                                    and render_mp3_ops (counts the decoder's long multiplies)
#   make -C test/host rec-check      MidiRecorder, SysExRecorder and SysExPlayer on an in-memory card
#   make -C test/host battery-check  the battery gauge on made-up voltage sequences
#   make -C test/host convert-check CONVERT_DIR=dir   Converter on MP3s and WAVs made with ffmpeg in dir,
#                                    against ffmpeg's own conversions (see convert_check.sh)
ROOT   := $(abspath ../..)
OUT    := $(ROOT)/build/host
CXX    ?= g++
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
            -Wno-unused-function -Wno-missing-field-initializers \
            -I$(CURDIR)/stub -I$(ROOT)/include
PLAYER_SRC := $(wildcard $(ROOT)/src/tracker_base.cpp) $(ROOT)/src/ui_tap.cpp $(ROOT)/src/mod_file.cpp \
              $(ROOT)/src/s3m_file.cpp $(ROOT)/src/xm_file.cpp $(ROOT)/src/it_file.cpp $(ROOT)/src/shared_ram_arena.cpp
HARNESS_SRC := $(CURDIR)/render.cpp $(CURDIR)/host_stubs.cpp
SECONDS ?= 60

.PHONY: all baseline check clean midi midi-baseline midi-check look players wav mp3 rec-check battery-check convert-check
all: $(OUT)/render $(OUT)/gen $(OUT)/corr

$(OUT)/corr: $(CURDIR)/corr.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -Wall -o $@ $<

$(OUT)/render: $(PLAYER_SRC) $(HARNESS_SRC) $(CURDIR)/host_snap.h $(wildcard $(CURDIR)/stub/*.h) $(wildcard $(ROOT)/include/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(PLAYER_SRC) $(HARNESS_SRC)

$(OUT)/gen: $(CURDIR)/gen.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -Wall -o $@ $<

$(OUT)/files.txt: $(OUT)/gen $(CURDIR)/files.list $(CURDIR)/files-it.list
	@mkdir -p $(OUT)/gen-files
	$(OUT)/gen $(OUT)/gen-files
	@{ grep -v '^#' $(CURDIR)/files.list | sed "s|^trackertunes/|$(ROOT)/trackertunes/|"; ls $(OUT)/gen-files/*; \
	   grep -v '^#' $(CURDIR)/files-it.list | sed "s|^trackertunes/|$(ROOT)/trackertunes/|"; } > $@

baseline: all $(OUT)/files.txt
	SECONDS=$(SECONDS) $(CURDIR)/render_all.sh $(OUT)/render $(OUT)/files.txt $(OUT)/baseline

check: all $(OUT)/files.txt
	SECONDS=$(SECONDS) $(CURDIR)/render_all.sh $(OUT)/render $(OUT)/files.txt $(OUT)/current
	$(CURDIR)/compare.sh $(OUT)/baseline $(OUT)/current

clean:
	rm -rf $(OUT)

# WAV target: the real wav_file.cpp over the host ring and FsFile.
WAV_SRC := $(ROOT)/src/wav_file.cpp $(ROOT)/src/shared_ram_arena.cpp $(CURDIR)/host_stubs.cpp $(CURDIR)/render_wav.cpp
wav: $(OUT)/render_wav

$(OUT)/render_wav: $(WAV_SRC) $(wildcard $(CURDIR)/stub/*.h) $(ROOT)/include/wav_file.h $(ROOT)/include/shared_ram_arena.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(WAV_SRC)

# MP3 target: the real mp3_file.cpp and Helix decoder over the host ring and FsFile.
HELIX := $(ROOT)/lib/helix_mp3/src
MP3_SRC := $(ROOT)/src/mp3_file.cpp $(ROOT)/src/shared_ram_arena.cpp $(CURDIR)/host_stubs.cpp $(CURDIR)/render_mp3.cpp
MP3_C := $(wildcard $(HELIX)/*.c)
MP3_DEPS := $(MP3_SRC) $(MP3_C) $(wildcard $(HELIX)/*.h) $(wildcard $(CURDIR)/stub/*.h) $(ROOT)/include/mp3_file.h $(ROOT)/include/shared_ram_arena.h
mp3: $(OUT)/render_mp3 $(OUT)/render_mp3_ops $(OUT)/corr

# render_mp3 runs the polyphase filter the device's way (MP3_POLY_SPLIT, in C).
# MP3_EXTRA=-DMP3_MULHS_C also uses the M0+ multiply on the host.
$(OUT)/render_mp3: $(MP3_DEPS)
	@mkdir -p $(OUT)/mp3obj
	cd $(OUT)/mp3obj && gcc -O2 -g -w -DMP3_POLY_SPLIT $(MP3_EXTRA) -c $(MP3_C)
	$(CXX) $(CXXFLAGS) -I$(HELIX) -o $@ $(MP3_SRC) $(OUT)/mp3obj/*.o

$(OUT)/render_mp3_ops: $(MP3_DEPS)
	@mkdir -p $(OUT)/mp3obj-ops
	cd $(OUT)/mp3obj-ops && gcc -O2 -g -w -DMP3_COUNT_OPS -c $(MP3_C)
	$(CXX) $(CXXFLAGS) -DMP3_COUNT_OPS -I$(HELIX) -o $@ $(MP3_SRC) $(OUT)/mp3obj-ops/*.o

# Convert check: the real convert.cpp, wav_file.cpp and mp3_file.cpp over host files.
CONVERT_SRC := $(ROOT)/src/convert.cpp $(ROOT)/src/wav_file.cpp $(ROOT)/src/mp3_file.cpp $(ROOT)/src/shared_ram_arena.cpp \
               $(CURDIR)/host_stubs.cpp $(CURDIR)/convert_check.cpp
CONVERT_DIR ?= $(OUT)/convert

$(OUT)/convert_check: $(CONVERT_SRC) $(MP3_C) $(wildcard $(HELIX)/*.h) $(wildcard $(CURDIR)/stub/*.h) $(ROOT)/include/convert.h \
                      $(ROOT)/include/wav_file.h $(ROOT)/include/mp3_file.h $(ROOT)/include/shared_ram_arena.h
	@mkdir -p $(OUT)/mp3obj-convert
	cd $(OUT)/mp3obj-convert && gcc -O2 -g -w -DMP3_POLY_SPLIT -c $(MP3_C)
	$(CXX) $(CXXFLAGS) -I$(HELIX) -o $@ $(CONVERT_SRC) $(OUT)/mp3obj-convert/*.o

convert-check: $(OUT)/convert_check
	$(CURDIR)/convert_check.sh $(OUT)/convert_check $(CONVERT_DIR) $(MP3_DIR)

# Recorder check: the real recorders over stub-rec/'s in-memory card.
REC_SRC := $(ROOT)/src/midi_recorder.cpp $(ROOT)/src/sysex_recorder.cpp $(ROOT)/src/sysex_player.cpp $(CURDIR)/rec_check.cpp

$(OUT)/rec_check: $(REC_SRC) $(wildcard $(CURDIR)/stub-rec/*.h $(CURDIR)/stub-rec/hardware/*.h) $(ROOT)/include/midi_recorder.h $(ROOT)/include/sysex_recorder.h $(ROOT)/include/smf.h $(ROOT)/include/sysex_player.h $(ROOT)/include/sysex_stream.h $(ROOT)/include/midi_output.h $(ROOT)/include/thru_hold.h
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I$(CURDIR)/stub-rec -I$(ROOT)/include -o $@ $(REC_SRC)

rec-check: $(OUT)/rec_check
	$(OUT)/rec_check

# Battery gauge check: battery_gauge.cpp on made-up voltage sequences.
$(OUT)/battery_check: $(ROOT)/src/battery_gauge.cpp $(CURDIR)/battery_check.cpp $(ROOT)/include/battery_gauge.h
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O1 -g -Wall -Wextra -I$(ROOT)/include -o $@ $(ROOT)/src/battery_gauge.cpp $(CURDIR)/battery_check.cpp

battery-check: $(OUT)/battery_check
	$(OUT)/battery_check

# MIDI target: the real synth.cpp and midi_file.cpp with a stub MidiOutput and I2S.
MIDI_SRC := $(ROOT)/src/synth.cpp $(ROOT)/src/midi_file.cpp $(ROOT)/src/ui_tap.cpp $(CURDIR)/midi_stubs.cpp $(CURDIR)/render_midi.cpp $(CURDIR)/host_stubs.cpp
MIDI_FLAGS := $(filter-out -I%,$(CXXFLAGS)) -DHOST_MIDI -I$(CURDIR)/stub-midi -I$(CURDIR)/stub -I$(ROOT)/include
MIDI_DIR := $(OUT)/midi

midi: $(OUT)/render_midi $(OUT)/corr $(OUT)/envcorr

$(OUT)/envcorr: $(CURDIR)/envcorr.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -Wall -o $@ $<

$(OUT)/render_midi: $(MIDI_SRC) $(wildcard $(CURDIR)/stub*/*.h) $(wildcard $(ROOT)/include/*.h) $(CURDIR)/midi_host.h $(CURDIR)/host_snap.h
	@mkdir -p $(OUT)
	$(CXX) $(MIDI_FLAGS) -o $@ $(MIDI_SRC)

# Copies the test files (read-only sources, listed in midi-files.list) into build/host/midi/.
$(MIDI_DIR)/.copied: $(CURDIR)/midi-files.list
	@mkdir -p $(MIDI_DIR)
	@grep -v '^#' $< | while IFS= read -r f; do [ -n "$$f" ] && cp -n "$$f" $(MIDI_DIR)/; done; touch $@

midi-baseline: midi $(MIDI_DIR)/.copied
	SECONDS=$(SECONDS) $(CURDIR)/render_midi_all.sh $(OUT)/render_midi $(MIDI_DIR) $(OUT)/midi-baseline

midi-check: midi $(MIDI_DIR)/.copied
	SECONDS=$(SECONDS) $(CURDIR)/render_midi_all.sh $(OUT)/render_midi $(MIDI_DIR) $(OUT)/midi-current
	EXT=wav $(CURDIR)/compare.sh $(OUT)/midi-baseline $(OUT)/midi-current

# M12 look: the real ui_look.cpp and ui_player.cpp drawn into a framebuffer, written as PNGs.
LOOK_SRC := $(ROOT)/src/ui_look.cpp $(ROOT)/src/ui_player.cpp $(ROOT)/src/ui_modes.cpp $(ROOT)/src/ui_rec.cpp $(ROOT)/src/ui_tap.cpp $(ROOT)/src/ui_saver.cpp $(CURDIR)/look_render.cpp
LOOK_H := $(wildcard $(CURDIR)/stub-look/*.h) $(ROOT)/include/ui_look.h $(ROOT)/include/ui_strip.h \
          $(ROOT)/include/ui_player.h $(ROOT)/include/ui_modes.h $(ROOT)/include/ui_rec.h $(ROOT)/include/ui_tap.h $(ROOT)/include/ui_saver.h $(CURDIR)/host_snap.h $(wildcard $(ROOT)/include/fonts/*.h)
OUT_DIR ?= $(OUT)/look

$(OUT)/look_render: $(LOOK_SRC) $(LOOK_H)
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -I$(CURDIR)/stub-look -I$(CURDIR) -I$(ROOT)/include -o $@ $(LOOK_SRC)

look: $(OUT)/look_render
	@mkdir -p $(OUT_DIR)
	$(OUT)/look_render $(OUT_DIR)

# Player screens: renders a few songs, snapshots the UI hand-overs partway
# in, then draws the screens from them.
SNAP_DIR := $(OUT)/snap
TT := $(ROOT)/trackertunes
players: $(OUT)/render $(OUT)/render_midi $(OUT)/look_render $(OUT)/files.txt $(MIDI_DIR)/.copied
	@mkdir -p $(SNAP_DIR) $(OUT_DIR)
	SNAP=12 SNAP_OUT=$(SNAP_DIR)/mod.snap $(OUT)/render "$$(grep AXELF $(OUT)/files.txt)" 14 $(SNAP_DIR)/mod.raw
	SNAP=40 SNAP_OUT=$(SNAP_DIR)/s3m.snap $(OUT)/render "$(TT)/artists/cmd/CMD - The Reflex.s3m" 60 $(SNAP_DIR)/s3m.raw
	SNAP=40 SNAP_OUT=$(SNAP_DIR)/xm.snap $(OUT)/render "$(TT)/various/Kenetic & Seenix - Our Little Fantasy.xm" 42 $(SNAP_DIR)/xm.raw
	SNAP=40 SNAP_OUT=$(SNAP_DIR)/it.snap $(OUT)/render "$(TT)/various/Katie Cadet - Lucky Star.it" 42 $(SNAP_DIR)/it.raw
	SNAP=30 SNAP_OUT=$(SNAP_DIR)/mid.snap $(OUT)/render_midi $(MIDI_DIR)/busy_schedule.mid $(SNAP_DIR)/mid.wav 31 0
	$(OUT)/look_render $(OUT_DIR) --players $(SNAP_DIR)
