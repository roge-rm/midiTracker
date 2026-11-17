#!/usr/bin/env python3
# flac_make.py, FLAC files for flac_check.sh that ffmpeg doesn't make:
#   flac_make.py verbatim <in.wav> <out.flac> <block>   PCM WAV (8/16/24-bit) as verbatim
#                                                     subframes, at the WAV's own bit depth
#   flac_make.py seektable <in.flac> <out.flac> <every>  adds a SEEKTABLE with a point every
#                                                     <every> frames (1 is every frame)
#   flac_make.py bits <in.flac> <out.flac> <bits>        STREAMINFO says <bits> bits per sample
#   flac_make.py cut <in.flac> <out.flac> <permille>     the first <permille>/1000 of the file
#   flac_make.py corrupt <in.flac> <out.flac>            junk over a few spots in the frames
#   flac_make.py id3 <in.flac> <out.flac>                an ID3v2 tag before the stream
#   flac_make.py badfirst <in.flac> <out.flac>           junk over the first frame
#   flac_make.py nolength <in.flac> <out.flac>           STREAMINFO's sample count 0 (unknown)
import struct
import sys


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, n):
        self.acc = (self.acc << n) | (v & ((1 << n) - 1))
        self.n += n
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def align(self):
        if self.n:
            self.put(0, 8 - self.n)


def utf8num(v):
    if v < 0x80:
        return bytes([v])
    for n, top in ((2, 0xC0), (3, 0xE0), (4, 0xF0), (5, 0xF8), (6, 0xFC)):
        if v < 1 << (5 * n + 1):
            b = [0x80 | ((v >> (6 * i)) & 0x3F) for i in range(n - 1)]
            return bytes([top | (v >> (6 * (n - 1)))] + b[::-1])
    raise ValueError(v)


