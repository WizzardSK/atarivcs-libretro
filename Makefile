# The core for the host (x86_64 or aarch64) and the shim the games load,
# which is always x86-64: on an aarch64 host the shim is built by the
# x86-64 CI job and shipped beside the core.
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -fPIC -Wall -Wextra -std=gnu11 -fvisibility=hidden
GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)

all: atarivcs_libretro.so

atarivcs_libretro.so: src/core.c src/daemon.c src/daemon.h src/shared.h src/libretro.h
	$(CC) $(CFLAGS) -fvisibility=default -DGIT_VERSION=\"$(GIT_VERSION)\" -shared -o $@ src/core.c src/daemon.c -pthread -ldl -lrt -Wl,--version-script=src/link.T

atarivcs_shim_x86_64.so: src/shim.c src/shared.h
	$(CC) $(CFLAGS) -shared -o $@ src/shim.c -ldl -lrt

clean:
	rm -f atarivcs_libretro.so atarivcs_shim_x86_64.so

.PHONY: all clean
