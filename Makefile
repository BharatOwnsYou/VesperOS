# VesperOS 4 "Nebula" (kernel 0.10) build.
#
#   make iso        builds BOTH kernels and one bootable ISO with a GRUB menu:
#                     - VesperOS 4 x86_64   (64-bit)
#                     - VesperOS 4 i686     (32-bit; the proven kernel, kept as the default/fallback)
#   make x64        just the 64-bit kernel      make i686   just the 32-bit kernel
#   make verify     mechanical checks of the built images (header checksum, page tables, mode-switch order ...)
#   make check      reports missing tools       make test-net   host tests for the network stack
#   make run-ahci / run-nvme   boot in QEMU with an emulated SATA / NVMe drive    make test-disk   automated check of both
#
# Toolchain: works with GNU (gcc/ld) and LLVM-only systems such as Chimera Linux (clang/ld.lld).
# Auto-detected; override explicitly with e.g.  make CC=clang LD=ld.lld
ifeq ($(origin CC),default)
CC := $(shell command -v gcc >/dev/null 2>&1 && echo gcc || echo clang)
endif
ifeq ($(origin LD),default)
LD := $(shell command -v ld >/dev/null 2>&1 && echo ld || echo ld.lld)
endif
# Only the legacy i686 boot stub (boot.asm) needs nasm; the x86_64 boot code is GNU-syntax .S built by $(CC).
ASM ?= nasm

