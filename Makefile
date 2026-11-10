# The core for the host (x86_64 or aarch64) with the shim the games load,
# which is always x86-64, built into it. On other hosts the shim needs an
# x86-64 cross compiler (Debian/Ubuntu: gcc-x86-64-linux-gnu).
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -fPIC -Wall -Wextra -std=gnu11 -fvisibility=hidden
GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)

ifeq ($(shell uname -m),x86_64)
SHIM_CC ?= $(CC)
else
SHIM_CC ?= x86_64-linux-gnu-gcc
endif

# FEX_ZIP=<zip>: FEX and its x86-64 libraries (scripts/fetch-fex.py, zipped
# with a build stamp in fex/build) carried in the core, which writes them to
# system/atarivcs; the aarch64 CI build does this
CORE_SOURCES = src/core.c src/daemon.c src/shim_blob.S
ifneq ($(FEX_ZIP),)
CORE_SOURCES += src/miniz/miniz.c src/fex_embed.S
CFLAGS += -DATARIVCS_EMBEDDED_FEX -DATARIVCS_FEX_ZIP='"$(abspath $(FEX_ZIP))"' -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
endif

all: atarivcs_libretro.so

atarivcs_libretro.so: src/core.c src/daemon.c src/daemon.h src/shared.h src/libretro.h src/shim_blob.S atarivcs_shim_x86_64.so $(FEX_ZIP)
	$(CC) $(CFLAGS) -fvisibility=default -DGIT_VERSION=\"$(GIT_VERSION)\" -shared -o $@ $(CORE_SOURCES) -pthread -ldl -lrt -Wl,--version-script=src/link.T

atarivcs_shim_x86_64.so: src/shim.c src/shim_udev.c src/shim_pad.h src/shared.h
	$(SHIM_CC) -O2 -g -fPIC -Wall -Wextra -std=gnu11 -fvisibility=hidden -shared -o $@ src/shim.c src/shim_udev.c -ldl -lrt

clean:
	rm -f atarivcs_libretro.so atarivcs_shim_x86_64.so

.PHONY: all clean
