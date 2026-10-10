#!/usr/bin/env python3
"""Finds the shader programs a manifest (shadercache_manifest.bin) names in the game's own files, for
shader_precompile: programs.bin, records {u64 hash, u32 size, bytes}, little-endian. It stays on this computer.

    collect_programs.py <shadercache_manifest.bin> <archives dir> <game dir> <out programs.bin> [--heads programs.txt]

<archives dir>: every .sharcfb of the game (archives.py). Programs sit there at 256-byte boundaries; a few
(the title's, the system's) are in content/Common/Misc/Misc.szs instead, found by a plain search. A program is
identified by the console's hash of its first 64 bytes (manifest records of type 3), then confirmed by the hash of
the whole program. Records of type 1 (the first samples, without that hash) need the first bytes from a debug server
"programs" list (--heads).
"""
import os
import struct
import sys

M = (1 << 64) - 1


def hash_bytes(b, h=0x9E3779B97F4A7C15):  # gfx/deko/shaders_dk.cpp hash_bytes
    n = len(b)
    i = 0
    while i + 8 <= n:
        h = ((h ^ int.from_bytes(b[i:i + 8], "little")) * 0xFF51AFD7ED558CCD) & M
        h ^= h >> 32
        i += 8
    while i < n:
        h = ((h ^ b[i]) * 0x100000001B3) & M
        i += 1
    return h ^ (h >> 29)


def read_manifest(path):
    """{program hash: (size, head hash or None)}"""
    d = open(path, "rb").read()
    o, out = 0, {}
    while o < len(d):
        t = d[o]
        if t == 2:
            _, size = struct.unpack_from("<QI", d, o + 1)
            o += 13 + size
        elif t in (1, 3):
            ph, size = struct.unpack_from("<QI", d, o + 2)
            o += 14
            head = None
            if t == 3:
                head = struct.unpack_from("<Q", d, o)[0]
                o += 8
            o += 16  # GLSL hash, fetch key
            n = struct.unpack_from("<H", d, o)[0]
            o += 2 + n * 6
            out[ph] = (size, head)
        else:
            sys.exit(f"manifest: unknown record type {t} at {o}")
    return out


def main():
    argv = sys.argv[1:]
    heads_path = None
    if "--heads" in argv:
        i = argv.index("--heads")
        heads_path = argv[i + 1]
        del argv[i:i + 2]
    args = argv
    if len(args) != 4:
        sys.exit(__doc__)
    manifest, archives, game, out = args
    heads = {}
    if heads_path:
        for line in open(heads_path):
            p = line.split()
            if len(p) >= 4:
                heads[int(p[1], 16)] = bytes.fromhex(p[3])[:64]
    want = read_manifest(manifest)
    # what identifies a program's first 64 bytes: their hash (type 3) or the bytes (type 1 with --heads)
    by_key = {}
    for h, (size, head) in want.items():
        key = head if head is not None else heads.get(h)
        if key is not None:
            by_key.setdefault(key, []).append((h, size))
    found = {}
    for f in sorted(os.listdir(archives)):
        d = open(os.path.join(archives, f), "rb").read()
        for off in range(0, len(d) - 64, 256):
            first = d[off:off + 64]
            for key in (first, hash_bytes(first)):
                for h, size in by_key.get(key, ()):
                    if h not in found and hash_bytes(d[off:off + size]) == h:
                        found[h] = d[off:off + size]
    missing = [h for h in want if h not in found]
    if missing:  # the title's and the system's programs: in Misc.szs, not at 256-byte boundaries
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from archives import misc_blobs
        for blob in misc_blobs(game):
            for h in missing:
                if h in found:
                    continue
                size, head = want[h]
                key = heads.get(h)
                if key is not None:
                    i = blob.find(key)
                    while i >= 0:
                        if hash_bytes(blob[i:i + size]) == h:
                            found[h] = blob[i:i + size]
                            break
                        i = blob.find(key, i + 1)
                elif head is not None:
                    for i in range(0, len(blob) - 64, 4):
                        if hash_bytes(blob[i:i + 64]) == head and hash_bytes(blob[i:i + size]) == h:
                            found[h] = blob[i:i + size]
                            break
    with open(out, "wb") as f:
        for h, b in found.items():
            f.write(struct.pack("<QI", h, len(b)) + b)
    print(f"{len(found)} of {len(want)} programs found -> {out}")
    if len(found) < len(want):
        print(f"not found: {len(want) - len(found)}")


if __name__ == "__main__":
    main()
