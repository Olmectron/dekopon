# SPDX-License-Identifier: GPL-2.0-or-later
#
# Toronja Transfer - fast file transfer between a PC and the SD card over the local network.
#
#   make                    Switch 1 build (devkitPro + libnx)
#   make PLATFORM=switch2   Switch 2 build (ounce-devkit)
#
# Output: out/<platform>/toronja-transfer.nro

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

PLATFORM ?= switch1
TARGET   := toronja-transfer
VERSION  := 1.0.0
OUT      := out/$(PLATFORM)

ifeq ($(PLATFORM),switch2)
include $(DEVKITPRO)/libounce/switch2_rules
ARCH     := $(SWITCH2_CPU) -mtp=soft -fPIE
DEFINES  := -D__SWITCH__ -D__SWITCH2__
LIBDIRS  := $(PORTLIBS) $(LIBNX) $(DEVKITPRO)/portlibs/switch
else
include $(DEVKITPRO)/libnx/switch_rules
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFINES  := -D__SWITCH__
LIBDIRS  := $(PORTLIBS) $(LIBNX)
endif

# The launcher's canvas and font (src/citra_switch/ui_canvas.h) draw the screen.
INCLUDES := $(foreach dir,$(LIBDIRS),-I$(dir)/include -I$(dir)/include/freetype2) \
            -I../../src
LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

CXXFLAGS := -g -O2 -Wall -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDES) \
            -std=gnu++20 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(OUT)/$(TARGET).map
LIBS     := -lfreetype -lharfbuzz -lfreetype -lpng -lbz2 -lz -lnx

TOOLS    := $(DEVKITPRO)/tools/bin

.PHONY: all clean
all: $(OUT)/$(TARGET).nro

$(OUT):
	mkdir -p $@

$(OUT)/main.o: source/main.cpp ../../src/citra_switch/ui_canvas.h | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/$(TARGET).elf: $(OUT)/main.o
	$(CXX) $(LDFLAGS) $^ $(LIBPATHS) $(LIBS) -o $@

$(OUT)/$(TARGET).nacp: | $(OUT)
	$(TOOLS)/nacptool --create "Toronja Transfer" "Olmectron" "$(VERSION)" $@

$(OUT)/$(TARGET).nro: $(OUT)/$(TARGET).elf $(OUT)/$(TARGET).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(OUT)/$(TARGET).nacp --icon=icon.jpg

clean:
	rm -rf out
