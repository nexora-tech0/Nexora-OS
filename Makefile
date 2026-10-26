# Nexora OS top-level Makefile
#
#   make iso        build/nexora-os.iso (default)
#   make run        boot the ISO in QEMU + OVMF, serial on stdio
#   make test       headless boot smoke test (normal boot + both stop-screen diagnostics)
#   make vbox       create/refresh the VirtualBox VM "Nexora OS" (scripts/create-vbox-vm.sh)
#   make toolchain  install the pinned LLVM/Rust/Limine into $(HZ_TOOLCHAIN)
#   make clean
#
# All paths are relative: the checkout may live under a directory containing spaces.

.SUFFIXES:
.DELETE_ON_ERROR:
.DEFAULT_GOAL := iso

include scripts/versions.env

# ---- Toolchain ------------------------------------------------------------------------------

HZ_TOOLCHAIN ?= $(HOME)/.nexora-os-toolchain

ifneq ($(wildcard $(HZ_TOOLCHAIN)/llvm/bin/clang),)
LLVM_BIN := $(HZ_TOOLCHAIN)/llvm/bin/
endif
ifneq ($(wildcard $(HZ_TOOLCHAIN)/cargo/bin/cargo),)
export RUSTUP_HOME := $(HZ_TOOLCHAIN)/rustup
export CARGO_HOME := $(HZ_TOOLCHAIN)/cargo
CARGO := $(HZ_TOOLCHAIN)/cargo/bin/cargo
else
CARGO := cargo
endif

CC := $(LLVM_BIN)clang
CXX := $(LLVM_BIN)clang++
LD := $(LLVM_BIN)ld.lld
LLD_LINK := $(LLVM_BIN)lld-link
NASM := nasm
XORRISO := xorriso
HOSTCC ?= cc
PYTHON ?= python3
LIMINE_DIR ?= $(HZ_TOOLCHAIN)/limine

BUILD := build

# V=1 shows full command lines.
ifeq ($(V),1)
Q :=
else
Q := @
endif
HZ_BUILD_ID := $(shell git describe --always --dirty 2>/dev/null || echo local)
# File times inside the boot images (M11: the 1.0.0 release date, not 1970; reproducible).
SOURCE_DATE_EPOCH ?= 1791374400

# ---- Kernel flags ---------------------------------------------------------------------------

CLANG_RESOURCE_DIR := $(shell $(CC) -print-resource-dir 2>/dev/null)

KERNEL_TARGET_FLAGS := --target=x86_64-unknown-none-elf -march=x86-64 -mcmodel=kernel \
    -mno-red-zone -mgeneral-regs-only -fno-pic -fno-pie

KERNEL_COMMON_FLAGS := $(KERNEL_TARGET_FLAGS) -ffreestanding -fno-stack-protector \
    -fno-stack-check -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections \
    -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -nostdinc -isystem $(CLANG_RESOURCE_DIR)/include \
    -Iinclude -Inet -Ithird_party/limine-protocol -I$(BUILD)/gen -MMD -MP

KERNEL_CFLAGS := -std=c17 $(KERNEL_COMMON_FLAGS)
KERNEL_CXXFLAGS := -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdinc++ \
    $(KERNEL_COMMON_FLAGS)
KERNEL_NASMFLAGS := -f elf64 -g -F dwarf -w+all
KERNEL_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
    --gc-sections --build-id=none -T init/kernel.ld

# Directories linked into the kernel image (every .c/.cpp/.asm in them is built).
KERNEL_SRC_DIRS := init hal ke mm ob ps io ex rtl win32k net drivers/bootvid drivers/pci drivers/disk drivers/ahci \
    drivers/net drivers/i8042 drivers/ramdisk fs/ustarfs fs/fat32

