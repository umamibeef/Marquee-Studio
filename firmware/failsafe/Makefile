# The boot flash chip's image: our boot loader and the failsafe (main.c). No dump is read.
LLVM ?= /opt/homebrew/opt/llvm/bin
LLD  ?= /opt/homebrew/opt/lld/bin
CC    = $(LLVM)/clang
LD    = $(LLD)/ld.lld
OBJCOPY = $(LLVM)/llvm-objcopy
PYTHON ?= ../../venv/bin/python
TARGET = --target=powerpc-unknown-eabi -mcpu=ppc
CFLAGS = $(TARGET) -Os -ffreestanding -fno-builtin -nostdlib -msoft-float -fno-pic -std=c11 -ffunction-sections -fdata-sections \
         -Wall -Wextra -I. -I../app -I../../bootloader/include -I$(B)
BOOT ?= ../../bootloader/build-dump/bootstrap.bin
B    ?= ../../build/failsafe
FONT  = ../../third_party/fonts/spleen/spleen-8x16.bdf
OBJS  = $(B)/start.o $(B)/hw.o $(B)/rt.o $(B)/main.o

all: $(B)/bootflash-failsafe.bin
$(B):
	mkdir -p $(B)
$(B)/failsafe_font.h: ../../tools/gen_failsafe_font.py $(FONT) | $(B)
	$(PYTHON) ../../tools/gen_failsafe_font.py $(FONT) $@
$(B)/start.o: ../app/start.S | $(B)
	$(CC) $(TARGET) -c $< -o $@
$(B)/hw.o: ../app/hw.c ../app/hw.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/rt.o: ../app/rt.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/main.o: main.c $(B)/failsafe_font.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/failsafe.elf: $(OBJS) ../app/app.ld
	$(LD) -T ../app/app.ld --gc-sections -nostdlib -static $(OBJS) -o $@
$(B)/failsafe.bin: $(B)/failsafe.elf
	$(OBJCOPY) -O binary $< $@
$(B)/bootflash-failsafe.bin: $(B)/failsafe.bin $(BOOT) ../app/mkimg.py
	$(PYTHON) ../app/mkimg.py $(BOOT) $(B)/failsafe.bin $@
	@ls -l $(B)/failsafe.bin $@ | awk '{print $$5, $$9}'
