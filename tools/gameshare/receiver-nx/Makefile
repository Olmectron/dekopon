# SPDX-License-Identifier: GPL-2.0-or-later
#
# Toronja Link - the receiver for Toronja's GameShare.
#
#   make                    Switch 1 build (devkitPro + libnx + switch-libjpeg-turbo)
#   make PLATFORM=switch2   Switch 2 build (ounce-devkit)
#
# Output: out/<platform>/toronja-link.nro

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

PLATFORM ?= switch1
TARGET   := toronja-link
OUT      := out/$(PLATFORM)

ifeq ($(PLATFORM),switch2)
include $(DEVKITPRO)/libounce/switch2_rules
ARCH     := $(SWITCH2_CPU) -mtp=soft -fPIE
DEFINES  := -D__SWITCH__ -D__SWITCH2__
# libjpeg is plain C: the Switch 1 portlib serves both.
LIBDIRS  := $(PORTLIBS) $(LIBNX) $(DEVKITPRO)/portlibs/switch
else
include $(DEVKITPRO)/libnx/switch_rules
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFINES  := -D__SWITCH__
LIBDIRS  := $(PORTLIBS) $(LIBNX)
endif

# The launcher's canvas and views (and its Amiibo reader) are shared with this app.
INCLUDES := $(foreach dir,$(LIBDIRS),-I$(dir)/include -I$(dir)/include/freetype2) \
            -I../../../src
LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

CXXFLAGS := -g -O2 -Wall -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDES) \
            -std=gnu++20 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(OUT)/$(TARGET).map
LIBS     := -ljpeg -lfreetype -lharfbuzz -lfreetype -lpng -lbz2 -lz -lnx

TOOLS    := $(DEVKITPRO)/tools/bin

.PHONY: all clean
all: $(OUT)/$(TARGET).nro

$(OUT):
	mkdir -p $@

$(OUT)/main.o: source/main.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The USB link is shared with Toronja itself.
$(OUT)/game_share_usb.o: ../../../src/citra_switch/game_share_usb.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/amiibo_reader.o: ../../../src/citra_switch/amiibo_reader.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/main.o: source/link_ui.h ../../../src/citra_switch/ui_views.h \
               ../../../src/citra_switch/ui_canvas.h

$(OUT)/$(TARGET).elf: $(OUT)/main.o $(OUT)/game_share_usb.o $(OUT)/amiibo_reader.o
	$(CXX) $(LDFLAGS) $^ $(LIBPATHS) $(LIBS) -o $@

$(OUT)/$(TARGET).nacp: | $(OUT)
	$(TOOLS)/nacptool --create "Toronja Link" "Olmectron" "0.2.0" $@

$(OUT)/$(TARGET).nro: $(OUT)/$(TARGET).elf $(OUT)/$(TARGET).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(OUT)/$(TARGET).nacp --icon=icon.jpg

clean:
	rm -rf out
