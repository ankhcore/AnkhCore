"""Release asset gate for embedded resources: every file referenced by src/app.rc and src/setup.rc must
be on the allowlist below (status "ship" in docs/ASSETS.md). Exit 2 lists the offenders.

  check_release_assets.py
"""
import os, re, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
# file referenced by a .rc -> reason it may ship (keep in step with docs/ASSETS.md)
ALLOWED = {
    'res/ankh.ico': 'official AnkhCore mark (ship)',
    'res/ankh256.png': 'official AnkhCore mark, 256 px frame of res/ankh.ico (ship)',
    'res/brand_logo.png': 'official AnkhCore logo (transparent), copy of D:/Ankh core/Ankhcore Desigen/logo.png (ship, owner 2026-11-14; replaces brand_logo.jpg)',
    'res/brand_background.png': 'Main Window and Candy background, sharp original supplied 2026-11-16, see docs/ASSETS.md (ship, D44: owner accepts the trademark risk)',
    'app.manifest': 'own file',
    'setup.manifest': 'own file',
    'payload.bin': 'core payload, gated by tools/pack_payload.py',
}


def main():
    bad = []
    for rc in ('app.rc', 'setup.rc'):
        text = open(os.path.join(REPO, 'src', rc), encoding='utf-8').read()
        for m in re.finditer(r'^\s*\S+\s+(?:RCDATA|ICON|RT_MANIFEST|BITMAP)\s+"([^"]+)"', text, re.M):
            f = m.group(1).replace('\\', '/')
            if f not in ALLOWED:
                bad.append(f'{rc}: {f}')
            else:
                print(f'ok   {rc}: {f}  ({ALLOWED[f]})')
    if bad:
        print('REFUSED (not cleared for release in docs/ASSETS.md):\n  ' + '\n  '.join(bad))
        sys.exit(2)
    print('release resources: all cleared')


if __name__ == '__main__':
    main()
