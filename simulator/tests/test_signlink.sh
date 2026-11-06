#!/bin/sh
# The Studio's sign link against an emulated sign: the DUMP=1 image boots on the emulated board behind a pty
# (tools/simsign_pty.py); sign-link-run stages the stock dump over it and RUNs it; the board must then boot the
# stock app from RAM. Usage: simulator/tests/test_signlink.sh [BUILD_DIR]   (from the repository root)
set -e
repo=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-$repo/build/simulator}
PY=$repo/venv/bin/python
tmp=$(mktemp -d)
trap 'kill $bridge 2>/dev/null; rm -rf "$tmp"' EXIT
"$PY" "$repo/tools/simsign_pty.py" "$repo/bootloader/build-dump/bootflash-fw.bin" \
    --app stock="$repo/analysis/extracted/ELITE_RGB" --app ours="$repo/analysis/build/fw/ELITE_RGB-ext" \
    --pty-file "$tmp/pty" > "$tmp/bridge.log" 2>&1 &
bridge=$!
while [ ! -s "$tmp/pty" ]; do sleep 0.2; done
"$build/sign-link-run" "$(cat "$tmp/pty")" "$repo/dumps/bootflash.bin" 40
wait $bridge
cat "$tmp/bridge.log"
grep -q "APP stock BOOTVOL .* (the stage: run from RAM)" "$tmp/bridge.log" && echo "SIGN LINK: PASS" || { echo "SIGN LINK: FAIL"; exit 1; }
