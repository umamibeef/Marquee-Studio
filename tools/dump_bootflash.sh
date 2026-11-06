#!/bin/sh
# Read the sign's socketed boot flash (AMD AM29LV040B, PLCC32) with a TL866 / T48 via minipro.
# READ ONLY: never pass -w or -E here. Reads twice and compares, so a bad contact can't slip through.
# Usage: dump_bootflash.sh     (output in ../dumps/)
set -e
cd "$(dirname "$0")/.."
mkdir -p dumps
CHIP="AM29LV040B@PLCC32"
# Patched minipro (build/): stock minipro 0.7.4 drives 3.3 V chips at 5 V on a TL866A (upstream #387).
MINIPRO="${MINIPRO:-$PWD/build/install/bin/minipro}"
[ -x "$MINIPRO" ] || { echo "Patched minipro not found at $MINIPRO"; exit 1; }
STAMP=$(date +%Y%m%d-%H%M%S)
A="dumps/bootflash-$STAMP-read1.bin"
B="dumps/bootflash-$STAMP-read2.bin"

echo "== Programmer"
"$MINIPRO" -k || { echo "No programmer found: is the TL866 plugged in?"; exit 1; }

echo "== Read 1 of 2 ($CHIP). minipro checks the chip ID first; an ID mismatch means bad seating."
"$MINIPRO" -p "$CHIP" -r "$A"
echo "== Read 2 of 2"
"$MINIPRO" -p "$CHIP" -r "$B"

echo "== Results"
ls -l "$A" "$B"
shasum -a 256 "$A" "$B"
CHECK=$(python3 -I tools/check_address_lines.py "$A" "$B")
echo "$CHECK"
if echo "$CHECK" | grep -q STUCK; then
    echo "BAD: an address line didn't reach the chip (contact problem). Reads are not valid, even if they match."
    exit 3
fi
if cmp -s "$A" "$B"; then
    echo "MATCH: both reads are identical and every address line toggles."
    cp "$A" dumps/bootflash.bin
    echo "Saved as dumps/bootflash.bin"
else
    echo "MISMATCH: the reads differ. Reseat the chip in the adapter and run this again."
    exit 2
fi
python3 -I - dumps/bootflash.bin <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
ff = d.count(0xFF) / len(d)
zero = d.count(0) / len(d)
print(f"{len(d)} bytes, {ff:.1%} 0xFF (erased), {zero:.1%} 0x00")
if ff > 0.99 or zero > 0.99:
    print("WARNING: almost all one value; that suggests a bad read, not real data.")
EOF
