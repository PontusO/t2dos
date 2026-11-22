# T2DOS build: cross compiler on the PC, userland binaries into an SD card image tree (root/)
CC_HOST = src/compiler/cc
PROGS = $(patsubst src/userland/%.c,root/bin/%.bin,$(wildcard src/userland/*.c))
BINUTILS = nm ar objdump
BIN_BINS = $(patsubst %,root/bin/%.bin,$(BINUTILS))
HOST_BINS = $(patsubst %,build/host/%,$(BINUTILS))
all: $(CC_HOST) root/bin/cc.bin $(PROGS) $(BIN_BINS) build/host/as root/bin/as.bin build/host/ld root/bin/ld.bin root/lib/sys.h root/lib/net.c root/lib/mtab.c root/lib/t2obj.h root/lib/t2obj.c root/lib/math.h root/bin/basic.bin basic_examples
# Board binaries: src/compiler/cc compiles to assembly and runs build/host/as and build/host/ld
# itself (temporary files in /tmp). TOOLS is what every such build needs.
TOOLS = $(CC_HOST) build/host/as build/host/ld sys.h
# CCBIN source flags: sources to $@. OPT=-O builds with the peephole optimizer.
OPT =
define CCBIN
	@mkdir -p root/bin
	./$(CC_HOST) $(OPT) $(2) -o $@ $(1)
endef
# the compiler's own sources: one file per stage (src/compiler/cc.h holds what they share).
# Adding a file lands here free, but Makefile.board's /src/cc.bin line and the
# check_make plan-board fixture list them too - update those as well (--pc fails on drift).
CC_SRC = $(sort $(wildcard src/compiler/*.c))
# gcc finds sys.h through -I .; T2CC resolves it against the current directory (the repo root)
$(CC_HOST): $(CC_SRC) sys.h
	gcc -O2 -w -I . -I src/compiler -o $@ $(CC_SRC)
root/bin/cc.bin: $(CC_SRC) $(TOOLS)
	@mkdir -p root/lib root/src
	$(call CCBIN,$(CC_SRC),-I src/compiler)
root/bin/%.bin: src/userland/%.c $(TOOLS) src/lib/net.c src/lib/mtab.c
	$(call CCBIN,$<,-I src/lib)
# binutils: host builds for the PC (tests) and board builds with src/compiler/cc
build/host/%: src/binutils/%.c src/lib/t2obj.h src/lib/t2obj.c
	@mkdir -p build/host
	gcc -O2 -w -I src/lib -I . -o $@ $<
root/bin/%.bin: src/binutils/%.c $(TOOLS) src/lib/t2obj.h src/lib/t2obj.c
	$(call CCBIN,$<,-I src/lib)
# BASIC: several files in src/basic/ (pcsys.c is the PC test build's shim, not part of it); cc takes
# one -I, so basic sources find sys.h in the current directory (the repo root)
BASIC_SRC = $(sort $(filter-out src/basic/pcsys.c,$(wildcard src/basic/*.c)))
root/bin/basic.bin: $(BASIC_SRC) src/basic/basic.h $(TOOLS)
	$(call CCBIN,$(BASIC_SRC),-I src/basic)
# example programs: examples/basic/*.bas into root/src/basic/
BASIC_EX = $(patsubst examples/basic/%,root/src/basic/%,$(wildcard examples/basic/*.bas))
basic_examples: $(BASIC_EX)
root/src/basic/%.bas: examples/basic/%.bas
	@mkdir -p root/src/basic && cp $< $@
.PHONY: basic_examples
# the assembler: same pattern, source in src/assembler/
build/host/as: src/assembler/as.c src/lib/t2obj.h src/lib/t2obj.c
	@mkdir -p build/host
	gcc -O2 -w -I src/lib -I . -o $@ src/assembler/as.c
root/bin/as.bin: src/assembler/as.c $(TOOLS) src/lib/t2obj.h src/lib/t2obj.c
	$(call CCBIN,src/assembler/as.c,-I src/lib)
# the linker: same pattern, source in src/linker/
build/host/ld: src/linker/ld.c src/lib/t2obj.h src/lib/t2obj.c
	@mkdir -p build/host
	gcc -O2 -w -I src/lib -I . -o $@ src/linker/ld.c
root/bin/ld.bin: src/linker/ld.c $(TOOLS) src/lib/t2obj.h src/lib/t2obj.c
	$(call CCBIN,src/linker/ld.c,-I src/lib)
root/lib/t2obj.h: src/lib/t2obj.h
	@mkdir -p root/lib && cp $< root/lib/
root/lib/t2obj.c: src/lib/t2obj.c
	@mkdir -p root/lib && cp $< root/lib/
root/lib/net.c: src/lib/net.c
	@mkdir -p root/lib && cp $< root/lib/
root/lib/mtab.c: src/lib/mtab.c
	@mkdir -p root/lib && cp $< root/lib/
root/lib/sys.h: sys.h
	@mkdir -p root/lib && cp $< root/lib/
root/lib/math.h: src/lib/math.h
	@mkdir -p root/lib && cp $< root/lib/
# Linux/ARM harness: make host && printf 'ls\n' | qemu-arm ./host_linux -r root /bin/sh.bin
host: host_linux.c sys.h src/kernel/netfs.c src/kernel/netfs.h src/kernel/telnetd.c src/kernel/telnetd.h src/kernel/tz.c src/kernel/tz.h
	arm-linux-gnueabihf-gcc -O1 -U_FORTIFY_SOURCE -mthumb -w -o host_linux host_linux.c src/kernel/tz.c -static -lm
# Reference compiler: gcc build of compiler/cc.c as of build/ref/COMMIT (byte-identity reference).
# The git show path is the HISTORIC tree's path (pre src/ move at that commit): do not prefix it.
ref:
	@mkdir -p build/ref
	@test -f build/ref/COMMIT || echo e4fb427 > build/ref/COMMIT
	git show $$(cat build/ref/COMMIT):compiler/cc.c > build/ref/cc.c
	gcc -O2 -w -I . -o build/ref/cc build/ref/cc.c
# the reference compiler for check_ref.py, built once (make ref rebuilds it)
build/ref/cc:
	$(MAKE) ref
check: build/host/nm build/host/ar build/host/objdump build/host/as build/host/ld $(CC_HOST) build/ref/cc
	python3 tests/obj/mkobj.py build/obj/sample.o
	build/host/nm build/obj/sample.o | diff - tests/obj/sample.expected
	@# bad objects: exit status 1 and the exact error message
	@for f in bad-magic bad-trunc bad-name bad-count; do \
	  out=$$(build/host/nm build/obj/$$f.o); st=$$?; \
	  if [ $$st -ne 1 ] || [ "$$out" != "nm: build/obj/$$f.o: not a T2DOS object" ]; then echo "FAIL $$f: status $$st, output '$$out'"; exit 1; fi; \
	done
	@out=$$(build/host/nm build/obj/missing.o); st=$$?; \
	  if [ $$st -ne 1 ] || [ "$$out" != "nm: cannot read build/obj/missing.o" ]; then echo "FAIL missing: status $$st, output '$$out'"; exit 1; fi
	@echo bad-object checks ok
	python3 tools/check_asm.py tests/asm/core.s tests/asm/fpu.s
	python3 tools/check_asm.py --relocs tests/asm/reloc.s | diff - tests/asm/reloc.expected
	test "$$(python3 tools/check_asm.py --inits tests/asm/reloc.s | tr '\n' ' ')" = ".Linit lfun "
	python3 tools/check_asm.py --errors tests/asm/errors/*.s
	@# randomized differential against GNU as (seeded): what as accepts, GNU as assembles to the same bytes
	python3 tools/check_asm.py --diff 3000
	python3 tools/check_link.py --pc
	@# objdump: exact dumps of the sample object and of reloc.s's object, errors, equality with GNU objdump
	build/host/objdump build/obj/sample.o | diff - tests/obj/objdump_sample.expected
	build/host/as -o build/obj/reloc.o tests/asm/reloc.s
	build/host/objdump build/obj/reloc.o | diff - tests/obj/objdump_reloc.expected
	@out=$$(build/host/objdump build/obj/bad-magic.o); st=$$?; \
	  if [ $$st -ne 1 ] || [ "$$out" != "objdump: build/obj/bad-magic.o: not a T2DOS object" ]; then echo "FAIL objdump bad-magic: status $$st, output '$$out'"; exit 1; fi
	@out=$$(build/host/objdump build/obj/missing.o); st=$$?; \
	  if [ $$st -ne 1 ] || [ "$$out" != "objdump: cannot read build/obj/missing.o" ]; then echo "FAIL objdump missing: status $$st, output '$$out'"; exit 1; fi
	@echo objdump error checks ok
	python3 tools/check_asm.py --objdump-corpus
	@# cc (driving as and ld) against the reference compiler (make ref): identical .bin files
	python3 tools/check_ref.py
	@# the cc driver: multi-file programs, assembly and C mixed, archives, pipeline errors
	python3 tools/check_toolchain.py --pc
	@# the tz DST engine: native unit tests of src/kernel/tz.c
	gcc -Wall -o build/host/tz_test tests/tz/test_tz.c && build/host/tz_test
.PHONY: ref check
clean:
	rm -rf $(CC_HOST) host_linux root/bin/*.bin build/s
