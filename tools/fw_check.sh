#!/bin/bash
# The firmware built from source, in the simulator. Stops at the first failure.
# fw_check.sh --no-dump also repeats the image build with dumps/ and analysis/ unreadable (macOS sandbox-exec).
set -eo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD; export REPO=$ROOT
IMG=bootloader/build-dump/bootflash-fw.bin
if [ "$1" = --no-dump ]; then
    P="(version 1)(allow default)(deny file-read* (subpath \"$ROOT/dumps\") (subpath \"$ROOT/analysis\"))"
    sandbox-exec -p "$P" make -s -C bootloader DUMP=1 -B fwimage | tail -1
    if sandbox-exec -p "$P" cat dumps/bootflash.bin >/dev/null 2>&1; then echo "the sandbox did not hide the dump"; exit 1; fi
else
    make -s -C bootloader DUMP=1 fwimage | tail -1
fi
cmake -S simulator -B build/simulator >/dev/null && cmake --build build/simulator -j 8 --target fw_run fw_reboot fw_umframe >/dev/null
echo "== the apps as the Studio draws them, frame for frame"
frontend/packages/sign-emu/test/make_appvm_dump.sh | tail -1
(cd frontend/packages/sign-emu && node --test test/appvm_firmware.test.ts 2>&1 | grep -E "starts at|^. (pass|fail)")
echo "== UMFRAME delivery on RS-485 and the DB9"
for ch in 1 0; do build/simulator/fw_umframe $IMG $ch /dev/null > build/fw_check.txt; cat build/fw_check.txt; grep -q PASS build/fw_check.txt; done
echo "== crash report"
A=$(/opt/homebrew/opt/llvm/bin/llvm-nm bootloader/build-dump/fw/app.elf | awk '$3=="cmd_info"{print $1}')
UMAMI_HOOKS=1 build/simulator/fw_run $IMG /dev/null 100000000 "cmd:mw $A 0" cmd:info > build/fw_check.txt
grep -E "^vector" build/fw_check.txt | cut -c1-70
grep -q "pc $A" build/fw_check.txt
echo "== the store across a power cycle; an update boots from the slot"
make -s -C firmware/app B=$ROOT/build/fw-slot VERSION=UM-slot >/dev/null
venv/bin/python firmware/app/mkimg.py bootloader/build-dump/bootstrap.bin build/fw-slot/app.bin build/fw-slot/image.bin emoji.bin=firmware/emoji/16/emoji.bin >/dev/null
build/simulator/fw_reboot $IMG build/fw-slot/image.bin build/text.mqap /dev/null > build/fw_check.txt
grep -E "^firmware UM-slot|^file app0" build/fw_check.txt | sort -u
grep -q "^firmware UM-slot" build/fw_check.txt
echo "FIRMWARE CHECKS PASSED"
