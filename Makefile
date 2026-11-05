# T2DOS build: cross compiler on the PC, userland binaries into an SD card image tree (root/)
CC_HOST = compiler/cc
PROGS = $(patsubst userland/%.c,root/bin/%.bin,$(wildcard userland/*.c))
all: $(CC_HOST) root/bin/cc.bin $(PROGS) root/lib/sys.h root/lib/net.c
# gcc finds sys.h through -I .; T2CC resolves it against the current directory (the repo root)
$(CC_HOST): compiler/cc.c sys.h
	gcc -O2 -w -I . -o $@ compiler/cc.c
root/bin/cc.bin: $(CC_HOST) compiler/cc.c sys.h
	@mkdir -p root/bin root/lib root/src
	./$(CC_HOST) -o $@ compiler/cc.c
root/bin/%.bin: userland/%.c $(CC_HOST) sys.h lib/net.c
	@mkdir -p root/bin
	./$(CC_HOST) -I lib -o $@ $<
root/lib/net.c: lib/net.c
	@mkdir -p root/lib && cp lib/net.c root/lib/
root/lib/sys.h: sys.h
	@mkdir -p root/lib && cp sys.h root/lib/
# Linux/ARM harness: make host && printf 'ls\n' | qemu-arm ./host_linux -r root /bin/sh.bin
host: host_linux.c sys.h
	arm-linux-gnueabihf-gcc -O1 -U_FORTIFY_SOURCE -mthumb -w -o host_linux host_linux.c -static
clean:
	rm -rf $(CC_HOST) host_linux root/bin/*.bin
