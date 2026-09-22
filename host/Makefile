# Native build of the voice engine + sequencer, for developing and listening to
# them without a Daisy board. Uses the host compiler, not the ARM toolchain.
#
#   make -C host && host/build/render out.wav && afplay out.wav

DAISYSP_DIR := ../lib/DaisySP
BUILD_DIR   := build
TARGET      := $(BUILD_DIR)/render

# DaisySP's headers include each other by bare filename, so every source
# directory has to be on the include path.
INCLUDES := $(addprefix -I,$(shell find $(DAISYSP_DIR)/Source -type d))
SOURCES  := render.cpp $(shell find $(DAISYSP_DIR)/Source -name '*.cpp')

CXXFLAGS := -std=c++17 -O2 -Wall $(INCLUDES)

.PHONY: all clean run
all: $(TARGET)

$(TARGET): $(SOURCES) ../src/engine/voice.h $(wildcard ../src/engine/voices/*.h) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	$(TARGET) $(BUILD_DIR)/out.wav

clean:
	rm -rf $(BUILD_DIR)

SEQTEST := $(BUILD_DIR)/seqtest
$(SEQTEST): seqtest.cpp $(wildcard ../src/seq/*.h) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) seqtest.cpp -o $@

.PHONY: seqtest
seqtest: $(SEQTEST)
	$(SEQTEST)

LOCKTEST := $(BUILD_DIR)/locktest
$(LOCKTEST): locktest.cpp $(wildcard ../src/seq/*.h) $(wildcard ../src/engine/*.h) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) locktest.cpp -o $@

.PHONY: locktest
locktest: $(LOCKTEST)
	$(LOCKTEST)

.PHONY: test
test: seqtest locktest clocktest uitest
	@$(BUILD_DIR)/render --selftest

CLOCKTEST := $(BUILD_DIR)/clocktest
$(CLOCKTEST): clocktest.cpp $(wildcard ../src/seq/*.h) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) clocktest.cpp -o $@

.PHONY: clocktest
clocktest: $(CLOCKTEST)
	$(CLOCKTEST)

UITEST := $(BUILD_DIR)/uitest
$(UITEST): uitest.cpp $(wildcard ../src/ui/*.h) $(wildcard ../src/io/*.h) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) uitest.cpp -o $@

.PHONY: uitest
uitest: $(UITEST)
	$(UITEST)