KERNEL_C_SRCS := $(foreach d,$(KERNEL_SRC_DIRS),$(wildcard $(d)/*.c))
KERNEL_CXX_SRCS := $(foreach d,$(KERNEL_SRC_DIRS),$(wildcard $(d)/*.cpp))
KERNEL_ASM_SRCS := $(foreach d,$(KERNEL_SRC_DIRS),$(wildcard $(d)/*.asm))

KOBJ := $(BUILD)/kobj
KERNEL_OBJS := $(KERNEL_C_SRCS:%=$(KOBJ)/%.o) $(KERNEL_CXX_SRCS:%=$(KOBJ)/%.o) \
    $(KERNEL_ASM_SRCS:%=$(KOBJ)/%.o)

# ---- Boot fonts (Symbol:third_party-relative TTF stem:pixels) -------------------------------

BOOTFONTS := FontUi14:inter/Inter-Regular:14 FontUi16:inter/Inter-Regular:16 \
    FontUiSemi16:inter/Inter-SemiBold:16 FontUiSemi24:inter/Inter-SemiBold:24 \
    FontDisplay44:inter/InterDisplay-SemiBold:44 FontMono13:jetbrains-mono/JetBrainsMono-Regular:13

FONTGEN := $(BUILD)/host/fontgen
FONT_SRCS := $(foreach f,$(BOOTFONTS),$(BUILD)/gen/fonts/$(word 1,$(subst :, ,$(f))).c)
FONT_OBJS := $(FONT_SRCS:%=$(KOBJ)/%.o)

define FONT_RULE
$(BUILD)/gen/fonts/$(1).c: third_party/$(2).ttf $(FONTGEN)
	@mkdir -p $$(@D)
	$(Q)$(FONTGEN) $$< $(3) $(1) "$(2) $(3)px" $$@
endef
$(foreach f,$(BOOTFONTS),$(eval $(call FONT_RULE,$(word 1,$(subst :, ,$(f))),$(word 2,$(subst :, ,$(f))),$(word 3,$(subst :, ,$(f))))))

# ---- Rust region library --------------------------------------------------------------------

RUST_CRATE_DIR := win32k/rgn
RUST_LIB := $(BUILD)/rust/$(RUST_KERNEL_TARGET)/release/libhorizon_rgn.a
RUST_SRCS := $(wildcard $(RUST_CRATE_DIR)/src/*.rs) $(RUST_CRATE_DIR)/Cargo.toml rust-toolchain.toml Makefile
RUST_KERNEL_FLAGS := -C code-model=kernel -C relocation-model=static -C force-frame-pointers=yes \
    --remap-path-prefix=src/=$(RUST_CRATE_DIR)/src/

# ---- User mode (PE32+) ----------------------------------------------------------------------

# PE32+ programs: Microsoft x64 ABI, no CRT, linked statically against the system-call stubs
# (user/lib/hzsys) and the runtime (user/lib/hzrt + rtl/string.c + rtl/format.c). No stack
# probes: the user stack is fully reserved up front with one guard page at the bottom.
USER_COMMON_FLAGS := --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
    -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
    -nostdinc -isystem $(CLANG_RESOURCE_DIR)/include -Iinclude -Iuser/include \
    -Ithird_party/stb/compat -MMD -MP
USER_CFLAGS := -std=c17 $(USER_COMMON_FLAGS)
USER_CXXFLAGS := -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics $(USER_COMMON_FLAGS)
USER_NASMFLAGS := -f win64 -w+all
USER_LDFLAGS := /nologo /nodefaultlib /machine:x64 /dynamicbase /nxcompat /subsystem:console \
    /entry:HzProcessStartup /stack:0x100000,0x10000
UOBJ := $(BUILD)/uobj
USER_RUNTIME_OBJS := $(UOBJ)/user/lib/hzsys/hzsys.asm.obj $(UOBJ)/user/lib/hzrt/crt.cpp.obj \
    $(UOBJ)/user/lib/hzrt/heap.cpp.obj $(UOBJ)/user/lib/hzrt/hzcore.cpp.obj $(UOBJ)/user/lib/hzrt/hzfile.cpp.obj \
    $(UOBJ)/user/lib/hzrt/hzapps.cpp.obj $(UOBJ)/user/lib/hzrt/hzcrypto.cpp.obj \
    $(UOBJ)/user/lib/hzrt/hzarchive.cpp.obj \
    $(UOBJ)/user/lib/hzrt/hzimage.cpp.obj \
    $(UOBJ)/rtl/string.c.obj \
    $(UOBJ)/rtl/format.c.obj
# hzuser (windows, drawing, fonts) is an archive: programs pull in only what they use.
HZUSER_OBJS := $(UOBJ)/user/lib/hzuser/hzusersys.asm.obj $(UOBJ)/user/lib/hzuser/window.cpp.obj \
    $(UOBJ)/user/lib/hzuser/draw.cpp.obj $(UOBJ)/user/lib/hzuser/font.cpp.obj
HZUSER_LIB := $(BUILD)/user/hzuser.lib
# hzui, the C++ UI toolkit over hzuser.
HZUI_OBJS := $(patsubst %,$(UOBJ)/user/lib/hzui/%.cpp.obj,painter element window controls textbox listview menu \
    editor dialogs wallpaper)
HZUI_LIB := $(BUILD)/user/hzui.lib
UI_FONTS := third_party/inter/Inter-Regular.ttf third_party/inter/Inter-SemiBold.ttf \
    third_party/inter/InterDisplay-SemiBold.ttf \
    third_party/jetbrains-mono/JetBrainsMono-Regular.ttf \
    third_party/icons/material/MaterialIcons-Regular.ttf
# Programs built from every .cpp in a directory (name:directory). The old user/tests programs
# (hello, usertest, crash, fstest, wintest, uitest, welcome, apptest) were removed with item 19, so
# the build has no separate standalone-program list any more: every executable is an app directory.
USER_APP_DIRS := user/shell $(patsubst %/,%,$(sort $(dir $(wildcard user/apps/*/*.cpp))))
USER_APP_NAMES := $(notdir $(USER_APP_DIRS))
USER_EXES := $(USER_APP_NAMES:%=$(BUILD)/user/%.exe)

