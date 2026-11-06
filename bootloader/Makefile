# BOOTSTRAP V2.00, rebuilt from C.
#   make            build build/bootstrap.elf and build/bootstrap.bin (faithful rebuild)
#   make NET=1      same plus the Ethernet network window (lwIP), in build-net/
#   make PATCH=1    same plus applying a "PATCH" archive entry (colour-emoji code) to the app in RAM;
#                   combine with NET=1 if wanted. Output in build[-net]-patch/
#   make DUMP=1     same plus the serial dump window on the DB9 (src/serdump.c); combines with
#                   NET=1 and PATCH=1. Output in build[-net]-dump[-patch]/
#   make image      <build>/bootflash.bin: new bootstrap + the original archive (from ORIG)
#   make test       host test: archive walk + NRV2D against the real dump
#   make cosim      run the original and the rebuild side by side in an emulator, compare
#   make netsim     NET=1 image on an emulated board with an emulated LAN (ARP, ping, TFTP)
#   make serdumpsim DUMP=1 image on an emulated board with an emulated DB9 UART and host
#   make tables     regenerate src/tables.c (tools/gen_tables_src.py; the dump is not read)

LLVM    ?= /opt/homebrew/opt/llvm/bin
LLD     ?= /opt/homebrew/opt/lld/bin
CC       = $(LLVM)/clang
LD       = $(LLD)/ld.lld
OBJCOPY  = $(LLVM)/llvm-objcopy
OBJDUMP  = $(LLVM)/llvm-objdump
HOSTCC  ?= cc
PYTHON  ?= $(if $(wildcard ../venv/bin/python),$(abspath ../venv/bin/python),python3)
VPYTHON ?= ../venv/bin/python

ORIG    ?= ../dumps/bootflash.bin
DUMP    ?= 0
# older use: DUMP=<path of the original boot flash dump>
ifneq ($(filter-out 0 1,$(DUMP)),)
ORIG    := $(DUMP)
override DUMP := 0
endif
REF_APP ?= ../analysis/extracted/ELITE_RGB

NET     ?= 0
PATCH   ?= 0
TARGET   = --target=powerpc-unknown-eabi -mcpu=ppc
BASEFLAGS = $(TARGET) -Os -std=c11 -ffreestanding -fno-builtin -nostdlib \
          -msoft-float -fno-pic -fno-common -ffunction-sections -fdata-sections \
          -fno-delete-null-pointer-checks
CFLAGS  = $(BASEFLAGS) -Wall -Wextra -Iinclude \
          '-DNRV2D_POLL()=wdt_kick()' -include include/mpc8xx.h
ASFLAGS = $(TARGET)

SRCS = src/main.c src/timer.c src/i2c.c src/memmap.c src/sdram.c src/mmu.c \
       src/loader.c src/archive.c src/nrv2d.c src/tables.c

ifeq ($(NET),1)
B        = build-net
LDSCRIPT = link_net.ld
LWIP     = ../third_party/lwip/src
NETINC   = -Inet/include -isystem net/include/libc -Inet -I$(LWIP)/include
CFLAGS  += -DNET $(NETINC)
LWIP_CFLAGS = $(BASEFLAGS) -w -Iinclude $(NETINC)
NET_SRCS = net/netboot.c net/fec.c net/libc.c
LWIP_SRCS = core/init.c core/def.c core/inet_chksum.c core/ip.c core/mem.c core/memp.c \
            core/netif.c core/pbuf.c core/udp.c core/timeouts.c \
            core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c \
            core/ipv4/ip4_frag.c core/ipv4/dhcp.c netif/ethernet.c apps/tftp/tftp.c
NET_OBJS = $(NET_SRCS:net/%.c=$(B)/net_%.o) $(addprefix $(B)/lwip/,$(notdir $(LWIP_SRCS:.c=.o)))
else
B        = build
LDSCRIPT = link.ld
NET_OBJS =
endif

ifeq ($(DUMP),1)
B       := $(B)-dump
CFLAGS  += -DDUMP
ASFLAGS += -DDUMP
SRCS    += src/serdump.c src/mainflash.c src/dbg.c
endif

# STAGE1=1 (with DUMP=1): the same boot loader as stage 1 of the two-stage boot, linked for SDRAM (link_s1.ld) and
# installed in the main flash's loader sector over the DB9 (include/serdump.h). `make DUMP=1 STAGE1=1 stage1`.
STAGE1  ?= 0
ifeq ($(STAGE1),1)
B       := $(B)-s1
CFLAGS  += -DSTAGE1
ASFLAGS += -DSTAGE1
LDSCRIPT = link_s1.ld
endif

ifeq ($(PATCH),1)
B       := $(B)-patch
CFLAGS  += -DPATCH
SRCS    += src/patch.c
PATCH_ENTRY = $(B)/PATCH.entry
endif

OBJS = $(B)/start.o $(SRCS:src/%.c=$(B)/%.o)
LDFLAGS = -T $(LDSCRIPT) --gc-sections -nostdlib -static

