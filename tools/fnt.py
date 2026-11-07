"""Windows 3.x .FON (NE) bitmap font reader: list fonts / render text."""
import struct, sys

def fonts(path):
    d = open(path, 'rb').read()
    ne = struct.unpack_from('<I', d, 0x3c)[0]
    if d[ne:ne + 2] != b'NE':
        return []
    rt = ne + struct.unpack_from('<H', d, ne + 0x24)[0]
    shift = struct.unpack_from('<H', d, rt)[0]
    p = rt + 2
    out = []
    while True:
        tid = struct.unpack_from('<H', d, p)[0]
        if tid == 0:
            break
        cnt = struct.unpack_from('<H', d, p + 2)[0]
        p += 8
        for i in range(cnt):
            off, ln = struct.unpack_from('<HH', d, p)
            p += 12
            if tid == 0x8008:  # RT_FONT
                out.append(parse_fnt(d[off << shift:(off << shift) + (ln << shift)]))
    return out

def parse_fnt(f):
    ver = struct.unpack_from('<H', f, 0)[0]
    h = {}
    h['ver'] = ver
    h['points'] = struct.unpack_from('<H', f, 0x44)[0]
    h['ascent'] = struct.unpack_from('<H', f, 0x4a)[0]
    h['height'] = struct.unpack_from('<H', f, 0x58)[0]
    h['weight'] = struct.unpack_from('<H', f, 0x53)[0]
    h['first'] = f[0x5f]
    h['last'] = f[0x60]
    face_off = struct.unpack_from('<I', f, 0x69)[0]
    h['face'] = f[face_off:f.index(b'\0', face_off)].decode('latin1')
    n = h['last'] - h['first'] + 2
    ct = 0x76 if ver == 0x200 else 0x94
    glyphs = {}
    for i in range(n - 1):
        if ver == 0x200:
            w, off = struct.unpack_from('<HH', f, ct + i * 4)
        else:
            w, off = struct.unpack_from('<HI', f, ct + i * 6)
        cols = (w + 7) // 8
        rows = []
        for y in range(h['height']):
            bits = 0
            for c in range(cols):
                bits = (bits << 8) | f[off + c * h['height'] + y]
            rows.append((bits, cols * 8))
        glyphs[h['first'] + i] = (w, rows)
    h['glyphs'] = glyphs
    return h

def render(font, text):
    """list of rows of 0/1"""
    W = sum(font['glyphs'].get(ord(c), (0, None))[0] for c in text)
    img = [[0] * W for _ in range(font['height'])]
    x = 0
    for c in text:
        g = font['glyphs'].get(ord(c))
        if not g:
            continue
        w, rows = g
        for y, (bits, nb) in enumerate(rows):
            for i in range(w):
                if bits >> (nb - 1 - i) & 1:
                    img[y][x + i] = 1
        x += w
    return img

if __name__ == '__main__':
    for p in sys.argv[1:]:
        for f in fonts(p):
            print(p.split('/')[-1], f['face'], 'pt', f['points'], 'h', f['height'], 'asc', f['ascent'], 'w', f['weight'], 'v%x' % f['ver'])
