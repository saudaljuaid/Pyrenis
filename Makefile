SHELL := /bin/sh

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso-root
KERNEL := $(BUILD_DIR)/seneri.elf
ISO := $(BUILD_DIR)/seneri.iso
SERIAL_LOG := $(BUILD_DIR)/serial.log
TEST_BUILD_DIR := $(BUILD_DIR)/tests
TEST_SCENARIOS := normal breakpoint invalid-opcode page-fault ist pit unexpected \
	double-fault apic ioapic retired apic-timer tsc pm-timer pit-retired timers \
	paging heap pci pci-ecam threads thread-guard framebuffer
TEST_TARGETS := $(addprefix qemu-test-,$(TEST_SCENARIOS))

CC := gcc
LD := ld
NM := nm
OBJDUMP := objdump
RUSTC := rustc
PYTHON := python3

# The one target Rust is built for. It matches the C flags exactly - no MMX, no
# SSE, soft float, no red zone - which is why the two halves can share a stack.
RUST_TARGET := x86_64-unknown-none
RUST_LIB := $(BUILD_DIR)/libseneri.a
RUST_SOURCES := $(wildcard src/rust/*.rs)
LOGO_SOURCE := assets/seneri-logo.png
LOGO_BLOB := $(BUILD_DIR)/logo.srl
LOGO_SIZE := 256

CPPFLAGS := -Iinclude
COMMON_FLAGS := -m64 -g -ffreestanding -fno-pie -fno-stack-protector
CFLAGS := $(COMMON_FLAGS) -std=c11 -O2 -mno-red-zone -mno-mmx -mno-sse \
	-mno-sse2 -msoft-float -fno-tree-vectorize -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes
ASFLAGS := $(COMMON_FLAGS) -Wa,--fatal-warnings
# --orphan-handling=error is what keeps the two languages honest. A section
# neither linker.ld names nor discards is otherwise placed wherever ld prefers,
# which is how a Rust static library silently opened a gap between data and bss
# the first time one was linked in. Now an unnamed section is a link error.
LDFLAGS := -nostdlib -z max-page-size=0x1000 -z noexecstack --fatal-warnings \
	--orphan-handling=error --build-id=none -T linker.ld \
	-Map=$(BUILD_DIR)/seneri.map

C_SOURCES := $(wildcard src/kernel/*.c)
C_OBJECTS := $(patsubst src/kernel/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_SOURCES := $(wildcard src/arch/x86_64/*.S)
ASM_OBJECTS := $(patsubst src/arch/x86_64/%.S,$(BUILD_DIR)/arch_%.o,$(ASM_SOURCES))
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

# Warnings are errors on both sides of the language boundary, and Rust is held
# to the stricter rule that an unsafe operation inside an unsafe function still
# needs its own unsafe block naming why it is sound.
RUSTFLAGS := --edition 2024 --target $(RUST_TARGET) --crate-type staticlib \
	--crate-name seneri -C panic=abort -C opt-level=2 \
	-C relocation-model=static -D warnings
DEPENDENCIES := $(C_OBJECTS:.o=.d)

# The qemu-test-% scenarios are deliberately absent from .PHONY. GNU Make skips
# implicit and pattern rule search for a phony target, so declaring them phony
# makes every scenario resolve to "nothing to be done" and pass without booting.
# They never create a file of their own name, so they rerun regardless.
.PHONY: all clean hooks iso kernel lint qemu-tests run smoke toolchain verify

all: kernel

kernel: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/arch_%.o: src/arch/x86_64/%.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/kernel/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Regenerated only when the logo itself changes. The result is a build
# artifact and is deliberately not committed; src/rust/abi.rs includes it.
$(LOGO_BLOB): $(LOGO_SOURCE) tools/make-logo-asset.py | $(BUILD_DIR)
	$(PYTHON) tools/make-logo-asset.py $(LOGO_SOURCE) $(LOGO_SIZE) $@

$(RUST_LIB): $(RUST_SOURCES) $(LOGO_BLOB) | $(BUILD_DIR)
	SENERI_LOGO_BLOB='$(CURDIR)/$(LOGO_BLOB)' \
		$(RUSTC) $(RUSTFLAGS) -o $@ src/rust/lib.rs

$(KERNEL): $(OBJECTS) $(RUST_LIB) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(RUST_LIB)

toolchain:
	@for tool in gcc ld grub-file readelf nm objdump rustc python3; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	@$(RUSTC) --print target-list | grep -Fxq '$(RUST_TARGET)' || \
		{ echo 'rustc does not know $(RUST_TARGET)'; exit 1; }
	@$(RUSTC) --target $(RUST_TARGET) --print target-libdir >/dev/null 2>&1 || \
		{ echo 'run: rustup target add $(RUST_TARGET)'; exit 1; }

lint:
	@if git grep -nI -E '[[:blank:]]+$$' -- . ':!assets/*'; then \
		echo "trailing whitespace is forbidden"; exit 1; \
	fi

verify: toolchain lint
	$(MAKE) clean
	$(MAKE) kernel
	grub-file --is-x86-multiboot2 $(KERNEL)
	readelf -h $(KERNEL) | grep -Eq 'Class:[[:space:]]+ELF64'
	readelf -h $(KERNEL) | grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64'
	@test -z "$$($(NM) -u $(KERNEL))" || { $(NM) -u $(KERNEL); exit 1; }
	@test "$$($(NM) $(KERNEL) | grep -Ec ' [tT] interrupt_vector_[0-9]+$$')" -eq 256
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'iretq'
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'ltr'
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'lidt'
	# This inspects the ELF file, and for a long time it was the only thing
	# behind Seneri's W^X claim - while the kernel ran on boot.S's huge pages
	# with no NX bit enabled at all. It is kept because it catches a bad link
	# before anything boots, but the guarantee now rests on paging.c walking
	# the installed tables at runtime; see docs/VIRTUAL_MEMORY.md.
	@if readelf -W -l $(KERNEL) | grep -Eq 'LOAD[[:space:]].*RWE'; then \
		echo "kernel contains an RWX load segment"; exit 1; \
	fi
	@$(OBJDUMP) -d $(KERNEL) | grep -Fq 'invlpg'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __text_start$$'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __rodata_start$$'
	@$(NM) $(KERNEL) | grep -Eq ' [ABDRTt] __data_start$$'
	# The Rust half has to actually be in the image, and has to have been
	# linked as ordinary code rather than as something with its own runtime.
	@$(NM) $(KERNEL) | grep -Eq ' T seneri_logo_decode$$'
	@$(NM) $(KERNEL) | grep -Eq ' T seneri_logo_self_test$$'

$(ISO): $(KERNEL) grub/grub.cfg
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNEL) $(ISO_ROOT)/boot/seneri.elf
	cp grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_ROOT)

iso: $(ISO)

$(TEST_BUILD_DIR)/%/seneri.iso: $(KERNEL) Makefile
	rm -rf $(TEST_BUILD_DIR)/$*
	mkdir -p $(TEST_BUILD_DIR)/$*/iso-root/boot/grub
	cp $(KERNEL) $(TEST_BUILD_DIR)/$*/iso-root/boot/seneri.elf
	printf '%s\n' 'set default=0' 'set timeout=0' '' \
		'menuentry "Seneri OS test" {' \
		'    multiboot2 /boot/seneri.elf seneri.test=$*' \
		'    boot' '}' >$(TEST_BUILD_DIR)/$*/iso-root/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(TEST_BUILD_DIR)/$*/iso-root

qemu-test-%: $(TEST_BUILD_DIR)/%/seneri.iso
	@for tool in qemu-system-x86_64 timeout grep; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing tool: $$tool"; exit 1; }; \
	done
	# 0x22, which is status 69, is left unused: it belongs to the ioapic-level
	# scenario in the level-triggered routing change, which was opened against
	# main before these were written.
	@case '$*' in \
		normal) expected=33 ;; \
		breakpoint) expected=35 ;; \
		invalid-opcode) expected=37 ;; \
		page-fault) expected=39 ;; \
		ist) expected=41 ;; \
		pit) expected=43 ;; \
		unexpected) expected=45 ;; \
		double-fault) expected=47 ;; \
		apic) expected=49 ;; \
		ioapic) expected=51 ;; \
		retired) expected=53 ;; \
		apic-timer) expected=55 ;; \
		tsc) expected=57 ;; \
		pm-timer) expected=59 ;; \
		pit-retired) expected=61 ;; \
		timers) expected=63 ;; \
		paging) expected=65 ;; \
		heap) expected=67 ;; \
		pci) expected=71 ;; \
		pci-ecam) expected=73 ;; \
		threads) expected=75 ;; \
		thread-guard) expected=77 ;; \
		framebuffer) expected=79 ;; \
		*) echo 'unknown QEMU scenario: $*'; exit 1 ;; \
	esac; \
	# Only pci-ecam departs from the default machine. i440fx publishes no \
	# MCFG, so every other scenario - including pci - proves the path that \
	# has nothing but the I/O ports. q35 is the only machine here with a \
	# PCI Express host bridge, and the root port is what gives the \
	# enumeration a second bus to find. \
	case '$*' in \
		pci-ecam) \
			hardware='-machine q35 -device pcie-root-port,id=rp0,chassis=1 -device e1000e,bus=rp0 -device e1000e' ;; \
		*) hardware='' ;; \
	esac; \
	log='$(TEST_BUILD_DIR)/$*/serial.log'; \
	rm -f "$$log"; \
	set +e; \
	timeout 15s qemu-system-x86_64 \
		-machine accel=tcg -m 128M -smp 1 $$hardware \
		-cdrom '$<' -display none -monitor none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-no-reboot >"$$log" 2>&1; result=$$?; \
	set -e; \
	begin_count=$$(grep -Fxc 'ST BEGIN $*' "$$log" || true); \
	pass_count=$$(grep -Fxc 'ST PASS $*' "$$log" || true); \
	if test $$result -ne $$expected -o "$$begin_count" -ne 1 -o "$$pass_count" -ne 1 || \
		grep -Fq 'ST FAIL' "$$log" || grep -Fq 'Seneri OS PANIC' "$$log"; then \
		echo 'QEMU scenario $* failed: status='$$result' expected='$$expected; \
		cat "$$log"; \
		exit 1; \
	fi; \
	if test '$*' = normal && \
		{ ! grep -Fq 'Seneri OS: ACPI root verified' "$$log" || \
		  ! grep -Fq 'Seneri OS: ACPI MADT verified' "$$log" || \
		  ! grep -Fq 'Seneri OS: ACPI topology verified' "$$log" || \
		  ! grep -Eq '^Seneri OS: ACPI I/O APIC id [0-9]+ at 0x' "$$log" || \
		  ! grep -Fq 'Seneri OS: local APIC online' "$$log" || \
		  ! grep -Fq 'Seneri OS: local APIC legacy routing LINT0 ExtINT' "$$log" || \
		  ! grep -Fq 'Seneri OS: I/O APIC online' "$$log" || \
		  ! grep -Fq 'Seneri OS: I/O APIC delivered eight interrupts' "$$log" || \
		  ! grep -Fq 'Seneri OS: legacy 8259 retired' "$$log" || \
		  ! grep -Fq 'Seneri OS: timer survives legacy retirement' "$$log" || \
		  ! grep -Eq '^Seneri OS: local APIC timer calibrated at [0-9]+ counts' "$$log" || \
		  ! grep -Fq 'Seneri OS: local APIC timer delivered eight interrupts' "$$log" || \
		  ! grep -Eq '^Seneri OS: TSC calibrated at [0-9]+ Hz' "$$log" || \
		  ! grep -Fq 'Seneri OS: TSC reference established' "$$log" || \
		  ! grep -Fq 'Seneri OS: ACPI FADT verified' "$$log" || \
		  ! grep -Fq 'Seneri OS: ACPI MCFG absent' "$$log" || \
		  ! grep -Fq 'Seneri OS: ACPI configuration windows verified' "$$log" || \
		  ! grep -Eq '^Seneri OS: ACPI PM timer port 0x[0-9A-F]+ width (24|32) bits address (fixed|extended)$$' "$$log" || \
		  ! grep -Eq '^Seneri OS: PM timer counted [0-9]+ ticks in [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'Seneri OS: PM timer independent reference established' "$$log" || \
		  ! grep -Eq '^Seneri OS: clocks agree: PM [0-9]+ ns, APIC timer [0-9]+ ns, TSC [0-9]+ ns$$' "$$log" || \
		  ! grep -Fq 'Seneri OS: PIT retired' "$$log" || \
		  ! grep -Fq 'Seneri OS: clocks survive PIT retirement' "$$log" || \
		  ! grep -Fq 'Seneri OS: monotonic clock on time-stamp counter' "$$log" || \
		  ! grep -Eq '^Seneri OS: slept [0-9]+ ns for a [0-9]+ ns deadline$$' "$$log" || \
		  ! grep -Fq 'Seneri OS: deadline timers online' "$$log" || \
		  ! grep -Fq 'Seneri OS: monotonic time established' "$$log" || \
		  ! grep -Eq '^Seneri OS: paging root 0x[0-9A-F]+ table frames [0-9]+ regions [0-9]+ NX yes write protect yes$$' "$$log" || \
		  ! grep -Eq '^Seneri OS: paging leaves [0-9]+ writable [0-9]+ executable [0-9]+ both 0$$' "$$log" || \
		  ! grep -Fq 'Seneri OS: kernel page tables installed' "$$log" || \
		  ! grep -Fq 'Seneri OS: no writable executable mapping' "$$log" || \
		  ! grep -Fq 'Seneri OS: virtual memory established' "$$log" || \
		  ! grep -Eq '^Seneri OS: heap window 0x[0-9A-F]+ size [0-9]+ guards 0x[0-9A-F]+ 0x[0-9A-F]+$$' "$$log" || \
		  ! grep -Eq '^Seneri OS: heap committed [0-9]+ bytes in [0-9]+ pages, live 3$$' "$$log" || \
		  ! grep -Fq 'Seneri OS: kernel heap online' "$$log" || \
		  ! grep -Fq 'Seneri OS: heap coalesced to one free block' "$$log" || \
		  ! grep -Fq 'Seneri OS: kernel heap established' "$$log" || \
		  ! grep -Eq '^Seneri OS: deadline table of [0-9]+ entries on the heap$$' "$$log" || \
		  ! grep -Eq '^Seneri OS: PCI mechanism 1 online, no window mapped$$' "$$log" || \
		  ! grep -Eq '^Seneri OS: PCI buses [1-9][0-9]* functions [1-9][0-9]* bridges [0-9]+$$' "$$log" || \
		  ! grep -Eq '^Seneri OS: PCI 0:0\.0 vendor 0x[0-9A-F]+ device 0x[0-9A-F]+ class 0x0*6\.0x0* ' "$$log" || \
		  ! grep -Fq 'Seneri OS: PCI configuration space enumerated' "$$log" || \
		  ! grep -Fq 'Seneri OS: PCI enumeration established' "$$log" || \
		  ! grep -Eq '^Seneri OS: threads online, 3 ready of [0-9]+ on 12 stack frames$$' "$$log" || \
		  ! grep -Fxq 'Seneri OS: thread rotation 123123123123' "$$log" || \
		  ! grep -Eq '^Seneri OS: threads switched [1-9][0-9]* times, 3 exited$$' "$$log" || \
		  ! grep -Fq 'Seneri OS: kernel threads established' "$$log" || \
		  ! grep -Eq '^Seneri OS: framebuffer [0-9]+x[0-9]+ at 0x[0-9A-F]+ pitch [0-9]+ RGB [0-9]+/[0-9]+/[0-9]+$$' "$$log" || \
		  ! grep -Fxq 'Seneri OS: framebuffer verified 786432 pixels' "$$log" || \
		  ! grep -Fq 'Seneri OS: framebuffer established' "$$log" || \
		  ! grep -Fq 'Seneri OS: never triple fault milestone passed' "$$log"; }; then \
		echo 'normal scenario did not complete the integrated production path'; \
		cat "$$log"; \
		exit 1; \
	fi; \
	diagnostics_ok=true; \
	case '$*' in \
		invalid-opcode) \
			grep -Fq '  vector=6 name=invalid opcode' "$$log" || diagnostics_ok=false ;; \
		page-fault) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000100000000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=0 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		unexpected) \
			grep -Fq '  vector=128 name=unexpected vector' "$$log" || diagnostics_ok=false ;; \
		double-fault) \
			grep -Fq 'Seneri OS DOUBLE FAULT - HALTED' "$$log" || diagnostics_ok=false ;; \
		paging) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000200000000' "$$log" && \
			grep -Fq '  page-fault bits: P=1 W=1 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		heap) \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000401000000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=1 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
		pci) \
			grep -Eq '^ST PCI ports functions [0-9]+ buses [0-9]+$$' "$$log" && \
			! grep -Fq 'Seneri OS: ACPI MCFG at' "$$log" || \
				diagnostics_ok=false ;; \
		pci-ecam) \
			grep -Fq 'Seneri OS: ACPI MCFG at' "$$log" && \
			grep -Eq '^ST PCI window agreed on [0-9]+ registers of [0-9]+ functions across [0-9]+ buses, [0-9]+ with MSI-X$$' "$$log" && \
			! grep -Eq '^ST PCI window agreed on [0-9]+ registers of 0 functions' "$$log" || \
				diagnostics_ok=false ;; \
		threads) \
			grep -Eq '^ST THREADS created [0-9]+ switches [0-9]+ exited [0-9]+$$' "$$log" || \
				diagnostics_ok=false ;; \
		framebuffer) \
			grep -Eq '^ST FRAMEBUFFER [0-9]+x[0-9]+ probes 16 pitch [0-9]+$$' "$$log" || \
				diagnostics_ok=false ;; \
		thread-guard) \
			grep -Fq 'ST THREAD guard 0x0000000800005000' "$$log" && \
			grep -Fq '  vector=14 name=page fault' "$$log" && \
			grep -Fq '  cr2=0x0000000800005000' "$$log" && \
			grep -Fq '  page-fault bits: P=0 W=1 U=0 RSVD=0 I=0' "$$log" || \
				diagnostics_ok=false ;; \
	esac; \
	if test "$$diagnostics_ok" != true; then \
		echo 'QEMU scenario $* omitted its required diagnostic'; \
		cat "$$log"; \
		exit 1; \
	fi; \
	echo 'QEMU scenario $* passed'

qemu-tests: $(TEST_TARGETS)
	@echo "all deterministic QEMU scenarios passed"

smoke: qemu-test-normal
	@echo "strict boot smoke test passed"

run: iso
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -no-reboot -no-shutdown

hooks:
	git config core.hooksPath .githooks
	@echo "repository hooks enabled"

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
