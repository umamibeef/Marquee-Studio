#!/bin/sh
# Capture all Ethernet traffic during a sign boot. Run with sudo.
# Usage: sudo tools/sign-capture.sh IFACE    (IFACE: the interface cabled to the sign)
# Give IFACE a static address that does not overlap any other network this machine is on.
set -e
IFACE=${1:?usage: sudo $0 IFACE}
D=$(cd "$(dirname "$0")/.." && pwd)/captures/network
mkdir -p "$D"
echo "Capturing on $IFACE -> $D/sign.pcap"
echo "Power-cycle the sign now, wait ~2 minutes after it finishes booting, then press Ctrl-C."
tcpdump -i "$IFACE" -n -e -s 0 -U -w "$D/sign.pcap" &
P=$!
trap 'kill $P 2>/dev/null; chmod a+r "$D/sign.pcap"; echo saved' INT TERM
tcpdump -i "$IFACE" -n -e -l 'not ip6' 2>/dev/null &
wait $P
