SHELL := /bin/bash

SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
BUILD_DIR := $(CURDIR)/.build/darwin
MACH_EXC_DEFS := $(SDKROOT)/usr/include/mach/mach_exc.defs
MACH_EXC_SERVER := $(BUILD_DIR)/mach_excServer.c
MACH_EXC_HEADER := $(BUILD_DIR)/mach_exc.h
MACH_EXC_USER := $(BUILD_DIR)/mach_excUser.c
KERNEL_BUILD_DIR := $(CURDIR)/.build/kernel
KERNEL_VMLINUX := $(KERNEL_BUILD_DIR)/vmlinux
UML_DARWIN := $(BUILD_DIR)/uml-darwin
ROOTFS_STAGED := $(BUILD_DIR)/rootfs.ext2
DEBUGFS := /opt/homebrew/opt/e2fsprogs/sbin/debugfs

CLANG := xcrun --sdk macosx clang
MIG := xcrun --sdk macosx mig -arch arm64
WARNINGS := -Wall -Wextra -Werror
CAPSTONE_CFLAGS := $(shell pkg-config --cflags capstone)
CAPSTONE_STATIC := $(shell pkg-config --variable=libdir capstone)/libcapstone.a
SLIRP_CFLAGS := $(shell pkg-config --cflags slirp)
SLIRP_STATIC := $(shell pkg-config --variable=libdir slirp)/libslirp.a
GLIB_STATIC := $(shell pkg-config --variable=libdir glib-2.0)/libglib-2.0.a
INTL_STATIC := /opt/homebrew/opt/gettext/lib/libintl.a
PCRE2_STATIC := $(shell pkg-config --variable=libdir libpcre2-8)/libpcre2-8.a
JOBS ?= 8

.PHONY: all probes network-probe kernel uml-darwin clean

all: uml-darwin

kernel:
	gmake -j$(JOBS) O=$(KERNEL_BUILD_DIR) \
		-C $(CURDIR)/linux-6.18 ARCH=um SUBARCH=arm64 \
		UML_HOST_OS=Darwin LLVM=/opt/homebrew/opt/llvm/bin/ \
		LD=/opt/homebrew/opt/lld/bin/ld.lld \
		CROSS_COMPILE=aarch64-unknown-linux-gnu-

$(KERNEL_VMLINUX): kernel
	@:

uml-darwin: $(UML_DARWIN) $(ROOTFS_STAGED)

$(ROOTFS_STAGED): rootfs.ext2 darwin/rootfs/debugfs.commands \
		darwin/rootfs/inittab darwin/rootfs/rcS darwin/rootfs/profile | $(BUILD_DIR)
	cp rootfs.ext2 $@
	$(DEBUGFS) -w -f darwin/rootfs/debugfs.commands $@

$(UML_DARWIN): Makefile darwin/loader/launcher.c darwin/loader/elf_loader.c \
		darwin/loader/elf_loader.h darwin/include/uml_elf64.h \
		darwin/include/uml_darwin_runtime.h darwin/runtime/bridge.c \
		darwin/runtime/vm.c darwin/runtime/exception.c \
		darwin/runtime/guest_entry.S darwin/network/slirp.c \
		$(MACH_EXC_SERVER) $(KERNEL_VMLINUX)
	$(CLANG) $(WARNINGS) $(CAPSTONE_CFLAGS) $(SLIRP_CFLAGS) -O3 -flto=thin \
		-Wl,-export_dynamic \
		darwin/loader/launcher.c darwin/loader/elf_loader.c \
		darwin/runtime/bridge.c darwin/runtime/vm.c \
		darwin/runtime/exception.c darwin/runtime/guest_entry.S \
		darwin/network/slirp.c \
		$(MACH_EXC_SERVER) $(CAPSTONE_STATIC) $(SLIRP_STATIC) \
		$(GLIB_STATIC) $(INTL_STATIC) $(PCRE2_STATIC) \
		-lresolv -liconv -lm -framework Foundation \
		-framework CoreFoundation -framework AppKit -framework Carbon \
		-Wl,-sectcreate,__DATA,__uml_elf,$(KERNEL_VMLINUX) -o $@

probes: $(BUILD_DIR)/svc0 $(BUILD_DIR)/mach-svc0 $(BUILD_DIR)/elf-abi-launcher
	$(BUILD_DIR)/svc0
	$(BUILD_DIR)/mach-svc0
	$(BUILD_DIR)/elf-abi-launcher; test $$? -eq 37

network-probe: $(BUILD_DIR)/network-probe
	$(BUILD_DIR)/network-probe

$(BUILD_DIR)/network-probe: darwin/probes/network.c darwin/network/slirp.c \
		darwin/include/uml_darwin_runtime.h | $(BUILD_DIR)
	$(CLANG) $(WARNINGS) $(SLIRP_CFLAGS) -O2 \
		darwin/probes/network.c darwin/network/slirp.c \
		$(SLIRP_STATIC) $(GLIB_STATIC) $(INTL_STATIC) $(PCRE2_STATIC) \
		-lresolv -liconv -lm -framework Foundation \
		-framework CoreFoundation -framework AppKit -framework Carbon \
		-o $@

$(BUILD_DIR):
	mkdir -p $@

$(MACH_EXC_SERVER) $(MACH_EXC_HEADER) $(MACH_EXC_USER) &: $(MACH_EXC_DEFS) | $(BUILD_DIR)
	$(MIG) -server $(MACH_EXC_SERVER) -header $(MACH_EXC_HEADER) \
		-user $(MACH_EXC_USER) $(MACH_EXC_DEFS)

$(BUILD_DIR)/svc0: darwin/probes/svc0.c | $(BUILD_DIR)
	$(CLANG) $(WARNINGS) -O2 $< -o $@

$(BUILD_DIR)/mach-svc0: darwin/probes/mach_svc0.c $(MACH_EXC_SERVER) | $(BUILD_DIR)
	$(CLANG) $(WARNINGS) -O2 darwin/probes/mach_svc0.c \
		$(MACH_EXC_SERVER) -o $@

$(BUILD_DIR)/elf-abi.o: darwin/probes/elf_abi.c | $(BUILD_DIR)
	SDKROOT=$(SDKROOT) /opt/homebrew/opt/llvm/bin/clang \
		--target=aarch64-unknown-linux-gnu -D__APPLE__ -D__MACH__ \
		-D__arm64__ -D__arm64 -U__linux__ -isysroot $(SDKROOT) \
		-isystem $(SDKROOT)/usr/include $(WARNINGS) -fPIC -c $< -o $@

$(BUILD_DIR)/elf-abi: $(BUILD_DIR)/elf-abi.o
	/opt/homebrew/opt/lld/bin/ld.lld -shared -e main \
		--image-base=0x100000000000 \
		--unresolved-symbols=ignore-all $< -o $@

$(BUILD_DIR)/elf-abi-launcher: darwin/loader/launcher.c \
		darwin/loader/elf_loader.c darwin/loader/elf_loader.h \
		darwin/include/uml_elf64.h $(BUILD_DIR)/elf-abi
	$(CLANG) $(WARNINGS) -O2 -DUML_LOADER_PROBE darwin/loader/launcher.c \
		darwin/loader/elf_loader.c \
		-Wl,-sectcreate,__DATA,__uml_elf,$(BUILD_DIR)/elf-abi -o $@

clean:
	rm -rf $(BUILD_DIR)
