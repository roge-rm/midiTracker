# aiff_make.py <outdir>
# Writes odd AIFFs with the WAV of the same audio beside them: x_*.aiff and x_*.wav.
#   x_layout: SSND before COMM, an odd-size chunk first, SSND offset 6, junk past COMM's frame count
#   x_12bit:  12-bit samples, left-justified in 16 bits
#   x_aifc:   big-endian AIFF-C, compression NONE
import struct, sys, math, random, wave
out = sys.argv[1]
random.seed(5)
N = 40000

def samples(mask):
    s = []
    for i in range(N):
        l = int(9000 * math.sin(i * 0.05) + random.randint(-3000, 3000))
        r = int(8000 * math.sin(i * 0.031 + 1) + random.randint(-3000, 3000))
        s.append(tuple(((max(-32768, min(32767, v)) & mask) ^ 0x8000) - 0x8000 for v in (l, r)))
    return s

def write_wav(path, s):
    w = wave.open(path, 'wb')
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(b''.join(struct.pack('<hh', *f) for f in s))
    w.close()

def chunk(cid, data):
    return cid + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')

def comm(frames, bits, comp=None):
    d = struct.pack('>HIH', 2, frames, bits) + struct.pack('>HQ', 0x400E, 44100 << 48)
    if comp: d += comp + b'\x04none' + b'\0' # pascal string, padded to even
    return chunk(b'COMM', d)

def form(kind, chunks):
    body = kind + b''.join(chunks)
    return b'FORM' + struct.pack('>I', len(body)) + body

def data(s): return b''.join(struct.pack('>hh', *f) for f in s)

s = samples(0xFFFF)
write_wav(out + '/x_layout.wav', s)
ssnd = chunk(b'SSND', struct.pack('>II', 6, 0) + b'\xAA' * 6 + data(s) + b'\x55' * 40)
open(out + '/x_layout.aiff', 'wb').write(form(b'AIFF', [chunk(b'ANNO', b'odd'), ssnd, comm(N, 16), chunk(b'APPL', b'tail!')]))

s12 = samples(0xFFF0)
write_wav(out + '/x_12bit.wav', s12)
open(out + '/x_12bit.aiff', 'wb').write(form(b'AIFF', [comm(N, 12), chunk(b'SSND', struct.pack('>II', 0, 0) + data(s12))]))

write_wav(out + '/x_aifc.wav', s)
open(out + '/x_aifc.aifc', 'wb').write(form(b'AIFC', [chunk(b'FVER', struct.pack('>I', 0xA2805140)), comm(N, 16, b'NONE'), chunk(b'SSND', struct.pack('>II', 0, 0) + data(s))]))
