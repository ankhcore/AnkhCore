"""Builds an AnkhCore test package: the setup plus the optional components it may install, with COMPONENTS.sha256.

  pack_components.py --setup <AnkhCore_..._Setup.exe> --themes <IconThemes folder> --mod <icon-resource-redirect.wh.cpp>
                     --mod-original <Resource-Redirect-1.3.wh.cpp> --reg <AnkhCore_Taskbar_Profile.reg>
                     --out <new package folder> [--doc <file> ...] [--zip <file.zip>]

Layout:  <setup>.exe, COMPONENTS.sha256, IconThemes\\<theme>\\..., Mods\\icon-resource-redirect.wh.cpp,
         AnkhCore_Taskbar_Profile.reg, and the --doc files.
COMPONENTS.sha256 lists every component file (not the setup, not the docs): '<sha256 lower-case>  <relative path>'.
The setup checks each component against it before installing it. Refuses: an existing output folder, a theme folder
without theme.ini, a mod that differs from the supplied original, any path that is not printable ASCII.
"""
import argparse, hashlib, os, shutil, sys, zipfile


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fail(msg):
    print('REFUSED: ' + msg)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--setup', required=True)
    ap.add_argument('--themes', required=True)
    ap.add_argument('--mod', required=True)
    ap.add_argument('--mod-original', required=True)
    ap.add_argument('--reg', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--doc', action='append', default=[])
    ap.add_argument('--zip')
    ap.add_argument('--smartctl', help='optional: a 64-bit smartctl.exe (smartmontools, GPL-2.0), staged as Tools/smartctl.exe')
    ap.add_argument('--smartctl-license', help='the smartmontools COPYING file, required with --smartctl; staged as Tools/COPYING.smartmontools')
    a = ap.parse_args()

    if os.path.exists(a.out) and os.listdir(a.out):
        fail(f'{a.out} exists and is not empty (a package is never overwritten)')
    for p in (a.setup, a.mod, a.mod_original, a.reg, *a.doc):
        if not os.path.isfile(p):
            fail(f'input not found: {p}')
    if not os.path.isdir(a.themes):
        fail(f'themes folder not found: {a.themes}')
    if sha256(a.mod) != sha256(a.mod_original):
        fail('the Resource Redirect mod differs from the supplied original source (it must be a verbatim copy)')

    if bool(a.smartctl) != bool(a.smartctl_license):
        fail('--smartctl and --smartctl-license go together (the GPL-2.0 text must ship with the program)')
    if a.smartctl:
        for p in (a.smartctl, a.smartctl_license):
            if not os.path.isfile(p):
                fail(f'input not found: {p}')
        with open(a.smartctl, 'rb') as f:
            head = f.read(0x400)
        pe = int.from_bytes(head[0x3C:0x40], 'little') if len(head) >= 0x40 else 0
        if head[:2] != b'MZ' or pe + 6 > len(head) or head[pe:pe + 2] != b'PE' or int.from_bytes(head[pe + 4:pe + 6], 'little') != 0x8664:
            fail('smartctl.exe is not a 64-bit Windows executable')
        if b'GNU GENERAL PUBLIC LICENSE' not in open(a.smartctl_license, 'rb').read():
            fail('the smartctl license file is not the GPL text')
    os.makedirs(a.out, exist_ok=True)
    shutil.copy2(a.setup, os.path.join(a.out, os.path.basename(a.setup)))
    entries = []

    themes = sorted(d for d in os.listdir(a.themes) if os.path.isdir(os.path.join(a.themes, d)))
    for t in themes:
        src = os.path.join(a.themes, t)
        if not os.path.isfile(os.path.join(src, 'theme.ini')):
            fail(f'theme {t} has no theme.ini')
        for root, _, files in os.walk(src):
            for f in sorted(files):
                s = os.path.join(root, f)
                rel = os.path.join('IconThemes', t, os.path.relpath(s, src))
                d = os.path.join(a.out, rel)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d)
                entries.append(rel)
    smart = ((a.smartctl, os.path.join('Tools', 'smartctl.exe')), (a.smartctl_license, os.path.join('Tools', 'COPYING.smartmontools'))) if a.smartctl else ()
    for src, rel in smart + ((a.mod, os.path.join('Mods', 'icon-resource-redirect.wh.cpp')), (a.reg, 'AnkhCore_Taskbar_Profile.reg')):
        d = os.path.join(a.out, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(src, d)
        entries.append(rel)
    for doc in a.doc:
        shutil.copy2(doc, os.path.join(a.out, os.path.basename(doc)))

    lines = ['# AnkhCore optional components. The setup installs a component only if every file below is present and unchanged.']
    total = 0
    for rel in sorted(entries, key=str.lower):
        rel = rel.replace('/', '\\')
        if any(ord(c) < 0x20 or ord(c) > 0x7E for c in rel):
            fail(f'path is not printable ASCII: {rel}')
        p = os.path.join(a.out, rel)
        total += os.path.getsize(p)
        lines.append(f'{sha256(p)}  {rel}')
    with open(os.path.join(a.out, 'COMPONENTS.sha256'), 'w', encoding='ascii', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'package: {a.out}')
    print(f'themes: {len(themes)} ({", ".join(themes)})')
    print(f'component files: {len(entries)}, {total / (1 << 20):.1f} MB; manifest COMPONENTS.sha256')

    if a.zip:
        if os.path.exists(a.zip):
            fail(f'{a.zip} exists (never overwritten)')
        with zipfile.ZipFile(a.zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for root, _, files in os.walk(a.out):
                for f in sorted(files):
                    s = os.path.join(root, f)
                    z.write(s, os.path.join(os.path.basename(os.path.normpath(a.out)), os.path.relpath(s, a.out)))
        print(f'zip: {a.zip}, {os.path.getsize(a.zip) / (1 << 20):.1f} MB, sha256 {sha256(a.zip)}')


if __name__ == '__main__':
    main()
