#!/bin/sh
# ARP-sweep every private and link-local range out one interface, looking for the sign. Run with sudo.
# Usage: sudo tools/sign-arpsweep.sh IFACE    (needs arp-scan in PATH)
IFACE=${1:?usage: sudo $0 IFACE}
D=$(cd "$(dirname "$0")/.." && pwd)/captures/network
mkdir -p "$D"
OUT=$D/arpsweep.txt
MAC=$(ifconfig "$IFACE" 2>/dev/null | awk '/ether/{print $2; exit}')
: > "$OUT"
tcpdump -i "$IFACE" -n -e -s 0 -U -w "$D/sweep.pcap" ${MAC:+"not ether src $MAC"} 2>/dev/null &
TP=$!
trap 'kill $TP 2>/dev/null; chmod a+r "$D/sweep.pcap" "$OUT"' EXIT INT TERM
scan() { echo "== $*" | tee -a "$OUT"; arp-scan -I "$IFACE" -g -B 50M -r 1 "$@" 2>&1 | grep -vE '^(Interface|Starting|Ending)' | tee -a "$OUT"; }
for R in 192.168.0.0/16 172.16.0.0/12 169.254.0.0/16; do
  scan "$R"
  scan --arpspa=0.0.0.0 "$R"
done
echo "Now 10.0.0.0/8 (16M addresses, about 4-5 minutes)..."
scan 10.0.0.0/8
echo "Done. Results in $OUT"
