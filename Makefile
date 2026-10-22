LIB_TARGET_64  = libaudio.a
PLAY_TARGET_64 = play

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
CC 	:= x86_64-elf-gcc
AR 	:= x86_64-elf-ar
else
CC 	:= gcc
AR 	:= ar
endif

ELF_BIN_DIR_64 = bin
ELF_OBJ_DIR_64 = $(ELF_BIN_DIR_64)/obj

LIB_DIR = sysroot/lib
LIBC = $(LIB_DIR)/blibc.a
CRT0 = $(LIB_DIR)/start.o

BLIBC_REPO = https://github.com/Bleed-Kernel/blibc.git
BLIBC_DIR  ?= $(if $(wildcard ../blibc/include/bleed),../blibc,external/blibc)

COMMON_CFLAGS = \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-Wall -Wextra \
	-O2 \
	-nostdlib \
	-no-pie \
	-m64 \
	-Iinclude \
	-Isysroot/include \
	-Isysroot/include/bleed \
	-nostdinc

LDFLAGS = \
	-static \
	-nostdlib \
	-no-pie \
	-m64 \
	-L$(LIB_DIR)

SRCS      = $(shell find src -name '*.c')
PLAY_SRCS = examples/play.c

LIB_OBJS_64  = $(patsubst %.c,$(ELF_OBJ_DIR_64)/%.o,$(SRCS))
PLAY_OBJS_64 = $(patsubst %.c,$(ELF_OBJ_DIR_64)/%.o,$(PLAY_SRCS))

LIBAUDIO = $(ELF_BIN_DIR_64)/$(LIB_TARGET_64)

all: $(LIBAUDIO) $(ELF_BIN_DIR_64)/$(PLAY_TARGET_64)

$(LIBAUDIO): $(LIB_OBJS_64)
	@mkdir -p $(ELF_BIN_DIR_64)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS_64)

$(ELF_BIN_DIR_64)/$(PLAY_TARGET_64): $(PLAY_OBJS_64) $(LIBAUDIO)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) $(PLAY_OBJS_64) $(LIBAUDIO) $(LIBC)

$(ELF_OBJ_DIR_64)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

-include $(LIB_OBJS_64:.o=.d) $(PLAY_OBJS_64:.o=.d)

$(LIB_OBJS_64) $(PLAY_OBJS_64): | blibc

# a sibling checkout is used as it is, only our own clone gets pulled. synced every build so
# the sysroot never goes stale against it
blibc:
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	elif [ "$(BLIBC_DIR)" = "external/blibc" ]; then \
		cd $(BLIBC_DIR) && git pull --ff-only; \
	fi
	@$(MAKE) -C $(BLIBC_DIR)
	@mkdir -p sysroot
	@cp -r $(BLIBC_DIR)/sysroot/* sysroot/

# straight into the kernels initrd, then `make repack` there
install: all
	COPYFILE_DISABLE=1 cp $(ELF_BIN_DIR_64)/$(PLAY_TARGET_64) ../Bleed-Kernel/initrd/bin/

build: all

clean:
	rm -rf bin

distclean:
	rm -rf bin sysroot external

.PHONY: all build clean distclean blibc install
