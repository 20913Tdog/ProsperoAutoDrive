# Spike S1: port probe.
#   make        builds the PS5 payload (needs PS5_PAYLOAD_SDK)
#   make host   builds a Linux variant of the same source for CI tests

ELF     := prospero-s1-portprobe.elf
HOSTBIN := s1-portprobe-host
SRC     := main.c
WARN    := -Wall -Wextra -Werror
HOST_CC ?= cc

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

.PHONY: all host clean

all: $(ELF)

$(ELF): $(SRC)
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is undefined - use "make host" for the Linux build)
endif
	$(CC) $(WARN) -O2 -o $@ $^

host: $(HOSTBIN)

$(HOSTBIN): $(SRC)
	$(HOST_CC) $(WARN) -O2 -o $@ $^

clean:
	rm -f $(ELF) $(HOSTBIN)
