ROOT := $(abspath .)

CACTLIB ?= $(abspath ../CactLibc-x86_32)
CACTSOLEINC ?= $(abspath ../Cactsole-x86_32/include)
LR_BIN ?= $(abspath ../LocalRepoCactOS-x86_32/lib/bin)
LR_SBIN ?= $(abspath ../LocalRepoCactOS-x86_32/lib/sbin)

_ACTIVE := $(filter-out clean,$(or $(MAKECMDGOALS),all))

ifneq ($(_ACTIVE),)
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
          -ffunction-sections -fdata-sections \
          -I$(CACTSOLEINC) -I$(CACTLIB)/include -Wall -Wextra

LDFLAGS := -m elf_i386 -pie --dynamic-linker=/lib/ld.so --hash-style=both \
           -nostdlib --gc-sections -T $(ROOT)/link.ld

COMMON_C := $(wildcard $(ROOT)/common/ex_*.c)
COMMON_O := $(COMMON_C:.c=.o)

BUILDD := $(ROOT)/build/bin

APPS := pwd ls mkdir rmdir tch rm cat wrt stat mv ln readlink \
        clear date uptime kill su sleep free fetch modload modunload run \
        echo true false whoami id chmod chown version \
        nconn net ping dhcp dns

SBIN_APPS := kill su modload modunload ping dhcp dns
BIN_APPS  := $(filter-out $(SBIN_APPS),$(APPS))

BINS := $(patsubst %,$(BUILDD)/%,$(APPS))

.PHONY: all clean install userbins

all: $(LIBC_SO) $(START_O) $(COMMON_O) $(BINS)

$(LIBC_SO) $(START_O):
	@test -f $(LIBC_SO) && test -f $(START_O) || (echo >&2 "Missing libc — build libc first (CACTLIB=$(CACTLIB))"; exit 1)

$(ROOT)/common/%.o: $(ROOT)/common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%/main.o: %/main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDD):
	mkdir -p $(BUILDD)

$(BUILDD)/%: %/main.o $(COMMON_O) $(START_O) $(LIBC_SO) | $(BUILDD)
	$(LD) $(LDFLAGS) $(START_O) $< $(COMMON_O) $(LIBC_SO) -o $@

install: all
	@mkdir -p $(LR_BIN) $(LR_SBIN)
	@for f in $(SBIN_APPS); do rm -f $(LR_BIN)/$$f; done
	cp -f $(patsubst %,$(BUILDD)/%,$(BIN_APPS)) $(LR_BIN)/
	cp -f $(patsubst %,$(BUILDD)/%,$(SBIN_APPS)) $(LR_SBIN)/

userbins: install

clean:
	rm -f $(COMMON_O) $(patsubst %,%/main.o,$(APPS)) $(BINS)
	rm -rf $(BUILDD)
