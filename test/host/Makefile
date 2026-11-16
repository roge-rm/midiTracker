# Host render harness for the tracker players.
#   make -C test/host            build build/host/render and gen
#   make -C test/host baseline   render every test file into build/host/baseline/
#   make -C test/host check      render into build/host/current/ and compare with baseline
#   make -C test/host midi           build build/host/render_midi (real synth.cpp and midi_file.cpp)
#   make -C test/host midi-baseline  render midi-files.list into build/host/midi-baseline/ (wav + stats)
#   make -C test/host midi-check     render into build/host/midi-current/ and compare with the baseline
#   fluid_compare.sh                 compare the midi renders with fluidsynth's
#   openmpt_compare.sh <list> <outdir> [secs]   correlate .it renders with openmpt123 (uses build/host/corr)
ROOT   := $(abspath ../..)
OUT    := $(ROOT)/build/host
CXX    ?= g++
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
            -Wno-unused-function -Wno-missing-field-initializers \
            -I$(CURDIR)/stub -I$(ROOT)/include
PLAYER_SRC := $(wildcard $(ROOT)/src/tracker_base.cpp) $(ROOT)/src/mod_file.cpp \
              $(ROOT)/src/s3m_file.cpp $(ROOT)/src/xm_file.cpp $(ROOT)/src/it_file.cpp $(ROOT)/src/shared_ram_arena.cpp
HARNESS_SRC := $(CURDIR)/render.cpp $(CURDIR)/host_stubs.cpp
SECONDS ?= 60

.PHONY: all baseline check clean midi midi-baseline midi-check
all: $(OUT)/render $(OUT)/gen $(OUT)/corr

$(OUT)/corr: $(CURDIR)/corr.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -Wall -o $@ $<

$(OUT)/render: $(PLAYER_SRC) $(HARNESS_SRC) $(wildcard $(CURDIR)/stub/*.h) $(wildcard $(ROOT)/include/*.h)
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

# MIDI target: the real synth.cpp and midi_file.cpp with a stub MidiOutput and I2S.
MIDI_SRC := $(ROOT)/src/synth.cpp $(ROOT)/src/midi_file.cpp $(CURDIR)/midi_stubs.cpp $(CURDIR)/render_midi.cpp $(CURDIR)/host_stubs.cpp
MIDI_FLAGS := $(filter-out -I%,$(CXXFLAGS)) -DHOST_MIDI -I$(CURDIR)/stub-midi -I$(CURDIR)/stub -I$(ROOT)/include
MIDI_DIR := $(OUT)/midi

midi: $(OUT)/render_midi $(OUT)/corr $(OUT)/envcorr

$(OUT)/envcorr: $(CURDIR)/envcorr.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -Wall -o $@ $<

$(OUT)/render_midi: $(MIDI_SRC) $(wildcard $(CURDIR)/stub*/*.h) $(wildcard $(ROOT)/include/*.h) $(CURDIR)/midi_host.h
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
