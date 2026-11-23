# Rebuild res/ for app.rc from resources extracted out of the shipped AnkhCore.exe.
# usage: mkres.py <rsrc_dir> <res_out_dir>
import os, struct, sys, shutil
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
grp = open(os.path.join(src, 'GROUP_ICON', '1'), 'rb').read()
_, _, count = struct.unpack('<HHH', grp[:6])
entries, images = [], []
for i in range(count):
    w, h, cc, r, planes, bpp, size, nid = struct.unpack('<BBBBHHIH', grp[6 + i * 14: 6 + i * 14 + 14])
    p = os.path.join(src, 'ICON', f'{nid}.ico')
    if not os.path.exists(p):
        p = os.path.join(src, 'ICON', f'{nid}')
    data = open(p, 'rb').read()
    if data[:4] == b'\x00\x00\x01\x00' and struct.unpack('<H', data[4:6])[0] == 1:
        data = data[22:]   # 7-Zip wraps each RT_ICON in a one-image ICO header
    entries.append((w, h, cc, r, planes, bpp, len(data)))
    images.append(data)
hdr = struct.pack('<HHH', 0, 1, count)
off = 6 + 16 * count
dirs = b''
for (w, h, cc, r, planes, bpp, size) in entries:
    dirs += struct.pack('<BBBBHHII', w, h, cc, r, planes, bpp, size, off)
    off += size
open(os.path.join(out, 'ankh.ico'), 'wb').write(hdr + dirs + b''.join(images))
m = {'BGLOGO': 'bglogo.png', 'PV_HUD': 'pv_hud.jpg', 'PV_TECH': 'pv_tech.jpg', 'PV_MAGNUM': 'pv_magnum.jpg',
     'PV_SWEET': 'pv_sweet.jpg', 'PV_BIGSUR_DARK': 'pv_bigsur_dark.jpg', 'PV_BIGSUR_LIGHT': 'pv_bigsur_light.jpg',
     'PV_BIGSUR': 'pv_bigsur.jpg'}
for k, v in m.items():
    shutil.copyfile(os.path.join(src, 'RCDATA', k), os.path.join(out, v))
print('icon images:', count, [e[:2] for e in entries])
