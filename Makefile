#---------------------------------------------------------------------------------
# Skylanders Portal for Switch. Standard devkitPro / libnx application Makefile.
#
#   make            builds skyportal.nro
#   make clean
#   make test       builds and runs the host-side tests (needs only gcc/g++, no devkitPro)
#---------------------------------------------------------------------------------
.SUFFIXES:

# `make test` and `make clean` do not need devkitPro.
ifeq ($(filter test clean,$(MAKECMDGOALS)),)
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif
endif

TOPDIR ?= $(CURDIR)
-include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET      := skyportal
BUILD       := build
SOURCES     := source source/infinity
DATA        := data
INCLUDES    := source source/infinity

APP_TITLE   := Skylanders Portal
APP_AUTHOR  := skyportal
APP_VERSION := 0.2.0

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# SDL2 + SDL2_ttf from devkitPro portlibs (install: sudo dkp-pacman -S switch-sdl2 switch-sdl2_ttf).
# pkg-config supplies the full static link line (EGL, GLES, freetype, ...).
SDL_PKGCONFIG := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
SDL_LIBS := $(shell $(SDL_PKGCONFIG) --libs --static sdl2 SDL2_ttf 2>/dev/null)
ifeq ($(strip $(SDL_LIBS)),)
SDL_LIBS := -lSDL2_ttf -lfreetype -lbz2 -lpng16 -lz -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau
endif
LIBS    := $(SDL_LIBS) -lnx -lm
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
export APP_TITLE APP_AUTHOR APP_VERSION

.PHONY: $(BUILD) clean all test

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).pfs0 $(TARGET).nso \
	        tests/portal_test tests/hid_test tests/figures_test tests/infinity_test tests/inf_*.o

# Host tests: the protocol core, USB/HID descriptors and figure files, on the PC.
HOSTCXX ?= g++
HOSTCC ?= gcc
HOSTFLAGS := -std=c++17 -Wall -Wextra -Isource
# The Disney Infinity core is C. Only the tests build the blank-figure generator.
INFSRC := source/infinity
test:
	$(HOSTCXX) $(HOSTFLAGS) tests/portal_test.cpp source/portal.cpp -o tests/portal_test
	$(HOSTCXX) $(HOSTFLAGS) tests/hid_test.cpp source/hid_function.cpp source/portal.cpp -o tests/hid_test
	$(HOSTCXX) $(HOSTFLAGS) tests/figures_test.cpp source/figures.cpp source/portal.cpp -o tests/figures_test
	$(HOSTCC) -std=c11 -Wall -Wextra -DINF_ENABLE_BLANK_FIGURES -I$(INFSRC) -c $(INFSRC)/infinity_core.c -o tests/inf_core.o
	$(HOSTCC) -std=c11 -Wall -Wextra -DINF_ENABLE_BLANK_FIGURES -I$(INFSRC) -c $(INFSRC)/inf_crypto.c -o tests/inf_crypto.o
	$(HOSTCC) -std=c11 -Wall -Wextra -DINF_ENABLE_BLANK_FIGURES -I$(INFSRC) -c $(INFSRC)/infinity_figures.c -o tests/inf_figures.o
	$(HOSTCXX) $(HOSTFLAGS) -DINF_ENABLE_BLANK_FIGURES tests/infinity_test.cpp source/infinity_function.cpp source/hid_function.cpp source/figures.cpp source/portal.cpp tests/inf_core.o tests/inf_crypto.o tests/inf_figures.o -o tests/infinity_test
	tests/portal_test
	tests/hid_test
	tests/figures_test
	tests/infinity_test

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
