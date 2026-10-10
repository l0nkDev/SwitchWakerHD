#!/usr/bin/env python3
"""The game's shader programs in its files: every .sharcfb inside its archives (.szs: Yaz0 + SARC; .pack: SARC;
nested), for collect_programs.py. Extracted once into a directory; it stays on this computer.

    archives.py <game dir> <out dir>
"""
import hashlib
import os
import struct
import sys


def yaz0(b):
    size = struct.unpack_from(">I", b, 4)[0]
    src, out = 16, bytearray()
    while len(out) < size:
        code = b[src]
        src += 1
        for i in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> i):
                out.append(b[src])
                src += 1
            else:
                b1, b2 = b[src], b[src + 1]
                src += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = b[src] + 0x12
                    src += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def sarc_list(d):
    data_off = struct.unpack_from(">I", d, 0xC)[0]
    n = struct.unpack_from(">H", d, 0x14 + 6)[0]
    nodes = 0x14 + 0xC
    names = nodes + n * 16 + 8
    out = []
    for i in range(n):
        _, attr, s, e = struct.unpack_from(">IIII", d, nodes + i * 16)
        no = (attr & 0xFFFF) * 4
        nm = d[names + no:d.index(b"\0", names + no)].decode()
        out.append((nm, d[data_off + s:data_off + e]))
    return out


def visit(data, found, depth=0):
    if data[:4] == b"Yaz0":
        try:
            data = yaz0(data)
        except Exception:
            return
    if data[:4] == b"BAHS":  # .sharcfb
        found.append(data)
    elif data[:4] == b"SARC" and depth < 4:
        try:
            for _, b in sarc_list(data):
                visit(b, found, depth + 1)
        except Exception:
            pass


def misc_blobs(game):
    """the decompressed files of content/Common/Misc/Misc.szs (the title's and the system's programs)"""
    d = open(os.path.join(game, "content", "Common", "Misc", "Misc.szs"), "rb").read()
    d = yaz0(d) if d[:4] == b"Yaz0" else d
    return [b for _, b in sarc_list(d)] if d[:4] == b"SARC" else [d]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    game, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    seen = set()
    for root, _, files in os.walk(os.path.join(game, "content")):
        for f in files:
            if f.endswith((".szs", ".pack", ".sarc", ".sharcfb")):
                found = []
                visit(open(os.path.join(root, f), "rb").read(), found)
                for b in found:
                    h = hashlib.sha1(b).hexdigest()[:16]
                    if h not in seen:
                        seen.add(h)
                        open(os.path.join(out, h + ".sharcfb"), "wb").write(b)
    print(f"{len(seen)} shader archives in {out}")


if __name__ == "__main__":
    main()
