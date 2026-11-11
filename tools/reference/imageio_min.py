"""Minimal image I/O with only the standard library and NumPy.

Reads 8-bit PNG (gray, gray+alpha, RGB, RGBA, palette; non-interlaced),
24/32-bit BMP and PAM (P7). Writes 8-bit RGB/RGBA PNG. Images are returned
as uint8 arrays of shape (height, width, 4).
"""

import struct
import zlib

import numpy as np

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def _unfilter(raw, height, stride, bpp):
    out = np.zeros((height, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.uint8)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        line = np.frombuffer(raw, dtype=np.uint8, count=stride, offset=pos + 1).copy()
        pos += stride + 1
        if ftype == 0:
            cur = line
        elif ftype == 1:  # Sub: running sum per channel
            cur = line.copy()
            for c in range(bpp):
                cur[c::bpp] = np.cumsum(line[c::bpp], dtype=np.uint64).astype(np.uint8)
        elif ftype == 2:  # Up
            cur = (line + prev).astype(np.uint8)
        elif ftype in (3, 4):  # Average, Paeth: sequential, done in Python
            cur = bytearray(line.tobytes())
            p = prev.tobytes()
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = p[i]
                if ftype == 3:
                    pred = (a + b) >> 1
                else:
                    c = p[i - bpp] if i >= bpp else 0
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pred) & 0xFF
            cur = np.frombuffer(bytes(cur), dtype=np.uint8)
        else:
            raise ValueError("bad PNG filter type %d" % ftype)
        out[y] = cur
        prev = out[y]
    return out


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != PNG_MAGIC:
        raise ValueError("%s: not a PNG" % path)
    pos = 8
    idat = []
    palette = trns = None
    while pos < len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            w, h, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            palette = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3)
        elif ctype == b"tRNS":
            trns = np.frombuffer(body, dtype=np.uint8)
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    if depth != 8 or interlace != 0:
        raise ValueError("%s: only 8-bit non-interlaced PNG is supported" % path)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(b"".join(idat))
    px = _unfilter(raw, h, w * channels, channels).reshape(h, w, channels)
    out = np.full((h, w, 4), 255, dtype=np.uint8)
    if color == 0:
        out[..., :3] = px
    elif color == 2:
        out[..., :3] = px
    elif color == 3:
        idx = px[..., 0]
        out[..., :3] = palette[idx]
        if trns is not None:
            alpha = np.full(256, 255, dtype=np.uint8)
            alpha[:len(trns)] = trns
            out[..., 3] = alpha[idx]
    elif color == 4:
        out[..., :3] = px[..., :1]
        out[..., 3] = px[..., 1]
    else:
        out[...] = px
    return out


def write_png(path, img):
    """Writes uint8 or uint16 (h, w, 3|4) as PNG with the Up filter on every row."""
    depth = 16 if img.dtype == np.uint16 else 8
    if depth == 16:
        img = np.ascontiguousarray(img, dtype=">u2").view(np.uint8).reshape(img.shape[0], img.shape[1], -1)
    else:
        img = np.ascontiguousarray(img, dtype=np.uint8)
    h, w, c = img.shape
    color = {3: 2, 4: 6, 6: 2, 8: 6}[c]
    rows = img.reshape(h, w * c)
    up = rows.copy()
    up[1:] = rows[1:] - rows[:-1]  # uint8 wraps, as PNG wants
    raw = np.concatenate([np.full((h, 1), 2, dtype=np.uint8), up], axis=1).tobytes()

    def chunk(t, body):
        return struct.pack(">I", len(body)) + t + body + struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(PNG_MAGIC)
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def read_pam(path):
    data = open(path, "rb").read()
    end = data.index(b"ENDHDR\n") + len(b"ENDHDR\n")
    fields = {}
    for line in data[:end].decode("ascii").splitlines()[1:-1]:
        k, v = line.split(None, 1)
        fields[k] = v
    w, h, d = int(fields["WIDTH"]), int(fields["HEIGHT"]), int(fields["DEPTH"])
    px = np.frombuffer(data, dtype=np.uint8, count=w * h * d, offset=end).reshape(h, w, d)
    if d == 4:
        return px.copy()
    out = np.full((h, w, 4), 255, dtype=np.uint8)
    out[..., :3] = px[..., :3]
    return out


def read_bmp(path):
    data = open(path, "rb").read()
    offset = struct.unpack("<I", data[10:14])[0]
    w, h = struct.unpack("<ii", data[18:26])
    bpp = struct.unpack("<H", data[28:30])[0]
    if bpp not in (24, 32):
        raise ValueError("%s: only 24/32-bit BMP is supported" % path)
    bottom_up = h > 0
    h = abs(h)
    bpr = bpp // 8
    stride = (w * bpr + 3) & ~3
    rows = np.frombuffer(data, dtype=np.uint8, count=stride * h, offset=offset).reshape(h, stride)
    px = rows[:, :w * bpr].reshape(h, w, bpr)
    if bottom_up:
        px = px[::-1]
    out = np.full((h, w, 4), 255, dtype=np.uint8)
    out[..., 0] = px[..., 2]
    out[..., 1] = px[..., 1]
    out[..., 2] = px[..., 0]
    return out