COMMON_SRC = $(wildcard kernel/*.c) $(wildcard drivers/*.c) $(wildcard net/*.c) $(wildcard offline_packages/*.c)

# ------------------------------------------------------------------------------------------- x86_64
X64_BIN    = vesperos4-x86_64.bin
X64_CFLAGS = -m64 -mcmodel=small -mno-red-zone -mgeneral-regs-only -fno-builtin -std=gnu11 -ffreestanding \
             -fno-pie -fno-pic -fno-stack-protector -fno-stack-check -fno-asynchronous-unwind-tables \
             -nostdlib -nostdinc -Wall -Wextra -O1 -Iinclude -Ikernel -Idrivers -Inet
X64_LDFLAGS = -m elf_x86_64 -z max-page-size=0x1000 -nostdlib -T arch/x86_64/linker.ld
X64_OBJ    = $(patsubst %.c,build/x86_64/%.o,$(COMMON_SRC) $(wildcard arch/x86_64/*.c)) \
             $(patsubst %.S,build/x86_64/%.o,$(wildcard arch/x86_64/*.S))

# ------------------------------------------------------------------------------------------- i686 (legacy fallback)
I686_BIN    = vesperos4-i686.bin
I686_CFLAGS = -m32 -march=i686 -mgeneral-regs-only -fno-builtin -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
              -fno-stack-check -nostdlib -nostdinc -Wall -Wextra -O1 -Iinclude -Ikernel -Idrivers -Inet
I686_LDFLAGS = -m elf_i386 -T linker.ld -nostdlib
I686_OBJ    = $(patsubst %.c,build/i686/%.o,$(COMMON_SRC) $(wildcard arch/i686/*.c)) build/i686/boot.o

ISO    = vesperos4.iso
ISODIR = iso

.PHONY: all x64 i686 iso verify verify-x64 clean run run-usb run-virtio run-rtl8139 run-ahci run-nvme test-net test-arch test-vga test-disk test check

all: x64

x64: $(X64_BIN)
i686: $(I686_BIN)

build/x86_64/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(X64_CFLAGS) -c $< -o $@

build/x86_64/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -m64 -c $< -o $@

$(X64_BIN): $(X64_OBJ) arch/x86_64/linker.ld
	$(LD) $(X64_LDFLAGS) -o $@ $(X64_OBJ)

build/i686/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(I686_CFLAGS) -c $< -o $@

build/i686/boot.o: boot.asm
	@mkdir -p $(@D)
	$(ASM) -f elf32 boot.asm -o $@

$(I686_BIN): $(I686_OBJ) linker.ld
	$(LD) $(I686_LDFLAGS) -o $@ $(I686_OBJ)

iso: $(X64_BIN) $(I686_BIN)
	mkdir -p $(ISODIR)/boot/grub
	rm -f $(ISODIR)/boot/vesperos2.bin
	cp $(X64_BIN) $(ISODIR)/boot/$(X64_BIN)
	cp $(I686_BIN) $(ISODIR)/boot/$(I686_BIN)
	cp grub.cfg $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISODIR)

verify-x64: $(X64_BIN)
	python3 tests/verify_boot.py $(X64_BIN) --arch x86_64

verify: $(X64_BIN) $(I686_BIN)
	python3 tests/verify_boot.py $(X64_BIN) --arch x86_64
	python3 tests/verify_boot.py $(I686_BIN) --arch i686

# QEMU (needs qemu-system-x86_64). User-mode networking: guest gets 10.0.2.15 via DHCP, gateway 10.0.2.2, DNS 10.0.2.3.
# The GRUB menu appears: first entry = x86_64, second = i686. Debug output (port 0xE9) goes to qemu-debug.log.
# The RTL8168 has no QEMU model - test that one on real hardware.
QEMU = qemu-system-x86_64 -cdrom $(ISO) -m 512M -serial stdio -debugcon file:qemu-debug.log
run: iso
	$(QEMU) -netdev user,id=n0 -device e1000,netdev=n0

run-virtio: iso
	$(QEMU) -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-modern=on

run-rtl8139: iso
	$(QEMU) -netdev user,id=n0 -device rtl8139,netdev=n0

run-usb: iso
	$(QEMU) -usb -device usb-kbd -device usb-mouse

# Storage: boots against a throwaway 64M image (disk.img, created on first use, never touched by 'make clean').
# 'disks' lists the drive, 'diskread' / 'diskwrites on' / 'diskwrite' poke at it, see docs/STORAGE.md.
disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=64

run-ahci: iso disk.img
	$(QEMU) -boot d -drive id=d0,file=disk.img,if=none,format=raw -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0

run-nvme: iso disk.img
	$(QEMU) -boot d -drive id=n0,file=disk.img,if=none,format=raw -device nvme,drive=n0,serial=vesper0

# Reports which build/ISO/test tools are missing on this machine.
check:
	@echo "compiler: $(CC)   linker: $(LD)   assembler (i686 only): $(ASM)"
	@for t in $(CC) $(LD) $(ASM) grub-mkrescue xorriso mformat python3 qemu-system-x86_64; do \
	  if command -v $$t >/dev/null 2>&1; then echo "  ok       $$t"; else echo "  MISSING  $$t"; fi; \
	done
	@echo "grub-mkrescue also needs the GRUB platform files installed (BIOS: i386-pc, UEFI: x86_64-efi)."

# Host-side unit tests for the network stack + hespernetd (simulated LAN, ASan/UBSan, fuzzing)
test-net:
	gcc -DHOST_TEST -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -o nettest tests/nettest.c net/netif.c net/netutil.c net/arp.c net/ipv4.c net/icmp.c net/udp.c \
	    net/dhcp.c net/dns.c net/hespernetd.c kernel/string.c
	./nettest

# Host-side tests for the x86_64 arch code: IDT/TSS bit encodings, trap-frame layout, crash-dump text, mmio_map page tables
test-arch:
	gcc -DHOST_TEST -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -no-pie \
	    -o archtest tests/archtest.c arch/x86_64/traps.c arch/x86_64/mmio.c
	./archtest

# Host-side test of the framebuffer text renderer: pixel-identical to the old renderer, never writes outside the console
test-vga:
	gcc -DHOST_TEST -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -no-pie \
	    -o vgatest tests/vgatest.c kernel/vga.c kernel/string.c
	./vgatest

# Boots both kernels in QEMU against emulated SATA / NVMe / 4K-sector NVMe drives and checks reads and writes
# on the host side (needs qemu + grub-mkrescue, takes a few minutes).
test-disk: iso
	python3 tests/qemu_disk_test.py

test: test-net test-arch test-vga verify

clean:
	rm -rf build archtest vgatest $(X64_BIN) $(I686_BIN) $(ISO) nettest archtest vgatest vesperos2.bin boot.o qemu-debug.log
	rm -f kernel/*.o drivers/*.o net/*.o offline_packages/*.o
	rm -rf $(ISODIR)/boot/vesperos2.bin $(ISODIR)/boot/$(X64_BIN) $(ISODIR)/boot/$(I686_BIN) $(ISODIR)/boot/grub/grub.cfg
