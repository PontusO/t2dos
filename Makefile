# T2DOS build: cross compiler on the PC, userland binaries into an SD card image tree (root/)
CC_HOST = compiler/cc
PROGS = $(patsubst userland/%.c,root/bin/%.bin,$(wildcard userland/*.c))
BINUTILS = nm
BIN_BINS = $(patsubst %,root/bin/%.bin,$(BINUTILS))
HOST_BINS = $(patsubst %,build/host/%,$(BINUTILS))
all: $(CC_HOST) root/bin/cc.bin $(PROGS) $(BIN_BINS) root/lib/sys.h root/lib/net.c root/lib/t2obj.h root/lib/t2obj.c
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
check: build/host/nm
	python3 tests/obj/mkobj.py build/obj/sample.o
	build/host/nm build/obj/sample.o | diff - tests/obj/sample.expected
.PHONY: ref check
clean:
	rm -rf $(CC_HOST) host_linux root/bin/*.bin