# ---- Images ---------------------------------------------------------------------------------

KERNEL_ELF := $(BUILD)/hzkrnl.elf
KERNEL_SYM := $(BUILD)/hzkrnl.sym
RAMDISK := $(BUILD)/ramdisk.tar
# Boot files laid out as on an EFI system partition, for Setup (live boots load it as "install").
INSTALL_TAR := $(BUILD)/install.tar
ISO ?= $(BUILD)/nexora-os.iso
ISO_ROOT ?= $(BUILD)/iso_root
LIMINE_CONF ?= boot/limine.conf

# =============================================================================================

.PHONY: all iso kernel ramdisk run test testdisk vbox toolchain check-tools check-apps clean distclean FORCE

all: iso
iso: check-tools check-apps $(ISO)
kernel: $(KERNEL_ELF)
ramdisk: $(RAMDISK)

check-tools:
	@missing=""; \
	for t in "$(CC)" "$(LD)" "$(LLD_LINK)" "$(NASM)" "$(XORRISO)" "$(CARGO)" "$(HOSTCC)"; do \
	    command -v "$$t" >/dev/null 2>&1 || missing="$$missing $$t"; \
	done; \
	[ -f "$(LIMINE_DIR)/BOOTX64.EFI" ] || missing="$$missing limine($(LIMINE_DIR))"; \
	if [ -n "$$missing" ]; then \
	    echo "error: missing tools:$$missing"; \
	    echo "Run 'make toolchain' (or use scripts/build.sh, which builds inside Docker)."; \
	    exit 1; \
	fi

check-apps:
	$(PYTHON) scripts/validate-app-catalog.py --binary $(USER_APP_NAMES)

$(BUILD)/gen/hzbuildid.h: FORCE
	@mkdir -p $(@D)
	@echo '#define HZ_BUILD_ID "$(HZ_BUILD_ID)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# ---- Kernel ---------------------------------------------------------------------------------

$(KOBJ)/%.c.o: %.c | $(BUILD)/gen/hzbuildid.h
	@mkdir -p $(@D)
	@echo "  CC      $<"
	$(Q)$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KOBJ)/%.cpp.o: %.cpp | $(BUILD)/gen/hzbuildid.h
	@mkdir -p $(@D)
	@echo "  CXX     $<"
	$(Q)$(CXX) $(KERNEL_CXXFLAGS) -c $< -o $@

$(KOBJ)/%.asm.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	$(Q)$(NASM) $(KERNEL_NASMFLAGS) -MD $(@:.o=.d) -MP $< -o $@

$(FONTGEN): tools/fontgen/fontgen.c third_party/stb/stb_truetype.h
	@mkdir -p $(@D)
	@echo "  HOSTCC  $<"
	$(Q)$(HOSTCC) -O2 -std=c11 -Ithird_party/stb -o $@ $< -lm

