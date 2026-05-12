# CactUserBins-x86_32 — отдельный ELF на утилиту; общие реализации в common/.
# Линкуем все common/*.o + --gc-sections: лишние entrypoints выкидываются.

ROOT        := $(abspath .)
CACTLIB     := $(abspath ../CactLib-x86_32)
CACTSOLEINC := $(abspath ../Cactsole-x86_32/include)

CC      := gcc
LD      := ld
START_O := $(CACTLIB)/build/pic/start.o
LIBC_SO := $(CACTLIB)/libc.so

CFLAGS := -m32 -ffreestanding -fPIE -fno-stack-protector -nostdlib \
          -ffunction-sections -fdata-sections \
          -I$(CACTSOLEINC) -I$(CACTLIB)/include -Wall -Wextra

LDFLAGS := -m elf_i386 -pie --no-dynamic-linker --hash-style=both \
           -nostdlib --gc-sections -T $(ROOT)/link.ld

COMMON_C := $(wildcard $(ROOT)/common/ex_*.c)
COMMON_O := $(COMMON_C:.c=.o)

BUILDD := $(ROOT)/build/bin

APPS := pwd ls mkdir rmdir tch rm cat wrt stat mv ln readlink \
        clear date uptime kill su sleep free fetch modload modunload run \
        echo true false whoami id chmod chown version \
        nconn net ping dhcp dns

# FHS-style split: cctkfs /sbin/* (sbinfs) vs /bin/* (binfs)
SBIN_APPS := kill su modload modunload ping dhcp dns
BIN_APPS  := $(filter-out $(SBIN_APPS),$(APPS))

BINS := $(patsubst %,$(BUILDD)/%,$(APPS))

.PHONY: all clean install userbins

all: $(LIBC_SO) $(START_O) $(COMMON_O) $(BINS)

$(LIBC_SO) $(START_O):
	$(MAKE) -C $(CACTLIB)

$(ROOT)/common/%.o: $(ROOT)/common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%/main.o: %/main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDD):
	mkdir -p $(BUILDD)

$(BUILDD)/%: %/main.o $(COMMON_O) $(START_O) $(LIBC_SO) | $(BUILDD)
	$(LD) $(LDFLAGS) $(START_O) $< $(COMMON_O) $(LIBC_SO) -o $@

LR_BIN  := $(abspath ../LocalRepoCactOS/lib/bin)
LR_SBIN := $(abspath ../LocalRepoCactOS/lib/sbin)

install: all
	@mkdir -p $(LR_BIN) $(LR_SBIN)
	@for f in $(SBIN_APPS); do rm -f $(LR_BIN)/$$f; done
	cp -f $(patsubst %,$(BUILDD)/%,$(BIN_APPS)) $(LR_BIN)/
	cp -f $(patsubst %,$(BUILDD)/%,$(SBIN_APPS)) $(LR_SBIN)/

# Совместимость с LocalRepoCactOS (цель userbins)
userbins: install

clean:
	rm -f $(COMMON_O) $(patsubst %,%/main.o,$(APPS)) $(BINS)
	rm -rf $(BUILDD)
