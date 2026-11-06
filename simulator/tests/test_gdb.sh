#!/bin/bash
# The simulator's GDB server, driven by gdb itself: attach, registers, a breakpoint (RTEMS context switch), a single
# step, disassembly, monitor commands, RTEMS tasks as threads, a watchpoint, detach; then the process must end cleanly
# when its stdin closes. Needs build/simulator (cmake) and a gdb with PowerPC support (brew install gdb).
set -u
cd "$(dirname "$0")/../.."
SIM=build/simulator/umami-marquee-sim
GDB=${GDB:-gdb}
PORT=${PORT:-12$((RANDOM % 90 + 10))}
fail=0
check() { if grep -q -- "$2" <<<"$OUT"; then echo "ok   $1"; else echo "FAIL $1 (no '$2')"; fail=1; fi; }

T=$(mktemp -d)
mkfifo "$T/in"
"$SIM" --no-window --gdb "$PORT" <"$T/in" >"$T/log" 2>&1 &
PID=$!
exec 4>"$T/in"                                          # the simulator's stdin, held open until the end
for _ in $(seq 100); do grep -q "gdb server on" "$T/log" 2>/dev/null && break; sleep 0.1; done
OUT=$(timeout 120 "$GDB" --batch -nx -ex "set architecture powerpc:MPC8XX" -ex "set endian big" -ex "set pagination off" \
  -ex "target remote :$PORT" -ex "info registers pc" -ex "break *0x4410" -ex "continue" -ex "info registers pc" \
  -ex "stepi" -ex "info registers pc" -ex "x/2i \$pc" -ex "monitor tasks" -ex "info threads" -ex "delete" \
  -ex "watch *(int*)0x45c4cc" -ex "continue" -ex "monitor status" -ex "delete" \
  -ex "set \$r3 = 0x12345678" -ex "info registers r3" -ex "set {int}0x7f0000 = 0xcafef00d" -ex "x/1wx 0x7f0000" \
  -ex "detach" 2>&1)
check "attach: halted at the app entry" "pc             0x0"
check "breakpoint at cpu_context_switch hit" "hit Breakpoint 1, 0x00004410"
check "single step" "pc             0x4414"
check "disassembly through the server" "isync"
check "monitor tasks" "INIT"
check "RTEMS tasks as threads" "Thread 167837697 (INIT"
check "watchpoint with old and new values" "New value ="
check "monitor status" "halted ("
check "register write" "r3             0x12345678"
check "memory write" "0xcafef00d"
check "detach" "detached"
exec 4>&-                                               # close the simulator's stdin: it must exit cleanly
wait "$PID"; rc=$?
rm -rf "$T"
if [ $rc -eq 0 ]; then echo "ok   clean exit"; else echo "FAIL exit status $rc"; fail=1; fi
[ $fail -eq 0 ] && echo "RESULT: PASS" || { echo "$OUT" | tail -30; echo "RESULT: FAIL"; }
exit $fail
