ROOT := $(abspath .)

CACTLIB ?= $(abspath ../CactLibc-x86_32)
CACTSOLEINC ?= $(abspath ../Cactsole-x86_32/include)
LR_BIN ?= $(abspath ../LocalRepoCactOS-x86_32/lib/bin)
LR_SBIN ?= $(abspath ../LocalRepoCactOS-x86_32/lib/sbin)

_ACTIVE := $(filter-out clean,$(or $(MAKECMDGOALS),all))
ifneq ($(filter-out clean test,$(or $(MAKECMDGOALS),all)),)
ifndef CACTLIB
$(error Set CACTLIB to the libc project root)
endif
ifndef CACTSOLEINC
$(error Set CACTSOLEINC to the shell headers directory (include/))
endif
ifndef LR_BIN
$(error Set LR_BIN to the staging bin directory for install)
endif
ifndef LR_SBIN
$(error Set LR_SBIN to the staging sbin directory for install)
endif
endif

CC      := gcc
LD      := ld
START_O := $(CACTLIB)/build/pic/start.o
LIBC_SO := $(CACTLIB)/clibc.so

CFLAGS := -m32 -ffreestanding -fPIE -fno-stack-protector -nostdlib \
          -ffunction-sections -fdata-sections -DCACTOS_TARGET \
          -I$(ROOT) -I$(CACTSOLEINC) -I$(CACTLIB)/include -Wall -Wextra

LDFLAGS := -m elf_i386 -pie --dynamic-linker=/lib/ld.so --hash-style=both \
           -nostdlib --gc-sections -T $(ROOT)/link.ld

COMMON_C := $(wildcard $(ROOT)/common/ex_*.c)
COMMON_O := $(COMMON_C:.c=.o)

BUILDD := $(ROOT)/build/bin

APPS := pwd ls mkdir rmdir tch rm cat wrt stat mv ln readlink \
        clear date uptime kill su sleep free fetch modload modunload run \
        echo true false whoami id chmod chown version \
        nconn net ping dhcp dns ip uxtest \
        nc wget dd df grep \
        fdisk mkfs.ext4 mkfs.fat32 cact-rootfs

SBIN_APPS := kill su modload modunload ping dhcp dns ip \
             dd df \
             fdisk mkfs.ext4 mkfs.fat32 cact-rootfs
BIN_APPS  := $(filter-out $(SBIN_APPS),$(APPS))

BINS := $(patsubst %,$(BUILDD)/%,$(APPS))

# Extra sources compiled into the disk/fs tools (pure format logic).
DISK_EXTRA := fdisk/ptab.o mkfs.ext4/ext4_fmt.o mkfs.fat32/fat32_fmt.o

.PHONY: all clean install userbins test

all: $(LIBC_SO) $(START_O) $(COMMON_O) $(BINS)

$(LIBC_SO) $(START_O):
	@test -f $(LIBC_SO) && test -f $(START_O) || (echo >&2 "Missing libc — build libc first (CACTLIB=$(CACTLIB))"; exit 1)

$(ROOT)/common/%.o: $(ROOT)/common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(DISK_EXTRA): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%/main.o: %/main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDD):
	mkdir -p $(BUILDD)

# one-ELF-per-tool default link (links shared common/ objects)
$(BUILDD)/%: %/main.o $(COMMON_O) $(START_O) $(LIBC_SO) | $(BUILDD)
	$(LD) $(LDFLAGS) $(START_O) $< $(COMMON_O) $(LIBC_SO) -o $@

# disk/fs tools: dedicated link lines (no common/ objects)
$(BUILDD)/fdisk: fdisk/main.o fdisk/ptab.o $(START_O) $(LIBC_SO) | $(BUILDD)
	$(LD) $(LDFLAGS) $(START_O) fdisk/main.o fdisk/ptab.o $(LIBC_SO) -o $@

$(BUILDD)/mkfs.ext4: mkfs.ext4/main.o mkfs.ext4/ext4_fmt.o $(START_O) $(LIBC_SO) | $(BUILDD)
	$(LD) $(LDFLAGS) $(START_O) mkfs.ext4/main.o mkfs.ext4/ext4_fmt.o $(LIBC_SO) -o $@

$(BUILDD)/mkfs.fat32: mkfs.fat32/main.o mkfs.fat32/fat32_fmt.o $(START_O) $(LIBC_SO) | $(BUILDD)
	$(LD) $(LDFLAGS) $(START_O) mkfs.fat32/main.o mkfs.fat32/fat32_fmt.o $(LIBC_SO) -o $@

install: all
	@mkdir -p $(LR_BIN) $(LR_SBIN)
	@for f in $(SBIN_APPS); do rm -f $(LR_BIN)/$$f; done
	cp -f $(patsubst %,$(BUILDD)/%,$(BIN_APPS)) $(LR_BIN)/
	cp -f $(patsubst %,$(BUILDD)/%,$(SBIN_APPS)) $(LR_SBIN)/

userbins: install

# ---- host-side test binaries (plain gcc -m32, no Cact libc) ----------------
HOST_CFLAGS := -m32 -O2 -Wall -Wno-unused-function -Wno-unused-variable
HOSTDIR := $(ROOT)/build/host
HOST_TOOLS := $(HOSTDIR)/fdisk $(HOSTDIR)/mkfs.ext4 \
              $(HOSTDIR)/mkfs.fat32 $(HOSTDIR)/cact-rootfs

$(HOSTDIR)/fdisk: fdisk/main.c fdisk/ptab.c | $(HOSTDIR)
	$(CC) $(HOST_CFLAGS) $^ -o $@

$(HOSTDIR)/mkfs.ext4: mkfs.ext4/main.c mkfs.ext4/ext4_fmt.c | $(HOSTDIR)
	$(CC) $(HOST_CFLAGS) $^ -o $@

$(HOSTDIR)/mkfs.fat32: mkfs.fat32/main.c mkfs.fat32/fat32_fmt.c | $(HOSTDIR)
	$(CC) $(HOST_CFLAGS) $^ -o $@

$(HOSTDIR)/cact-rootfs: cact-rootfs/main.c | $(HOSTDIR)
	$(CC) $(HOST_CFLAGS) $^ -o $@

$(HOSTDIR):
	mkdir -p $(HOSTDIR)

test: $(HOST_TOOLS)
	@PATH="$(HOSTDIR):$$PATH" tests/run_tests.sh

clean:
	rm -f $(COMMON_O) $(patsubst %,%/main.o,$(APPS)) $(DISK_EXTRA) $(BINS)
	rm -rf $(BUILDD) $(HOSTDIR)
