# Send framed commands to the sign and record any reply. Usage: cmdscan.py A|B
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import signserial as s

passname = sys.argv[1]
bodies = ([bytes([x, 0]) for x in range(1, 256)] if passname == "A"
          else [bytes([0, 1, x]) for x in range(256)])
log = s.Log(f"cmdscan{passname}", 115200)
log.info(f"pass {passname}: {len(bodies)} frames, 0.3 s apart, log -> {log.path}")
fd = s.open_port(115200, log)
s.set_lines(fd, log)
w = s.Watch(fd, log)
hits = []
try:
    for body in bodies:
        out = s.frame(body)
        os.write(fd, out)
        got, end = b"", time.time() + 0.3
        while time.time() < end:
            b = s.read(fd, 0.02)
            if b:
                got += b
                end = max(end, time.time() + 0.2)  # keep collecting while it talks
        if got:
            log.info(f"REPLY to {out.hex(' ')}:")
            log.data("<", got)
            hits.append((body.hex(' '), got.hex(' ')))
        elif body[-1] % 32 == 0:
            log.info(f"... sent up to body {body.hex(' ')}, no replies so far" if not hits else f"... at {body.hex(' ')}")
except KeyboardInterrupt:
    log.info("interrupted")
log.info(f"pass {passname} done: {len(hits)} replies")
for b, g in hits:
    log.info(f"  body {b} -> {g}")
os.close(fd)
