#!/usr/bin/env python3
"""A silent logo_s.wav in the stock sound's own format (IMA ADPCM, stereo, 11025 Hz, 512-byte blocks of 505
samples, with a fact chunk): one block of zeros, which decodes to silence. Our firmware image carries it in place of
the 43 KB start-up sound (the sign's speaker is not connected), so logo_sound_load still finds a valid file.
    silent_wav.py OUT.wav"""
import struct, sys

BLOCK, SAMPLES = 512, 505
fmt = struct.pack("<HHIIHHHH", 17, 2, 11025, 11177, BLOCK, 4, 2, SAMPLES)
fact = struct.pack("<I", SAMPLES)
data = bytes(BLOCK)
body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"fact" + struct.pack("<I", len(fact)) + fact
        + b"data" + struct.pack("<I", len(data)) + data)
open(sys.argv[1], "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
