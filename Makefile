# T2DOS build: cross compiler on the PC, userland binaries into an SD card image tree (root/)
CC_HOST = compiler/cc
PROGS = $(patsubst userland/%.c,root/bin/%.bin,$(wildcard userland/*.c))
BINUTILS = nm
BIN_BINS = $(patsubst %,root/bin/%.bin,$(BINUTILS))
HOST_BINS = $(patsubst %,build/host/%,$(BINUTILS))
all: $(CC_HOST) root/bin/cc.bin $(PROGS) $(BIN_BINS) build/host/as root/bin/as.bin build/host/ld root/bin/ld.bin root/lib/sys.h root/lib/net.c root/lib/t2obj.h root/lib/t2obj.c
# gcc finds sys.h through -I .; T2CC resolves it against the current directory (the repo root)
$(CC_HOST): compiler/cc.c sys.h
	gcc -O2 -w -I . -o $@ compiler/cc.c
root/bin/cc.bin: $(CC_HOST) compiler/cc.c sys.h
	@mkdir -p root/bin root/lib root/src
	./$(CC_HOST) -o $@ compiler/cc.c
root/bin/%.bin: userland/%.c $(CC_HOST) sys.h lib/net.c
	@mkdir -p root/bin
	./$(CC_HOST) -I lib -o $@ $<
# binutils: host builds for the PC (tests) and board builds with compiler/cc
build/host/%: binutils/%.c lib/t2obj.h lib/t2obj.c
	@mkdir -p build/host
	gcc -O2 -w -I lib -I . -o $@ $<
root/bin/%.bin: binutils/%.c $(CC_HOST) sys.h lib/t2obj.h lib/t2obj.c
	@mkdir -p root/bin
	./$(CC_HOST) -I lib -o $@ $<
# the assembler: same pattern, source in assembler/
build/host/as: assembler/as.c lib/t2obj.h lib/t2obj.c
	@mkdir -p build/host
	gcc -O2 -w -I lib -I . -o $@ assembler/as.c
root/bin/as.bin: assembler/as.c $(CC_HOST) sys.h lib/t2obj.h lib/t2obj.c
	@mkdir -p root/bin
	./$(CC_HOST) -I lib -o $@ assembler/as.c
# the linker: same pattern, source in linker/
build/host/ld: linker/ld.c lib/t2obj.h lib/t2obj.c
	@mkdir -p build/host
	gcc -O2 -w -I lib -I . -o $@ linker/ld.c
root/bin/ld.bin: linker/ld.c $(CC_HOST) sys.h lib/t2obj.h lib/t2obj.c
	@mkdir -p root/bin
	./$(CC_HOST) -I lib -o $@ linker/ld.c
root/lib/t2obj.h: lib/t2obj.h
	@mkdir -p root/lib && cp lib/t2obj.h root/lib/
root/lib/t2obj.c: lib/t2obj.c
	@mkdir -p root/lib && cp lib/t2obj.c root/lib/
root/lib/net.c: lib/net.c
	@mkdir -p root/lib && cp lib/net.c root/lib/
root/lib/sys.h: sys.h
	@mkdir -p root/lib && cp sys.h root/lib/
# Linux/ARM harness: make host && printf 'ls\n' | qemu-arm ./host_linux -r root /bin/sh.bin
host: host_linux.c sys.h
	arm-linux-gnueabihf-gcc -O1 -U_FORTIFY_SOURCE -mthumb -w -o host_linux host_linux.c -static
# Reference compiler: gcc build of compiler/cc.c at the commit in build/ref/COMMIT (byte-identity reference)
ref:
	@mkdir -p build/ref
	@test -f build/ref/COMMIT || echo 040b630 > build/ref/COMMIT
	git show $$(cat build/ref/COMMIT):compiler/cc.c > build/ref/cc.c
	gcc -O2 -w -I . -o build/ref/cc build/ref/cc.c
check: build/host/nm build/host/as build/host/ld
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
	python3 tools/check_link.py --pc
.PHONY: ref check
clean:
	rm -rf $(CC_HOST) host_linux root/bin/*.bin