$(RUST_LIB): $(RUST_SRCS)
	@echo "  CARGO   $(RUST_CRATE_DIR)"
	+$(Q)RUSTFLAGS="$(RUST_KERNEL_FLAGS)" $(CARGO) build --release --quiet \
	    --manifest-path $(RUST_CRATE_DIR)/Cargo.toml --target $(RUST_KERNEL_TARGET) \
	    --target-dir $(BUILD)/rust

$(KERNEL_ELF): $(KERNEL_OBJS) $(FONT_OBJS) $(RUST_LIB) init/kernel.ld
	@echo "  LD      $@"
	$(Q)$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS) $(FONT_OBJS) $(RUST_LIB)
	@echo "kernel: $@ ($$(stat -c %s $@) bytes)"

# Symbol table boot module (tools/mksym.py): stop screens and logs symbolize addresses with it.
$(KERNEL_SYM): $(KERNEL_ELF) tools/mksym.py
	$(Q)$(PYTHON) tools/mksym.py $(LLVM_BIN)llvm-nm $< $@

# ---- User mode ------------------------------------------------------------------------------

$(UOBJ)/%.c.obj: %.c
	@mkdir -p $(@D)
	@echo "  CC      $< (user)"
	$(Q)$(CC) $(USER_CFLAGS) -c $< -o $@

$(UOBJ)/%.cpp.obj: %.cpp
	@mkdir -p $(@D)
	@echo "  CXX     $< (user)"
	$(Q)$(CXX) $(USER_CXXFLAGS) -c $< -o $@

$(UOBJ)/%.asm.obj: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $< (user)"
	$(Q)$(NASM) $(USER_NASMFLAGS) $< -o $@

$(HZUSER_LIB): $(HZUSER_OBJS)
	@mkdir -p $(@D)
	@echo "  LIB     $@"
	$(Q)rm -f $@
	$(Q)$(LLVM_BIN)llvm-ar rcs --format=coff $@ $^

$(HZUI_LIB): $(HZUI_OBJS)
	@mkdir -p $(@D)
	@echo "  LIB     $@"
	$(Q)rm -f $@
	$(Q)$(LLVM_BIN)llvm-ar rcs --format=coff $@ $^

