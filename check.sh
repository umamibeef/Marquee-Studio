#!/bin/bash
# Every check in the project, in one place. Stops at the first failure.
#   ./check.sh          quick: decompile compile check + firmware-verified tests + bootloader build/host test
#   ./check.sh --full   also the bootloader co-simulations (boot, network, patch), the C++ simulator and its GDB server, the
#                       colour-emoji test and the from-source firmware with native emoji (build and in-firmware test)
# The decompile compile check also runs at the end of analysis/apply_symbols.sh (every naming stage).
set -eo pipefail
cd "$(dirname "$0")"
PY=venv/bin/python
APP=analysis/extracted/ELITE_RGB
# each step's time is printed when the next one starts (and at the end): CHECK_T0 is the step's start
CHECK_T0=$(date +%s); CHECK_NAME=""
step() {
    if [ -n "$CHECK_NAME" ]; then printf '   (%ss)\n' "$(( $(date +%s) - CHECK_T0 ))"; fi
    CHECK_T0=$(date +%s); CHECK_NAME="$*"
    printf '\n== %s\n' "$*"
}
trap 'if [ -n "$CHECK_NAME" ]; then printf "   (%ss)\n" "$(( $(date +%s) - CHECK_T0 ))"; fi' EXIT

step "decompile compiles (analysis/tools/compile_check.py; no regressions vs the baseline)"
python3 analysis/tools/compile_check.py --ratchet

step "message markup against the firmware tokenizer (Unicorn)"
$PY analysis/tools/test_message_markup.py | tail -1

step "studio markup encoder + emulator against the firmware (studio/test_markup.py)"
(cd studio && ../$PY test_markup.py | tail -1)

step "studio/emulator effect rendering against the firmware drawing code (studio/test_effects.py)"
(cd studio && ../$PY test_effects.py | tail -1)

step "imported pixel fonts (studio/test_font_import.py; font_build under Unicorn)"
(cd studio && ../$PY test_font_import.py | tail -1)

step "studio web API on a copy of the content folder (studio/test_studio_api.py)"
(cd studio && ../$PY test_studio_api.py | tail -1)

step "money amounts: studio amount_format == the firmware's (studio/test_amount.py)"
(cd studio && ../$PY test_amount.py | tail -1)

if command -v npm >/dev/null 2>&1; then
    step "front end: tsc --noEmit, eslint (shadcn/ui only), vite build (frontend/)"
    (cd frontend && { [ -d node_modules ] || npm ci --silent; } && npm run --silent typecheck && npm run --silent lint \
        && npm run --silent build >/dev/null && echo "  front end OK")
    step "web studio: every preview emulated in the browser (frontend/scripts/emulation-only.mjs)"
    (cd studio && MARQUEE_STUDIO_PORT=8799 exec "../$PY" server.py >/dev/null 2>&1) & SRV=$!
    for _ in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:8799/api/config && break; sleep 0.2; done
    (cd frontend && node scripts/emulation-only.mjs http://127.0.0.1:8799); R=$?
    kill $SRV 2>/dev/null || true; wait $SRV 2>/dev/null || true
    [ $R = 0 ] || exit 1
else
    step "front end: skipped (node/npm not installed)"
fi

step "CRC routines against references and tools/signproto.py"
$PY analysis/tools/test_crcs.py | tail -1

step "/dev/random is MT19937"
$PY analysis/tools/test_mt.py | tail -1

step "claims: named functions do what their names say (analysis/tools/test_claims.py)"
(cd analysis/tools && ../../$PY test_claims.py | tail -1)

step "recreated source == firmware: difftest self-test and the curated list"
(cd analysis/tools && ../../$PY difftest.py --selftest | grep -c "^  ok" | sed 's/^/  self-test cases caught: /' \
  && ../../$PY difftest.py --quiet $(grep -v "^#" difftest_list.txt) | tail -1)

step "FPGA loading against a fake FPGA (analysis/tools/test_fpga.py)"
(cd analysis/tools && ../../$PY test_fpga.py | tail -1)

step "soft-float helpers identified by running them"
$PY analysis/tools/softfloat_id.py | grep -c . | sed 's/^/  identified: /'

step "NVRAM decoder against the firmware"
$PY tools/test_nvram_decode.py $APP | tail -1

step "GIF -> FLC against the firmware decoders"
$PY tools/test_gif2flc.py $APP | tail -1

step "bootloader: build and host test"
make -s -C bootloader all
make -s -C bootloader test | tail -2
step "NRV2D compressor round trip (bootloader/tools/nrv2d_pack.py)"
python3 bootloader/tools/nrv2d_pack.py --test

step "eBPF interpreter (firmware/ext/ebpf.c) on the bpf-conformance programs, natively"
mkdir -p build && cc -O2 -Wall -Wextra -I firmware/ext firmware/ext/ebpf.c firmware/ext/test/ebpf_conformance.c \
    -o build/ebpf_conformance && build/ebpf_conformance frontend/packages/sign-emu/test/ebpf_conformance.tsv | tail -1

step "every app in apps/ builds into a bundle (tools/mkapp.py; needs Homebrew LLVM for the BPF target)"
for a in apps/*/app.c; do d=$(dirname "$a"); $PY tools/mkapp.py "$d" build/$(basename "$d").mqap | tail -1; done

if [ "$1" = "--full" ]; then
    step "bootloader co-simulation (boot to the app)"
    make -s -C bootloader cosim | tail -2
    step "bootloader network simulation"
    make -s -C bootloader netsim | tail -2
    step "recreated source == firmware: every function the full sweep found equivalent (about 20 minutes)"
    (cd analysis/tools && ../../$PY difftest.py --quiet --trials 6 $(grep -v "^#" difftest_full.txt) | tail -1)
    step "the C++ simulator: build, and its GDB server driven by gdb (simulator/tests/test_gdb.sh)"
    (cmake -S simulator -B build/simulator -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build/simulator -j 8 >/dev/null)
    simulator/tests/test_gdb.sh | tail -1
    step "lines in the firmware's attract configuration are shown (umami-marquee-sim-test attracttest)"
    build/simulator/umami-marquee-sim-test attracttest | tail -1
    step "colour emoji patch"
    make -s -C bootloader emojitest | tail -2
    step "the firmware from source: image, Studio parity, delivery, crash, store and update across a restart (tools/fw_check.sh)"
    tools/fw_check.sh
    step "the Studio's sign link against an emulated sign behind a pty (simulator/tests/test_signlink.sh)"
    simulator/tests/test_signlink.sh | tail -1
fi
printf '\nALL CHECKS PASSED\n'
