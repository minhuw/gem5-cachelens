# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

X86_CC ?= x86_64-linux-gnu-gcc
ARM_CC ?= aarch64-linux-gnu-gcc

X86_BINARY := bin/x86/linux/cachelens-checkpoint
ARM_BINARY := bin/arm/linux/cachelens-checkpoint

.PHONY: all clean

all: $(X86_BINARY) $(ARM_BINARY)

$(X86_BINARY): src/checkpoint-data-x86.S
	mkdir -p $(dir $@)
	$(X86_CC) -nostdlib -static -no-pie -Wl,--build-id=none -o $@ $<

$(ARM_BINARY): src/checkpoint-data-arm64.S
	mkdir -p $(dir $@)
	$(ARM_CC) -nostdlib -static -no-pie \
		-Wl,--build-id=none -o $@ $<

clean:
	rm -f $(X86_BINARY) $(ARM_BINARY)
