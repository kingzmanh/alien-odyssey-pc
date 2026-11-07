"""Decode a Deluxe Paint PBM (chunky IFF) .LBM to a PIL image.
usage: lbm.py in.lbm out.png"""
import struct, sys
from PIL import Image


def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'FORM' and d[8:12] == b'PBM '
    p, end = 12, 8 + struct.unpack('>I', d[4:8])[0]
    w = h = comp = 0
    pal, body = None, None
    while p + 8 <= end:
        cid, n = d[p:p + 4], struct.unpack('>I', d[p + 4:p + 8])[0]
        c = d[p + 8:p + 8 + n]
        if cid == b'BMHD':
            w, h = struct.unpack('>HH', c[:4])
            comp = c[10]
        elif cid == b'CMAP':
            pal = list(c)
        elif cid == b'BODY':
            body = c
        p += 8 + n + (n & 1)
    row = w + (w & 1)
    if comp == 1:   # ByteRun1
        out, i = bytearray(), 0
        while len(out) < row * h and i < len(body):
            b = body[i]; i += 1
            if b < 128:
                out += body[i:i + b + 1]; i += b + 1
            elif b > 128:
                out += bytes([body[i]]) * (257 - b); i += 1
        body = bytes(out)
    img = Image.frombytes('P', (w, h), bytes(b''.join(body[y * row:y * row + w] for y in range(h))))
    img.putpalette(pal + [0] * (768 - len(pal)))
    return img


if __name__ == '__main__':
    load(sys.argv[1]).convert('RGB').save(sys.argv[2])
