# The Umami marquee firmware, built from source. Inputs: this tree, firmware/ext, the content files named on mkimg's
# command line, and third_party/zlib-1.1.4. No vendor dump is read anywhere in this build.
LLVM    ?= /opt/homebrew/opt/llvm/bin
LLD     ?= /opt/homebrew/opt/lld/bin
ZLIB    ?= ../../third_party/zlib-1.1.4
VERSION ?= UM-$(shell date +%Y%m%d-%H%M)
CC       = $(LLVM)/clang
LD       = $(LLD)/ld.lld
OBJCOPY  = $(LLVM)/llvm-objcopy
TARGET   = --target=powerpc-unknown-eabi -mcpu=ppc
BASE     = $(TARGET) -Os -ffreestanding -fno-builtin -nostdlib -msoft-float -fno-pic
CFLAGS   = $(BASE) -MMD -MP -std=c11 -ffunction-sections -fdata-sections -Wall -Wextra -I. -I../../bootloader/include -I../ext -DFW_VERSION='"$(VERSION)"'
ZFLAGS   = $(BASE) -std=gnu89 -DSTDC -DNO_ERRNO_H -w -Ilibc -I$(ZLIB)
B       ?= ../../build/fw

APP   = core hw serial umframe delivery rt
EXT   = apps console store update crash dbg fonts native_emoji ebpf gdbstub
ENG   = $(patsubst engine/%.c,%,$(wildcard engine/*.c))
ZOBJ  = adler32 crc32 infblock infcodes inffast inflate inftrees infutil uncompr zutil
OBJS  = $(B)/start.o $(APP:%=$(B)/%.o) $(EXT:%=$(B)/x_%.o) $(ENG:%=$(B)/e_%.o) $(ZOBJ:%=$(B)/z_%.o)

ifeq ($(wildcard fpga_bits.h),)
$(error fpga_bits.h is missing. It stays out of git: see tools/extract_fpga_bits.py)
endif

all: $(B)/app.bin
$(B):
	mkdir -p $(B)
$(B)/start.o: start.S | $(B)
	$(CC) $(TARGET) -c $< -o $@
$(B)/%.o: %.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/x_%.o: ../ext/%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/e_%.o: engine/%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/z_%.o: $(ZLIB)/%.c | $(B)
	$(CC) $(ZFLAGS) -c $< -o $@
$(B)/app.elf: $(OBJS) app.ld
	$(LD) -T app.ld --gc-sections -nostdlib -static $(OBJS) -o $@
$(B)/app.bin: $(B)/app.elf
	$(OBJCOPY) -O binary $< $@
	@echo "app.bin: $$(wc -c < $@) bytes"
-include $(wildcard $(B)/*.d)
