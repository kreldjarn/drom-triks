# Native build of the engine, sequencer and UI, for developing and listening to
# them without a Daisy board. Uses the host compiler, not the ARM toolchain.
#
#   make -C host run     render a WAV
#   make -C host test    every test suite

DAISYSP_DIR := ../lib/DaisySP
BUILD_DIR   := build

# DaisySP's headers include each other by bare filename, so every source
# directory has to be on the include path. DaisySP-LGPL is a separate library
# (LGPL-2.1, where DaisySP proper is MIT) and ships its own tree — we use its
# ReverbSc and Compressor; see the note at the top of src/engine/fx.h.
DAISYSP_LGPL_DIR := $(DAISYSP_DIR)/DaisySP-LGPL
INCLUDES := $(addprefix -I,$(shell find $(DAISYSP_DIR)/Source -type d)) \
            $(addprefix -I,$(shell find $(DAISYSP_LGPL_DIR)/Source -type d))
# -pthread is for machinetest's real two-thread producer/consumer run. Apple
# clang links std::thread without it; GCC on Linux does not, and CI is Linux.
CXXFLAGS := -std=c++17 -O2 -Wall -pthread $(INCLUDES)

# DaisySP is 40-odd files and never changes; build it once into an archive
# rather than recompiling it for every target.
DSP_SRCS      := $(shell find $(DAISYSP_DIR)/Source -name '*.cpp')
DSP_LGPL_SRCS := $(shell find $(DAISYSP_LGPL_DIR)/Source -name '*.cpp')
DSP_OBJS := $(patsubst $(DAISYSP_DIR)/Source/%.cpp,$(BUILD_DIR)/dsp/%.o,$(DSP_SRCS)) \
            $(patsubst $(DAISYSP_LGPL_DIR)/Source/%.cpp,$(BUILD_DIR)/dsp-lgpl/%.o,$(DSP_LGPL_SRCS))

# DaisySP-LGPL is a submodule *of DaisySP*, so a one-level clone leaves it
# empty and the only symptom is a wall of undefined references to ReverbSc and
# Compressor at link time. Say so instead.
ifeq ($(strip $(DSP_LGPL_SRCS)),)
$(error DaisySP-LGPL has no sources. It is nested inside DaisySP, so a \
one-level submodule checkout misses it. Fix with: \
git submodule update --init --recursive)
endif
DSP_LIB  := $(BUILD_DIR)/libdaisysp_host.a

