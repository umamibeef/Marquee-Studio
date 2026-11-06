#!/usr/bin/env python3
"""Bridge the sign's DB9 serial port to a local TCP port for GDB's remote protocol (pyserial, so macOS sets the
PL2303 baud correctly via IOSSIOSPEED).

    tools/gdb_bridge.py /dev/cu.usbserial-XXXX [1235]

Then:  /opt/homebrew/bin/gdb bootloader/build-dump/fw/app.elf
       (gdb) set architecture powerpc:common
       (gdb) target remote :1235

The sim/Sign window must NOT hold the port while this runs. The firmware's GDB stub speaks the protocol over SCC1
at 115200 8N1.
"""
import sys, socket, select, serial

def main():
    port = sys.argv[1]
    tcp = int(sys.argv[2]) if len(sys.argv) > 2 else 1235
    ser = serial.Serial(port, 115200, timeout=0, rtscts=False, dsrdtr=False)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", tcp)); srv.listen(1)
    print(f"gdb bridge: {port} @115200 <-> tcp 127.0.0.1:{tcp}", flush=True)
    while True:
        conn, _ = srv.accept()
        conn.setblocking(False)
        print("client connected", flush=True)
        try:
            while True:
                r, _, _ = select.select([ser.fd, conn], [], [], 1.0)
                if ser.fd in r:
                    d = ser.read(4096)
                    if d:
                        conn.sendall(d)
                if conn in r:
                    try:
                        d = conn.recv(4096)
                    except OSError:
                        d = b""
                    if not d:
                        break
                    ser.write(d); ser.flush()
        except (ConnectionResetError, BrokenPipeError):
            pass
        conn.close()
        print("client disconnected", flush=True)

if __name__ == "__main__":
    main()
