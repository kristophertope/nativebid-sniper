LIB_DIR := ../lib/sniper
JSON := ../third_party/ArduinoJson.h
JSON_URL := https://github.com/bblanchon/ArduinoJson/releases/download/v7.2.1/ArduinoJson-v7.2.1.h

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I$(LIB_DIR) -I. -I../third_party
SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
ifneq ($(SDK),)
ifeq ($(wildcard /Library/Developer/CommandLineTools/usr/include/c++/v1/map),)
CXXFLAGS += -isystem $(SDK)/usr/include/c++/v1
endif
endif

LIB_SRC := $(wildcard $(LIB_DIR)/*.cpp)
LIB_HDR := $(wildcard $(LIB_DIR)/*.h)

all: sniper sim

sniper: main.cpp platform_host.cpp platform_host.h $(LIB_SRC) $(LIB_HDR) $(JSON)
	$(CXX) $(CXXFLAGS) -o $@ main.cpp platform_host.cpp $(LIB_SRC) -lcurl

sim: sim.cpp $(LIB_SRC) $(LIB_HDR) $(JSON)
	$(CXX) $(CXXFLAGS) -o $@ sim.cpp $(LIB_SRC)

$(JSON):
	mkdir -p $(dir $@)
	curl -sfL -o $@ $(JSON_URL)

clean:
	rm -f sniper sim

.PHONY: all clean