all: $(B)/bootstrap.bin

$(B)/%.o: src/%.c include/*.h $(wildcard net/*.h net/include/*.h) | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/start.o: src/start.S | $(B)
	$(CC) $(ASFLAGS) -c $< -o $@

$(B)/net_%.o: net/%.c net/*.h net/include/*.h include/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# one rule per lwIP source, objects named after the file
define LWIP_RULE
$(B)/lwip/$(notdir $(1:.c=.o)): $(LWIP)/$(1) net/include/lwipopts.h net/include/arch/cc.h | $(B)
	@mkdir -p $(B)/lwip
	$(CC) $(LWIP_CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(LWIP_SRCS),$(eval $(call LWIP_RULE,$(f))))

$(B)/bootstrap.elf: $(OBJS) $(NET_OBJS) $(LDSCRIPT)
	@for o in $(OBJS); do \
	  $(OBJDUMP) -h $$o | awk -v f=$$o '$$2 ~ /^\.(s?data|s?bss)/ && $$2 !~ /rel\.ro/ && $$3 != "00000000" {print "error: " f " has writable data (" $$2 ")"; bad=1} END {exit bad}' || exit 1; \
	done
	$(LD) $(LDFLAGS) $(OBJS) $(NET_OBJS) -o $@ -Map $(B)/bootstrap.map

$(B)/bootstrap.bin: $(B)/bootstrap.elf
	$(OBJCOPY) -O binary --only-section=.text --only-section=.data $< $@
	@$(PYTHON) -c "import os;print('bootstrap: %d bytes (original 0x2684 = 9860)' % os.path.getsize('$@'))"

stage1: $(B)/stage1.img
$(B)/stage1.img: $(B)/bootstrap.bin tools/mks1.py
	$(PYTHON) tools/mks1.py $(B)/bootstrap.bin $@ --version "$(shell cat ../analysis/build/fw/ext/VERSION 2>/dev/null)-s1"

$(B)/bootstrap.dis: $(B)/bootstrap.elf
	$(OBJDUMP) -d --triple=powerpc-unknown-eabi $< > $@

image: $(B)/bootflash.bin
$(B)/bootflash.bin: $(B)/bootstrap.bin tools/mkimage.py $(ORIG) $(PATCH_ENTRY)
	$(PYTHON) tools/mkimage.py $(B)/bootstrap.bin $(ORIG) $@ $(if $(PATCH_ENTRY),--add $(PATCH_ENTRY))

# The firmware, in a boot-flash image, built from source (../firmware/app, ../firmware/ext, third_party/zlib): this boot
# loader, the app (NRV2D-compressed), the fonts of ../firmware/fonts.txt, the emoji and the start-up animation. No dump
# is read. With DUMP=1 it is <build>-dump/bootflash-fw.bin, what the Sign window installs. Writing it to the sign is a
# separate step (docs/FLASHING.md).
EMOJI_SIZE ?= 16
FW_BUILD = $(abspath $(B))/fw
FONT_NAMES = $(shell sed -e 's/\#.*//' ../firmware/fonts.txt | awk 'NF >= 2 {print $$1}')
FONT_FILES = $(foreach f,$(FONT_NAMES),$(f)=$(B)/fonts/$(f)) fonts.lst=$(B)/fonts/fonts.lst fonts.auto=$(B)/fonts/fonts.auto
# The sign's own stock fonts (fonte01..04), converted to .mqf (proportional grid) so our firmware loads them by name
# like any other font and draws them exactly as the Studio does. Same glyphs, new container; no dump is read at build.
STOCK_SRC   = ../studio/content/fonts
STOCK_FONTS = fonte01.png fonte02.png fonte03.png fonte04.png
STOCK_FILES = $(foreach f,$(STOCK_FONTS),$(f)=$(B)/fonts/$(f))
$(B)/fonts/.stamp: ../firmware/fonts.txt ../firmware/auto_fonts.txt ../marquee_pack/font_import.py ../tools/stock_to_mqf.py ../marquee_pack/mqf.py ../marquee_pack/sign_emu.py $(foreach f,$(STOCK_FONTS),$(STOCK_SRC)/$(f)) | $(B)
	rm -rf $(B)/fonts && cd .. && venv/bin/python -m marquee_pack.font_import --set firmware/fonts.txt $(abspath $(B))/fonts
	printf '%s\n' $(sort $(FONT_NAMES)) > $(B)/fonts/fonts.lst
	../venv/bin/python ../tools/stock_to_mqf.py $(STOCK_SRC) $(B)/fonts $(STOCK_FONTS)
	printf '%s\n' $(STOCK_FONTS) >> $(B)/fonts/fonts.lst
	grep -vE '^#|^$$' ../firmware/auto_fonts.txt > $(B)/fonts/fonts.auto
	touch $@
fwimage: $(B)/bootflash-fw.bin
.PHONY: fwapp
fwapp:
	$(MAKE) -C ../firmware/app B=$(FW_BUILD)
