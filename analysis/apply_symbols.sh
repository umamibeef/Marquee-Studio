#!/bin/sh
# Apply analysis/symbols.tsv to the Ghidra project and re-export the named decompile:
#   ghidra_out/elite_rgb_named.c (+ .functions.txt, + .h), then compile-checks it (tools/compile_check.py)
# Needs JDK 21 (JDK 27 crashes Ghidra's JPype) and the Ghidra build in ../build.
set -e
cd "$(dirname "$0")"
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"
# refuse duplicate names up front (they would give clashing C definitions)
python3 - <<'PY'
import re, collections, sys
c = collections.Counter(l.split("\t")[2] for l in open("symbols.tsv")
                        if re.match(r"[0-9a-f]{8}\t", l) and len(l.split("\t")) > 2)
dup = [n for n, k in c.items() if k > 1]
if dup:
    sys.exit("symbols.tsv: duplicate names: " + ", ".join(dup))
PY
# Ghidra compiles every script in -scriptPath, so give it only the two it needs.
GS=$(mktemp -d)
cp gscripts/ApplySymbols.java gscripts/ExportDecomp.java "$GS"/
../build/ghidra_12.2_PUBLIC/support/analyzeHeadless ghidra_proj elitefull -process ELITE_RGB -noanalysis \
    -scriptPath "$GS" \
    -postScript ApplySymbols.java "$PWD/symbols.tsv" \
    -postScript ExportDecomp.java "$PWD/ghidra_out/elite_rgb_named" 2>&1 \
  | grep -E "ApplySymbols:|fail |no function|skip:|DONE|ERROR"
rm -rf "$GS"
# compile check: the decompile must not lose any function that compiled before (ratchets the baseline)
python3 tools/compile_check.py --ratchet
