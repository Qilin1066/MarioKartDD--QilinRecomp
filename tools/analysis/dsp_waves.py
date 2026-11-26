#!/usr/bin/env python3
"""List the waves of a JAudio archive (GCKart.baa) and decode one, as a reference.

  dsp_waves.py <GCKart.baa>                         every wave: archive, index, format, rate, place
  dsp_waves.py <GCKart.baa> --decode FILE.aw INDEX --waves <AudioRes/Waves> --dol <main.dol> -o out.wav
                                                    decode one with this script's own decoder

A .baa holds wave systems (WSYS); each names .aw files (WINF) and, per wave,
its format (0 ADPCM of 4 bits in 9-byte frames of 16 samples, 1 ADPCM of 2
bits in 5-byte frames, 2 PCM8, 3 PCM16), root key, sample rate, the offset
and length of its bytes in the .aw, loop flag and points, sample count and
the two history samples at the loop. The listing's numbers are what
mkdd_dsp_check takes (--aw, --offset, --bytes, --format, --samples, --loop).

The decoder is the published one for this ADPCM ("AFC"): a frame's first byte
is scale (high nibble) and predictor (low nibble); each sample is
nibble << scale plus (c0 * last + c1 * before) >> 11, with sixteen fixed
coefficient pairs (taken from the DOL, where JAudio keeps them for the DSP). It does what the DSP microcode does before resampling, so
its output compares with mkdd_dsp_check's at pitch 0x1000 only up to that
resampler's filter. Output is game data: write it outside the repository.
"""
import argparse
import os
import struct
import sys
import wave

COEFS = []  # sixteen pairs, read from the DOL (JASDSPInterface.cpp's DSPADPCM_FILTER)
ADPCM_FILTER = 0x80328E60  # GM4E01 rev 0


def read_coefs(dol_path):
    d = open(dol_path, "rb").read()
    offs = struct.unpack(">18I", d[0:72])
    addrs = struct.unpack(">18I", d[72:144])
    sizes = struct.unpack(">18I", d[144:216])
    for a, o, n in zip(addrs, offs, sizes):
        if n and a <= ADPCM_FILTER and ADPCM_FILTER + 64 <= a + n:
            v = struct.unpack_from(">32h", d, o + ADPCM_FILTER - a)
            COEFS[:] = [(v[2 * i], v[2 * i + 1]) for i in range(16)]
            return
    sys.exit("dsp_waves: the coefficient table is not in this DOL")


FORMATS = ["adpcm4", "adpcm2", "pcm8", "pcm16"]


def waves(baa):
    out = []
    at = baa.find(b"WSYS")
    while at >= 0:
        winf = at + struct.unpack_from(">I", baa, at + 0x10)[0]
        if baa[winf:winf + 4] == b"WINF":
            n = struct.unpack_from(">I", baa, winf + 4)[0]
            for g in struct.unpack_from(">%dI" % n, baa, winf + 8):
                group = at + g
                name = baa[group:group + 0x70].split(b"\0")[0].decode("ascii", "replace")
                count = struct.unpack_from(">I", baa, group + 0x70)[0]
                for i, w in enumerate(struct.unpack_from(">%dI" % count, baa, group + 0x74)):
                    e = at + w
                    fmt, key = baa[e + 1], baa[e + 2]
                    rate, offset, size, loop, start, end, samples = struct.unpack_from(">fIIIIII", baa, e + 4)
                    last, before = struct.unpack_from(">hh", baa, e + 0x20)
                    out.append(dict(file=name, index=i, format=fmt, key=key, rate=rate, offset=offset, bytes=size,
                                    loop=loop != 0, loop_start=start, loop_end=end, samples=samples, last=last, before=before))
        at = baa.find(b"WSYS", at + 4)
    return out


def clamp(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def decode(data, fmt, samples):
    out = []
    if fmt == 2:
        return [(b - 256 if b > 127 else b) << 8 for b in data[:samples]]
    if fmt == 3:
        return list(struct.unpack(">%dh" % samples, data[:samples * 2]))
    last = before = 0
    frame = 9 if fmt == 0 else 5
    for f in range(0, len(data) - frame + 1, frame):
        scale, c = data[f] >> 4, COEFS[data[f] & 15]
        for i in range(16):
            if fmt == 0:
                nib = (data[f + 1 + i // 2] >> (4 if i % 2 == 0 else 0)) & 15
                nib = nib - 16 if nib > 7 else nib
                delta = nib << scale
            else:
                nib = (data[f + 1 + i // 4] >> (6 - 2 * (i % 4))) & 3
                nib = nib - 4 if nib > 1 else nib
                delta = (nib << scale) << 2  # the 2-bit code sits two bits higher
            v = clamp(delta + ((c[0] * last + c[1] * before) >> 11))
            before, last = last, v
            out.append(v)
            if len(out) == samples:
                return out
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baa")
    ap.add_argument("--decode", nargs=2, metavar=("FILE", "INDEX"))
    ap.add_argument("--waves", help="the folder with the .aw files")
    ap.add_argument("--dol", help="main.dol: the ADPCM coefficients are read from it")
    ap.add_argument("-o", "--output")
    a = ap.parse_args()
    all_waves = waves(open(a.baa, "rb").read())
    if not a.decode:
        for w in all_waves:
            print("%-26s %4d %-6s key %3d %7.0f Hz --offset %d --bytes %d --format %d --samples %d%s" % (
                w["file"], w["index"], FORMATS[w["format"] & 3], w["key"], w["rate"], w["offset"], w["bytes"], w["format"],
                w["samples"], " --loop %d %d" % (w["loop_start"], w["loop_end"]) if w["loop"] else ""))
        return
    w = [x for x in all_waves if x["file"] == a.decode[0] and x["index"] == int(a.decode[1])]
    if not w or not a.waves or not a.output or not a.dol:
        sys.exit("dsp_waves: no such wave, or --waves, --dol or -o missing")
    read_coefs(a.dol)
    w = w[0]
    with open(os.path.join(a.waves, w["file"]), "rb") as f:
        f.seek(w["offset"])
        data = f.read(w["bytes"])
    pcm = decode(data, w["format"], w["samples"])
    with wave.open(a.output, "wb") as o:
        o.setnchannels(1)
        o.setsampwidth(2)
        o.setframerate(int(w["rate"]))
        o.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
    print("%s #%d: %d samples at %.0f Hz -> %s" % (w["file"], w["index"], len(pcm), w["rate"], a.output))


if __name__ == "__main__":
    main()