ENGINE_HDRS := $(wildcard ../src/engine/*.h) $(wildcard ../src/engine/voices/*.h) \
               $(wildcard ../src/seq/*.h) $(wildcard ../src/io/*.h) \
               $(wildcard ../src/ui/*.h) $(wildcard ../src/util/*.h) ../src/machine.h

.PHONY: all clean run test linux
all: $(BUILD_DIR)/render

# Build and test the way CI does: Linux, GCC, libstdc++.
#
# Worth having because Apple clang's libc++ includes headers transitively that
# libstdc++ does not, so a missing #include compiles here and fails there. That
# is not a hypothetical - it is how CI first went red. Uses its own BUILD_DIR so
# it cannot collide with the host build's object files.
linux:
	docker run --rm -v "$(CURDIR)/..":/src -w /src/host gcc:13 \
	  make BUILD_DIR=build-linux test

$(BUILD_DIR)/dsp/%.o: $(DAISYSP_DIR)/Source/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/dsp-lgpl/%.o: $(DAISYSP_LGPL_DIR)/Source/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(DSP_LIB): $(DSP_OBJS)
	$(AR) rcs $@ $^

# Targets that instantiate voices need DaisySP; the pure-logic ones do not.
$(BUILD_DIR)/render: render.cpp wav.h $(ENGINE_HDRS) $(DSP_LIB)
	$(CXX) $(CXXFLAGS) render.cpp $(DSP_LIB) -o $@

$(BUILD_DIR)/uitest: uitest.cpp $(ENGINE_HDRS) $(DSP_LIB)
	$(CXX) $(CXXFLAGS) uitest.cpp $(DSP_LIB) -o $@

$(BUILD_DIR)/machinetest: machinetest.cpp $(ENGINE_HDRS) $(DSP_LIB)
	$(CXX) $(CXXFLAGS) machinetest.cpp $(DSP_LIB) -o $@

$(BUILD_DIR)/uxtest: uxtest.cpp $(ENGINE_HDRS) $(DSP_LIB)
	$(CXX) $(CXXFLAGS) uxtest.cpp $(DSP_LIB) -o $@

# macOS only: CoreAudio for output, no third-party dependencies.
$(BUILD_DIR)/play: play.cpp $(ENGINE_HDRS) $(DSP_LIB)
	$(CXX) $(CXXFLAGS) play.cpp $(DSP_LIB) -o $@ \
	  -framework AudioToolbox -framework AudioUnit -framework CoreAudio \
	  -framework CoreFoundation

$(BUILD_DIR)/seqtest: seqtest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) seqtest.cpp -o $@

$(BUILD_DIR)/locktest: locktest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) locktest.cpp -o $@

$(BUILD_DIR)/sysextest: sysextest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) sysextest.cpp -o $@

$(BUILD_DIR)/storagetest: storagetest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) storagetest.cpp -o $@

$(BUILD_DIR)/lfotest: lfotest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) lfotest.cpp -o $@

$(BUILD_DIR)/fxtest: fxtest.cpp $(ENGINE_HDRS) $(DSP_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) fxtest.cpp $(DSP_LIB) -o $@

$(BUILD_DIR)/striptest: striptest.cpp $(ENGINE_HDRS) $(DSP_LIB)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) striptest.cpp $(DSP_LIB) -o $@

$(BUILD_DIR)/clocktest: clocktest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) clocktest.cpp -o $@

.PHONY: seqtest locktest clocktest sysextest storagetest lfotest striptest fxtest uitest machinetest uxtest play
seqtest:     $(BUILD_DIR)/seqtest;     $<
locktest:    $(BUILD_DIR)/locktest;    $<
clocktest:   $(BUILD_DIR)/clocktest;   $<
striptest:   $(BUILD_DIR)/striptest;   $<
fxtest:      $(BUILD_DIR)/fxtest;      $<
lfotest:     $(BUILD_DIR)/lfotest;     $<
storagetest: $(BUILD_DIR)/storagetest; $<
sysextest:   $(BUILD_DIR)/sysextest;   $<
uitest:      $(BUILD_DIR)/uitest;      $<
machinetest: $(BUILD_DIR)/machinetest; $<
uxtest:      $(BUILD_DIR)/uxtest;      $<
play:        $(BUILD_DIR)/play;        $<

run: $(BUILD_DIR)/render
	$(BUILD_DIR)/render $(BUILD_DIR)/out.wav

test: $(BUILD_DIR)/seqtest $(BUILD_DIR)/locktest $(BUILD_DIR)/clocktest \
      $(BUILD_DIR)/sysextest $(BUILD_DIR)/storagetest $(BUILD_DIR)/lfotest $(BUILD_DIR)/striptest $(BUILD_DIR)/fxtest $(BUILD_DIR)/uitest $(BUILD_DIR)/machinetest \
      $(BUILD_DIR)/uxtest $(BUILD_DIR)/render
	@$(BUILD_DIR)/seqtest && $(BUILD_DIR)/locktest && $(BUILD_DIR)/clocktest \
	  && $(BUILD_DIR)/sysextest && $(BUILD_DIR)/storagetest && $(BUILD_DIR)/lfotest && $(BUILD_DIR)/striptest && $(BUILD_DIR)/fxtest \
	  && $(BUILD_DIR)/uitest && $(BUILD_DIR)/machinetest \
	  && $(BUILD_DIR)/uxtest && $(BUILD_DIR)/render --selftest

clean:
	rm -rf $(BUILD_DIR)
