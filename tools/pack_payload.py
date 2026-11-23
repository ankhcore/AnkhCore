"""Builds the AnkhCore 2.5 core installer payload (ANKPK2) from installer/payload.json. Runs on Windows.

  pack_payload.py --app <built AnkhCore.exe> --inputs <release-inputs folder> --out <payload.bin>

Refuses (exit 2): an entry whose asset status is not allowed, an input whose SHA-256 differs from the
manifest, a missing input, or a payload path outside APP/ and SENS/. Output is deterministic (fixed
order, fixed deflate settings): the same inputs give a byte-identical payload. Writes MANIFEST (SHA-256
of every file, verified by the setup after writing) into the payload and <out>.lock.txt beside it.
"""
import argparse, hashlib, json, os, re, struct, sys, zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def fail(msg):
    print('REFUSED: ' + msg)
    sys.exit(2)


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_DEFAULT_STRATEGY)
    return c.compress(data) + c.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--app', required=True)
    ap.add_argument('--inputs', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    man = json.load(open(os.path.join(REPO, 'installer', 'payload.json'), encoding='utf-8'))
    allowed = set(man['allowed_status'])
    files = []
    for e in man['entries']:
        p, src, status = e['path'], e['from'], e['status']
        if status not in allowed:
            fail(f'{p}: asset status "{status}" is not allowed in a release (docs/ASSETS.md)')
        if not re.fullmatch(r'(APP|SENS|SUX|THEMETOOLS|UNIGET)/([A-Za-z0-9._\- ]+(/[A-Za-z0-9._\- ]+)*/?)?', p) or '..' in p:
            fail(f'{p}: not a core payload path')
        kind, _, name = src.partition(':')
        sux_root = os.path.join(os.path.dirname(os.path.abspath(a.inputs)), 'SUX')
        theme_root = os.path.dirname(REPO)
        path = {'build': a.app, 'repo': os.path.join(REPO, name), 'inputs': os.path.join(a.inputs, name), 'sux': os.path.join(sux_root, name), 'theme': os.path.join(theme_root, name), 'apps': os.path.join(theme_root, 'apps', name), 'fancontrol': os.path.join(theme_root, 'Fan control', name)}.get(kind)
        if not path or not os.path.isfile(path):
            fail(f'{p}: input not found ({src} -> {path})')
        data = open(path, 'rb').read()
        sha = hashlib.sha256(data).hexdigest()
        if 'sha256' in e and e['sha256'] != sha:
            fail(f'{p}: SHA-256 {sha} does not match the manifest ({e["sha256"]})')
        if e.get('zip'):   # a folder shipped as the contents of a verified zip (2.7: UniGetUI)
            import zipfile, io
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                for zi in sorted(zf.infolist(), key=lambda i: i.filename):
                    if zi.is_dir():
                        continue
                    zn = zi.filename[len(e['strip']):] if e.get('strip') and zi.filename.startswith(e['strip']) else zi.filename
                    if not zn: continue
                    zp = p + zn
                    if not re.fullmatch(r'[A-Za-z0-9._\-(), ]+(/[A-Za-z0-9._\-(), ]+)*', zn) or '..' in zp:
                        fail(f'{zp}: not a safe payload path')
                    zd = zf.read(zi)
                    files.append((zp, zd, hashlib.sha256(zd).hexdigest()))
            continue
        files.append((p, data, sha))
    if len({p for p, _, _ in files}) != len(files):
        fail('duplicate payload paths')
    files.sort(key=lambda f: f[0])
    manifest = ''.join(f'{sha}  {p}\n' for p, _, sha in files).encode('utf-8')
    entries = []
    for p, data in [(p, d) for p, d, _ in files] + [('MANIFEST', manifest)]:
        z = deflate(data)
        entries.append((p, 1, len(data), z) if len(z) < len(data) * 0.95 else (p, 0, len(data), data))
    with open(a.out, 'wb') as fo:
        fo.write(b'ANKPK2\0\0' + struct.pack('<I', len(entries)))
        for p, m, raw, data in entries:
            pb = p.encode('utf-8')
            fo.write(struct.pack('<I', len(pb)) + pb + bytes([m]) + struct.pack('<QQ', raw, len(data)) + data)
    payload_sha = hashlib.sha256(open(a.out, 'rb').read()).hexdigest()
    with open(a.out + '.lock.txt', 'w', encoding='utf-8', newline='\n') as fo:
        fo.write(f'payload {payload_sha}  {os.path.getsize(a.out)} bytes\n')
        for p, data, sha in files:
            fo.write(f'{sha}  {len(data):10d}  {p}\n')
    print(f'payload: {len(files)} files + MANIFEST, {os.path.getsize(a.out) / 2**20:.1f} MB, sha256 {payload_sha}')


if __name__ == '__main__':
    main()
