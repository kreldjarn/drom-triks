# Native build of the engine, sequencer and UI, for developing and listening to
# them without a Daisy board. Uses the host compiler, not the ARM toolchain.
#
#   make -C host run     render a WAV
#   make -C host test    every test suite

DAISYSP_DIR := ../lib/DaisySP
BUILD_DIR   := build

# DaisySP's headers include each other by bare filename, so every source
# directory has to be on the include path.
INCLUDES := $(addprefix -I,$(shell find $(DAISYSP_DIR)/Source -type d))
CXXFLAGS := -std=c++17 -O2 -Wall $(INCLUDES)

# DaisySP is 40-odd files and never changes; build it once into an archive
# rather than recompiling it for every target.
DSP_SRCS := $(shell find $(DAISYSP_DIR)/Source -name '*.cpp')
DSP_OBJS := $(patsubst $(DAISYSP_DIR)/Source/%.cpp,$(BUILD_DIR)/dsp/%.o,$(DSP_SRCS))
DSP_LIB  := $(BUILD_DIR)/libdaisysp_host.a

ENGINE_HDRS := $(wildcard ../src/engine/*.h) $(wildcard ../src/engine/voices/*.h) \
               $(wildcard ../src/seq/*.h) $(wildcard ../src/io/*.h) \
               $(wildcard ../src/ui/*.h) $(wildcard ../src/util/*.h) ../src/machine.h

.PHONY: all clean run test
all: $(BUILD_DIR)/render

$(BUILD_DIR)/dsp/%.o: $(DAISYSP_DIR)/Source/%.cpp
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

$(BUILD_DIR)/seqtest: seqtest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) seqtest.cpp -o $@

$(BUILD_DIR)/locktest: locktest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) locktest.cpp -o $@

$(BUILD_DIR)/clocktest: clocktest.cpp $(ENGINE_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) clocktest.cpp -o $@

.PHONY: seqtest locktest clocktest uitest machinetest
seqtest:     $(BUILD_DIR)/seqtest;     $<
locktest:    $(BUILD_DIR)/locktest;    $<
clocktest:   $(BUILD_DIR)/clocktest;   $<
uitest:      $(BUILD_DIR)/uitest;      $<
machinetest: $(BUILD_DIR)/machinetest; $<

run: $(BUILD_DIR)/render
	$(BUILD_DIR)/render $(BUILD_DIR)/out.wav

test: $(BUILD_DIR)/seqtest $(BUILD_DIR)/locktest $(BUILD_DIR)/clocktest \
      $(BUILD_DIR)/uitest $(BUILD_DIR)/machinetest $(BUILD_DIR)/render
	@$(BUILD_DIR)/seqtest && $(BUILD_DIR)/locktest && $(BUILD_DIR)/clocktest \
	  && $(BUILD_DIR)/uitest && $(BUILD_DIR)/machinetest \
	  && $(BUILD_DIR)/render --selftest

clean:
	rm -rf $(BUILD_DIR)
