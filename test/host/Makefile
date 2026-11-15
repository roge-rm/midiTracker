# Host render harness for the tracker players.
#   make -C test/host            build build/host/render and gen
#   make -C test/host baseline   render every test file into build/host/baseline/
#   make -C test/host check      render into build/host/current/ and compare with baseline
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

.PHONY: all baseline check clean
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
