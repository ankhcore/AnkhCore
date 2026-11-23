import struct, zlib, os, sys, time
S=sys.argv[1]; out=sys.argv[2]
entries=[]
def add(path, data, compress=True):
    if compress:
        c=zlib.compressobj(9, zlib.DEFLATED, -15); z=c.compress(data)+c.flush()
        if len(z) < len(data)*0.95: entries.append((path,1,len(data),z)); return
    entries.append((path,0,len(data),data))
# old payload: REG, SRC, M64, M32, SUX, WIN
d=open(S+'/inst35b/payload.bin','rb').read()
n=struct.unpack_from('<I',d,8)[0]; o=12
for i in range(n):
    l=struct.unpack_from('<I',d,o)[0]; o+=4; p=d[o:o+l].decode(); o+=l
    s=struct.unpack_from('<Q',d,o)[0]; o+=8; data=d[o:o+s]; o+=s
    top=p.split('/')[0]
    if top in ('REG','SRC','M64','M32','SUX','WIN'): add(p,data)
add('WH/windhawk_setup.exe', open('/mnt/user-data/uploads/windhawk_setup_offline_1.exe','rb').read(), False)
add('THEME/As3aD.theme', open(S+'/ankh/stage/theme/As3aD.theme','rb').read())
for f in sorted(os.listdir(S+'/wall_anubis')):
    add('WALL/'+f, open(S+'/wall_anubis/'+f,'rb').read(), False)
add('APP/AnkhCore.exe', open(S+'/ankh/AnkhCore.exe','rb').read())
# --- 2.2: hardware monitoring (AnkhSensors C# source, LHM, PawnIO, NBFC configs) ---
import os as _os
_sdir=S+'/ankh/sens_stage'
for _root,_dirs,_files in _os.walk(_sdir):
    for _f in sorted(_files):
        _full=_os.path.join(_root,_f)
        _rel=_os.path.relpath(_full,_sdir).replace(chr(92),'/')
        add('SENS/'+_rel, open(_full,'rb').read())

# --- 2.2: icons applied by the bundled 7tsp (exe + packs already zipped) ---
_tdir=S+'/tsp_stage'
for _f in sorted(_os.listdir(_tdir)):
    add('TSP/'+_f, open(_os.path.join(_tdir,_f),'rb').read(), False)  # already compressed
with open(out,'wb') as fo:
    fo.write(b'ANKPK2\0\0'+struct.pack('<I',len(entries)))
    for p,m,raw,data in entries:
        pb=p.encode('utf-8'); fo.write(struct.pack('<I',len(pb))+pb+bytes([m])+struct.pack('<QQ',raw,len(data))); fo.write(data)
import collections
c=collections.Counter(); sz=collections.Counter(); rw=collections.Counter()
for p,m,raw,data in entries: k=p.split('/')[0]; c[k]+=1; sz[k]+=len(data); rw[k]+=raw
for k in sorted(c): print(f'{k:6s} {c[k]:4d} {rw[k]/2**20:7.1f} MB -> {sz[k]/2**20:7.1f} MB')
print('total', len(entries), os.path.getsize(out)/2**20, 'MB')
