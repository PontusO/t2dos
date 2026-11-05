# T2DOS build: cross compiler on the PC, userland binaries into an SD card image tree (root/)
PROGS = $(patsubst userland/%.c,root/bin/%.bin,$(wildcard userland/*.c))
all: cc root/bin/cc.bin $(PROGS) root/lib/sys.h root/lib/net.c
cc: cc.c sys.h
	gcc -O2 -w -o cc cc.c
root/bin/cc.bin: cc cc.c sys.h
	@mkdir -p root/bin root/lib root/src
	./cc -o $@ cc.c
root/bin/%.bin: userland/%.c cc sys.h lib/net.c
	@mkdir -p root/bin
	./cc -I lib -o $@ $<
root/lib/net.c: lib/net.c
	@mkdir -p root/lib && cp lib/net.c root/lib/
root/lib/sys.h: sys.h
	@mkdir -p root/lib && cp sys.h root/lib/
# Linux/ARM harness: make host && printf 'ls\n' | qemu-arm ./host_linux -r root /bin/sh.bin
host: host_linux.c sys.h
	arm-linux-gnueabihf-gcc -O1 -U_FORTIFY_SOURCE -mthumb -w -o host_linux host_linux.c -static
clean:
	rm -rf cc host_linux root/bin/*.bin