define USER_APP_RULE
$(BUILD)/user/$(1).exe: $(patsubst %,$(UOBJ)/%.obj,$(wildcard $(2)/*.cpp)) $(USER_RUNTIME_OBJS) $(HZUI_LIB) $(HZUSER_LIB)
	@mkdir -p $$(@D)
	@echo "  LINK    $$@"
	$(Q)$(LLD_LINK) $(USER_LDFLAGS) /out:$$@ $$^
endef
$(foreach d,$(USER_APP_DIRS),$(eval $(call USER_APP_RULE,$(notdir $(d)),$(d))))

# ---- Ramdisk and ISO ------------------------------------------------------------------------

ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

LICENSE_FILES := THIRD_PARTY_NOTICES.md third_party/inter/LICENSE.txt \
    third_party/jetbrains-mono/OFL.txt third_party/limine-protocol/LICENSE \
    third_party/icons/material/LICENSE.txt third_party/stb/LICENSE.txt

.PHONY: prepare-wallpapers
prepare-wallpapers:
	$(Q)$(PYTHON) tools/prepare-wallpapers.py assets/Wallpapers $(BUILD)/wallpapers

# The system tree, the same one Setup copies to an installed disk: C:\Nexora holds System (the
# programs), Fonts, Apps (the manifests), Wallpapers, Licenses and Settings (the .ini files that
# SettingsPath() reads and writes); rootfs/ supplies Apps and anything else a clean install starts
# with. C:\Users and its per-user folders are not here: they belong to an installation, not to the
# installation media.
$(RAMDISK): $(USER_EXES) $(ROOTFS_FILES) $(LICENSE_FILES) $(UI_FONTS) prepare-wallpapers
	rm -rf $(BUILD)/ramdisk_root
	mkdir -p $(BUILD)/ramdisk_root/Nexora/System $(BUILD)/ramdisk_root/Nexora/Fonts \
	    $(BUILD)/ramdisk_root/Nexora/Apps $(BUILD)/ramdisk_root/Nexora/Wallpapers \
	    $(BUILD)/ramdisk_root/Nexora/Licenses $(BUILD)/ramdisk_root/Nexora/Settings
	cp -R rootfs/. $(BUILD)/ramdisk_root/
	cp $(USER_EXES) $(BUILD)/ramdisk_root/Nexora/System/
	cp $(UI_FONTS) $(BUILD)/ramdisk_root/Nexora/Fonts/
	cp -R $(BUILD)/wallpapers/. $(BUILD)/ramdisk_root/Nexora/Wallpapers/
	cp THIRD_PARTY_NOTICES.md $(BUILD)/ramdisk_root/Nexora/Licenses/ThirdPartyNotices.md
	cp third_party/inter/LICENSE.txt $(BUILD)/ramdisk_root/Nexora/Licenses/Inter-OFL.txt
	cp third_party/jetbrains-mono/OFL.txt $(BUILD)/ramdisk_root/Nexora/Licenses/JetBrainsMono-OFL.txt
	cp third_party/icons/material/LICENSE.txt $(BUILD)/ramdisk_root/Nexora/Licenses/MaterialIcons-Apache-2.0.txt
	cp third_party/limine-protocol/LICENSE $(BUILD)/ramdisk_root/Nexora/Licenses/Limine-protocol-0BSD.txt
	cp third_party/stb/LICENSE.txt $(BUILD)/ramdisk_root/Nexora/Licenses/stb-MIT.txt
	cp $(LIMINE_DIR)/LICENSE $(BUILD)/ramdisk_root/Nexora/Licenses/Limine-BSD-2-Clause.txt
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@$(SOURCE_DATE_EPOCH) \
	    -cf $@ -C $(BUILD)/ramdisk_root Nexora

$(INSTALL_TAR): $(KERNEL_ELF) $(KERNEL_SYM) $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/LICENSE
	rm -rf $(BUILD)/install_root
	mkdir -p $(BUILD)/install_root/EFI/BOOT $(BUILD)/install_root/boot/limine
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/install_root/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL_ELF) $(KERNEL_SYM) $(BUILD)/install_root/boot/
	cp $(LIMINE_DIR)/LICENSE $(BUILD)/install_root/boot/limine/LICENSE
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@$(SOURCE_DATE_EPOCH) \
	    -cf $@ -C $(BUILD)/install_root EFI boot

$(ISO): $(KERNEL_ELF) $(KERNEL_SYM) $(RAMDISK) $(INSTALL_TAR) $(LIMINE_CONF) $(LIMINE_DIR)/BOOTX64.EFI \
        $(LIMINE_DIR)/limine-uefi-cd.bin
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	cp $(KERNEL_ELF) $(KERNEL_SYM) $(RAMDISK) $(INSTALL_TAR) $(ISO_ROOT)/boot/
	cp $(LIMINE_CONF) $(ISO_ROOT)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	cp $(LIMINE_DIR)/LICENSE $(ISO_ROOT)/boot/limine/LICENSE
	cp THIRD_PARTY_NOTICES.md $(ISO_ROOT)/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	$(XORRISO) -as mkisofs -quiet -R -r -J -V NEXORAOS \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(ISO_ROOT) -o $@
	@echo "iso: $@ ($$(stat -c %s $@) bytes)"

# ---- Running --------------------------------------------------------------------------------

# FAT32 test disk (scripts/mkdisk.sh): attached by `make run` and the smoke tests.
TEST_DISK := $(BUILD)/test/disk.img

testdisk: $(TEST_DISK)

$(TEST_DISK): scripts/mkdisk.sh
	scripts/mkdisk.sh $@

run: iso $(TEST_DISK)
	QEMU_DISK=$(TEST_DISK) scripts/run-qemu.sh $(ISO)

test: iso $(TEST_DISK)
	$(PYTHON) scripts/smoke-test.py

vbox: iso
	scripts/create-vbox-vm.sh $(ISO)

toolchain:
	scripts/setup-toolchain.sh

clean:
	rm -rf $(BUILD)

distclean: clean

-include $(shell find $(BUILD)/kobj $(BUILD)/uobj -name '*.d' 2>/dev/null)
