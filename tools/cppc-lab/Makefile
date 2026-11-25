# Independent offline host build. Does not include the repository/kext Makefiles.
CXX := clang++
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD := $(ROOT)/build
WARN := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wformat=2
BASE := -std=c++17 $(WARN) -fno-exceptions -fno-rtti -I$(ROOT)/src -I$(ROOT)/host
CORE := $(ROOT)/src/Descriptors.cpp $(ROOT)/src/PccRead.cpp
HOST := $(ROOT)/host/Input.cpp
HEADERS := $(ROOT)/src/CppcLab.hpp $(ROOT)/host/Input.hpp
FIXTURE := $(ROOT)/fixtures
INPUTS := $(FIXTURE)/pcct.hex $(FIXTURE)/madt.hex $(FIXTURE)/static.cpc $(FIXTURE)/xnu.map
SAN := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all

.PHONY: all test sanitize check freestanding
all: $(BUILD)/cppc-lab
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/cppc-lab: $(CORE) $(HOST) $(ROOT)/host/main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(BASE) -O2 $(CORE) $(HOST) $(ROOT)/host/main.cpp -o $@
$(BUILD)/tests: $(CORE) $(HOST) $(ROOT)/tests/test.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(BASE) -O2 $(CORE) $(HOST) $(ROOT)/tests/test.cpp -o $@
$(BUILD)/tests-san: $(CORE) $(HOST) $(ROOT)/tests/test.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(BASE) $(SAN) $(CORE) $(HOST) $(ROOT)/tests/test.cpp -o $@
$(BUILD)/cppc-lab-san: $(CORE) $(HOST) $(ROOT)/host/main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(BASE) $(SAN) $(CORE) $(HOST) $(ROOT)/host/main.cpp -o $@
test: $(BUILD)/tests $(BUILD)/cppc-lab
	$(BUILD)/tests $(INPUTS)
	$(BUILD)/cppc-lab --hex $(INPUTS)
sanitize: $(BUILD)/tests-san $(BUILD)/cppc-lab-san
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/tests-san $(INPUTS)
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/cppc-lab-san --hex $(INPUTS)
freestanding: | $(BUILD)
	$(CXX) $(BASE) -ffreestanding -c $(ROOT)/src/Descriptors.cpp -o $(BUILD)/Descriptors.o
	$(CXX) $(BASE) -ffreestanding -c $(ROOT)/src/PccRead.cpp -o $(BUILD)/PccRead.o
check: test sanitize freestanding