$(B)/bootflash-fw.bin: fwapp $(B)/bootstrap.bin ../firmware/app/mkimg.py tools/nrv2d_pack.py ../firmware/emoji/$(EMOJI_SIZE)/emoji.bin $(B)/fonts/.stamp | $(B)
	$(PYTHON) ../firmware/app/mkimg.py $(B)/bootstrap.bin $(FW_BUILD)/app.bin $@ emoji.bin=../firmware/emoji/$(EMOJI_SIZE)/emoji.bin $(FONT_FILES) $(STOCK_FILES)

# colour-emoji patch code (linked at 0xC60000) -> PATCH archive entry
EMOJI_FLAGS = $(TARGET) -O2 -ffreestanding -fno-builtin -nostdlib -msoft-float -fno-pic -Wall -Wextra
$(B)/emoji.elf: patch/hook.S patch/emoji.c patch/link.ld | $(B)
	$(CC) $(EMOJI_FLAGS) -c patch/hook.S -o $(B)/hook.o
	$(CC) $(EMOJI_FLAGS) -c patch/emoji.c -o $(B)/emoji.o
	$(LD) -T patch/link.ld $(B)/hook.o $(B)/emoji.o -o $@
$(B)/emoji.bin: $(B)/emoji.elf
	$(OBJCOPY) -O binary $< $@
$(B)/PATCH.entry: $(B)/emoji.bin ../tools/mkpatch.py
	$(PYTHON) ../tools/mkpatch.py $< $(REF_APP) $@

test: build/host_test
	./build/host_test $(ORIG) $(REF_APP)

build/host_test: test/host_test.c src/archive.c src/nrv2d.c include/*.h | build
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -o $@ test/host_test.c src/archive.c src/nrv2d.c

emojitest:
	$(MAKE) PATCH=1 emoji
	$(VPYTHON) test/test_colour_emoji.py $(REF_APP) build-patch/emoji.bin ../analysis/extracted/fonte04.png

emoji: $(B)/emoji.bin

cosim:
	$(MAKE) NET=0 DUMP=0 PATCH=0 image
	$(VPYTHON) test/cosim.py $(ORIG) build/bootflash.bin $(REF_APP)

netsim:
	$(MAKE) NET=1 DUMP=0 PATCH=0 image
	$(VPYTHON) test/netsim.py build-net/bootflash.bin $(ORIG) $(REF_APP)

serdumpsim:
	$(MAKE) NET=0 DUMP=0 PATCH=0 image
	$(MAKE) NET=0 DUMP=1 PATCH=0 image
	$(MAKE) NET=1 DUMP=1 PATCH=0 image
	$(VPYTHON) test/serdumpsim.py build-dump/bootflash.bin build/bootflash.bin \
	    build-net-dump/bootflash.bin $(ORIG) $(REF_APP)

# fwtest on a real board's configuration: EEPROM=DIR holding eeprom_a0.bin / eeprom_a2.bin (tools/saleae_eeprom.py).
# The firmware then loads the panel FPGA (the simulator's fake one) and draws at the board's own size; the stock
# app's loading progress bars (rows 12-15) move with the image's file list, so they are left out.
fwtest-real: $(B)/bootflash-fw.bin
	cmake -S ../simulator -B ../build/simulator >/dev/null && cmake --build ../build/simulator --target umami-marquee-sim-test >/dev/null
	EEPROM_A0=$(EEPROM)/eeprom_a0.bin EEPROM_A2=$(EEPROM)/eeprom_a2.bin MASK_ROWS=12-15 $(if $(EXT_VERSION),FW_VERSION=$(EXT_VERSION) )\
	    $(SIMTEST) flashtest $(B)/bootflash-fw.bin $(FW_APP) 200000000 200000000

# permanent updates: INSTALL / UNINSTALL into the update slot and the boot from it, on a main-flash command model
installsim:
	$(MAKE) DUMP=1 fwimage
	$(MAKE) DUMP=1 STAGE1=1 stage1
	$(VPYTHON) test/installsim.py build-dump/bootflash-fw.bin ../analysis/build/fw/ELITE_RGB-ext $(ORIG) ../analysis/extracted/ELITE_RGB build-dump-s1/stage1.img

# run from RAM: the DUMP=1 firmware image on the chip, the stock dump staged over serial and booted from RAM
runsim:
	$(MAKE) DUMP=1 fwimage
	$(VPYTHON) test/runsim.py build-dump/bootflash-fw.bin ../analysis/build/fw/ELITE_RGB-ext $(ORIG) ../analysis/extracted/ELITE_RGB

tables:
	$(PYTHON) tools/gen_tables_src.py > src/tables.c

$(B):
	mkdir -p $(B)

clean:
	rm -rf build build-net build-patch build-net-patch build-dump build-net-dump \
	       build-dump-patch build-net-dump-patch

.PHONY: runsim all image fwimage fwtest test cosim netsim serdumpsim emojitest emoji tables clean