def read_wav(path):
    d = open(path, 'rb').read()
    pos = 12
    fmt = data = None
    while pos + 8 <= len(d):
        cid, size = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[pos + 8:pos + 24])
        elif cid == b'data':
            data = d[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, align, bits = fmt
    width = bits // 8
    n = len(data) // align
    chans = [[0] * n for _ in range(ch)]
    for i in range(n):
        for c in range(ch):
            o = i * align + c * width
            if bits == 8:
                v = data[o] - 128
            else:
                v = int.from_bytes(data[o:o + width], 'little', signed=True)
            chans[c][i] = v
    return rate, bits, chans


def verbatim(src, dst, block):
    rate, bits, chans = read_wav(src)
    ch, n = len(chans), len(chans[0])
    bpscode = {8: 1, 12: 2, 16: 4, 20: 5, 24: 6}[bits]
    frames = bytearray()
    maxframe, minframe = 0, 1 << 30
    for num, start in enumerate(range(0, n, block)):
        cnt = min(block, n - start)
        h = bytearray([0xFF, 0xF8, 0x70, ((ch - 1) << 4) | (bpscode << 1)])  # block size from the 16 bits at the end, rate from STREAMINFO
        h += utf8num(num) + struct.pack('>H', cnt - 1)
        h.append(crc8(h))
        b = Bits()
        for c in range(ch):
            b.put(0, 1)
            b.put(1, 6)  # verbatim
            b.put(0, 1)
            for i in range(start, start + cnt):
                b.put(chans[c][i], bits)
        b.align()
        f = bytes(h) + bytes(b.out)
        f += struct.pack('>H', crc16(f))
        maxframe, minframe = max(maxframe, len(f)), min(minframe, len(f))
        frames += f
    si = bytearray(struct.pack('>HH', block, block))
    si += minframe.to_bytes(3, 'big') + maxframe.to_bytes(3, 'big')
    v = (rate << 44) | ((ch - 1) << 41) | ((bits - 1) << 36) | n
    si += v.to_bytes(8, 'big') + bytes(16)
    out = b'fLaC' + bytes([0x80]) + len(si).to_bytes(3, 'big') + bytes(si) + bytes(frames)
    open(dst, 'wb').write(out)


def blocks(d):
    """(start, type, len, last) of each header block, and the first frame's offset."""
    assert d[:4] == b'fLaC'
    pos, out = 4, []
    while True:
        t, ln = d[pos] & 0x7F, int.from_bytes(d[pos + 1:pos + 4], 'big')
        last = d[pos] & 0x80
        out.append((pos, t, ln, last))
        pos += 4 + ln
        if last:
            return out, pos


def frame_starts(d, first, si):
    """(offset, first sample, samples) of each frame, found by sync code and header CRC."""
    minblock = int.from_bytes(si[0:2], 'big')
    out = []
    pos = first
    while pos + 16 <= len(d):
        if d[pos] != 0xFF or (d[pos + 1] & 0xFE) != 0xF8:
            pos += 1
            continue
        b2 = d[pos + 2]
        bs, var = b2 >> 4, d[pos + 1] & 1
        q = pos + 4
        c = d[q]
        if c < 0x80:
            nb, v = 1, c
        elif c >= 0xFE:
            nb, v = 7, 0
        elif c >= 0xC0:
            nb = next(n for n, top in ((6, 0xFC), (5, 0xF8), (4, 0xF0), (3, 0xE0), (2, 0xC0)) if c >= top)
            v = c & (0x7F >> nb)
        else:
            pos += 1
            continue
        for k in range(1, nb):
            v = (v << 6) | (d[q + k] & 0x3F)
        q += nb
        if bs == 6:
            cnt = d[q] + 1
            q += 1
        elif bs == 7:
            cnt = int.from_bytes(d[q:q + 2], 'big') + 1
            q += 2
        elif bs == 1:
            cnt = 192
        elif 2 <= bs <= 5:
            cnt = 576 << (bs - 2)
        elif bs >= 8:
            cnt = 256 << (bs - 8)
        else:
            pos += 1
            continue
        rc = b2 & 15
        q += 1 if rc == 12 else 2 if rc in (13, 14) else 0
        if crc8(d[pos:q]) != d[q]:
            pos += 1
            continue
        out.append((pos - first, v if var else v * minblock, cnt))
        pos = q + 1
    return out


def seektable(src, dst, every):
    d = open(src, 'rb').read()
    bl, first = blocks(d)
    si = d[bl[0][0] + 4:bl[0][0] + 4 + 34]
    pts = frame_starts(d, first, si)[::every]
    body = b''.join(struct.pack('>QQH', s, o, c) for o, s, c in pts)
    # STREAMINFO, then the table, then the other blocks without an old table.
    keep = [b for b in bl[1:] if b[1] != 3]
    out = bytearray(b'fLaC')
    out += bytes([0]) + d[bl[0][0] + 1:bl[0][0] + 4 + 34]
    parts = [(3, body)] + [(t, d[p + 4:p + 4 + ln]) for p, t, ln, _ in keep]
    for i, (t, body) in enumerate(parts):
        out += bytes([t | (0x80 if i == len(parts) - 1 else 0)]) + len(body).to_bytes(3, 'big') + body
    out += d[first:]
    open(dst, 'wb').write(out)
    print(f"{len(pts)} seek points")


def setbits(src, dst, bits):
    d = bytearray(open(src, 'rb').read())
    p = 8 + 12  # STREAMINFO body byte 12
    d[p] = (d[p] & 0xFE) | (((bits - 1) >> 4) & 1)
    d[p + 1] = (d[p + 1] & 0x0F) | (((bits - 1) & 15) << 4)
    open(dst, 'wb').write(d)


def main():
    cmd, a, b = sys.argv[1:4]
    if cmd == 'verbatim':
        verbatim(a, b, int(sys.argv[4]))
    elif cmd == 'seektable':
        seektable(a, b, int(sys.argv[4]))
    elif cmd == 'bits':
        setbits(a, b, int(sys.argv[4]))
    elif cmd == 'cut':
        d = open(a, 'rb').read()
        open(b, 'wb').write(d[:len(d) * int(sys.argv[4]) // 1000])
    elif cmd == 'corrupt':
        d = bytearray(open(a, 'rb').read())
        for k in (3, 5, 7):
            at = len(d) * k // 10
            d[at:at + 300] = bytes((i * 37 + k) & 0xFF for i in range(300))
        open(b, 'wb').write(d)
    elif cmd == 'badfirst':
        d = bytearray(open(a, 'rb').read())
        _, first = blocks(d)
        d[first + 8:first + 208] = bytes((i * 53 + 1) & 0xFF for i in range(200))
        open(b, 'wb').write(d)
    elif cmd == 'nolength':
        d = bytearray(open(a, 'rb').read())
        p = 8 + 13  # STREAMINFO body byte 13: bits low 4, then the 36-bit count
        d[p] &= 0xF0
        d[p + 1:p + 5] = bytes(4)
        open(b, 'wb').write(d)
    elif cmd == 'id3':
        d = open(a, 'rb').read()
        body = b'TIT2' + struct.pack('>I', 6) + b'\0\0\0test\0'
        open(b, 'wb').write(b'ID3\x03\x00\x00' + bytes([0, 0, 0, len(body)]) + body + d)


main()
