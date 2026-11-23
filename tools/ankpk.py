"""ANKPK2 package tool (read-only on its inputs).

  ankpk.py list    <setup.exe | payload.bin>              list entries, grouped by top folder
  ankpk.py extract <setup.exe | payload.bin> <out dir> [prefix ...]   unpack entries (optionally only these prefixes)

ANKPK2 layout (written by build_payload.py / tools\\pack_payload.py, read by base.h ParsePackage):
  b'ANKPK2\\0\\0' + u32 count, then per entry:
  u32 path_len, path (UTF-8, '/'-separated), u8 method (0 stored, 1 raw deflate), u64 raw_size, u64 data_size, data
A setup.exe carries the package as its PAYLOAD resource; it is located by its magic (the exe is only read).
"""
import collections, os, struct, sys, zlib

MAGIC = b'ANKPK2\0\0'


def load(path):
    d = open(path, 'rb').read()
    o = d.find(MAGIC)
    while o >= 0:   # the magic also appears as a string constant in the setup's code: take the first that parses
        try:
            return parse(d, o)
        except (UnicodeDecodeError, struct.error, IndexError, ValueError):
            o = d.find(MAGIC, o + 1)
    sys.exit('no ANKPK2 package in ' + path)


def parse(d, o):
    n = struct.unpack_from('<I', d, o + 8)[0]
    if n > 100000:
        raise ValueError('implausible entry count')
    o += 12
    entries = []
    for _ in range(n):
        ln = struct.unpack_from('<I', d, o)[0]; o += 4
        p = d[o:o + ln].decode('utf-8'); o += ln
        m = d[o]; o += 1
        raw, size = struct.unpack_from('<QQ', d, o); o += 16
        if m > 1 or o + size > len(d):
            raise ValueError('bad entry')
        entries.append((p, m, raw, d[o:o + size])); o += size
    return entries


def unpack(m, raw, data):
    out = zlib.decompress(data, -15) if m == 1 else data
    if len(out) != raw:
        raise ValueError('size mismatch')
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, src = sys.argv[1], sys.argv[2]
    entries = load(src)
    if cmd == 'list':
        c, rw = collections.Counter(), collections.Counter()
        for p, m, raw, data in entries:
            k = p.split('/')[0]; c[k] += 1; rw[k] += raw
        for k in sorted(c):
            print(f'{k:6s} {c[k]:5d} files {rw[k] / 2**20:8.1f} MB')
        print(f'total  {len(entries):5d} files {sum(rw.values()) / 2**20:8.1f} MB')
        if '-v' in sys.argv:
            for p, m, raw, data in entries:
                print(f'{raw:12d}  {p}')
    elif cmd == 'extract':
        out = sys.argv[3]; prefixes = sys.argv[4:]
        for p, m, raw, data in entries:
            if prefixes and not any(p.startswith(x) for x in prefixes):
                continue
            dest = os.path.join(out, *p.split('/'))
            if os.path.commonpath([os.path.abspath(dest), os.path.abspath(out)]) != os.path.abspath(out):
                sys.exit('unsafe path in package: ' + p)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, 'wb').write(unpack(m, raw, data))
        print('extracted to', out)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
