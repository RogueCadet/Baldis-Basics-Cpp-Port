# Export the sprites / textures an iOS hierarchy dump references into the port's texture format.
# Input: a dump from tools/ios-dump-hierarchy.py (PPtrs are relative to the dumped level's
# externals table). For every Image.m_Sprite / RawImage.m_Texture it decodes the genuine iOS asset
# with UnityPy and writes data/tex/<key>.raw (RGBA8, row 0 = image TOP, the port's convention) and
# merges {w, h, point, mip} into data/textures-index.json. Keys are "ios_<assetsfile>_<pathID>":
# opaque to the engine, which treats texture guids as strings.
#   point: the Texture2D's m_TextureSettings.m_FilterMode == 0;  mip: m_MipCount > 1.
# A Sprite exports its own rect (sprite.image), so atlas-packed sprites come out standalone.
# usage: py -3.10 -I tools/ios-export-assets.py <pkgdir> <level> <dump.json>
import json
import os
import sys

sys.path.insert(0, sys.argv[1])
import UnityPy  # noqa: E402

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = 'F:/baldidecomp/mobile/BALDI_1.4.3_iOS/Payload/BaldisBasicsClassic.app'
LEVEL, DUMP = sys.argv[2], sys.argv[3]
env = UnityPy.load(APP + '/Data')
byname = {k.replace('\\', '/').split('/')[-1]: f for k, f in env.files.items()}
lv = byname[LEVEL]


def target(fid, pid):
    sf = lv if fid == 0 else byname.get(lv.externals[fid - 1].path.split('/')[-1])
    name = LEVEL if fid == 0 else lv.externals[fid - 1].path.split('/')[-1]
    return (sf.objects.get(pid) if sf is not None else None), name


refs = set()


def scan(v):
    # every cross-file PPtr (a field such as ItemImageScript.noItemSprite is one too); only the ones
    # that resolve to a Texture2D / Sprite are exported below
    if isinstance(v, dict):
        if set(v.keys()) == {'m_FileID', 'm_PathID'}:
            if v['m_PathID']:
                refs.add((v['m_FileID'], v['m_PathID']))
            return
        for x in v.values():
            scan(x)
    elif isinstance(v, list):
        for x in v:
            scan(x)


def walk(r):
    for c in r['comps']:
        if isinstance(c.get('fields'), dict):
            scan(c['fields'])
    for ch in r['children']:
        walk(ch)


walk(json.load(open(DUMP)))
idx_path = os.path.join(PORT, 'data', 'textures-index.json')
index = json.load(open(idx_path))
os.makedirs(os.path.join(PORT, 'data', 'tex'), exist_ok=True)
# IDENTITY: an iOS image byte-identical to a texture the PC bake already has IS that texture. Scripts
# compare textures by object (ItemImageScript: slot texture == blankSprite), so the iOS key must
# resolve to the PC guid's object. data/ios-asset-map.json records ios key -> PC guid.
import hashlib
pc_by_hash = {}
for g, v in index.items():
    if g.startswith('ios_'):
        continue
    fp = os.path.join(PORT, 'data', 'tex', g + '.raw')
    if os.path.exists(fp) and os.path.getsize(fp) == v['w'] * v['h'] * 4:
        pc_by_hash[(v['w'], v['h'], hashlib.md5(open(fp, 'rb').read()).hexdigest())] = g
amap_path = os.path.join(PORT, 'data', 'ios-asset-map.json')
amap = json.load(open(amap_path)) if os.path.exists(amap_path) else {}
for fid, pid in sorted(refs):
    obj, fname = target(fid, pid)
    if obj is None or obj.type.name not in ('Sprite', 'Texture2D'):
        continue
    key = 'ios_%s_%d' % (fname.replace('.', '_'), pid)
    data = obj.read()
    if obj.type.name == 'Sprite':
        img = data.image
        tex = data.m_RD.texture.read()
    else:
        img = data.image
        tex = data
    img = img.convert('RGBA')
    w, h = img.size
    raw = img.tobytes()   # PIL rows = top first, the port's convention
    same = pc_by_hash.get((w, h, hashlib.md5(raw).hexdigest()))
    if same:
        amap[key] = same
        print('%-40s == PC texture %s (byte-identical %dx%d, %s)' % (key, same, w, h, getattr(data, 'm_Name', '')))
        continue
    open(os.path.join(PORT, 'data', 'tex', key + '.raw'), 'wb').write(raw)
    ts = tex.m_TextureSettings
    index[key] = {'w': w, 'h': h, 'point': 1 if ts.m_FilterMode == 0 else 0, 'mip': 1 if getattr(tex, 'm_MipCount', 1) > 1 else 0}
    print('%-40s %-8s %4dx%-4d %s filter=%d mips=%d  (%s)' % (key, obj.type.name, w, h, getattr(data, 'm_Name', ''), ts.m_FilterMode, getattr(tex, 'm_MipCount', 1), tex.m_TextureFormat))
json.dump(index, open(idx_path, 'w'), indent=1)
json.dump(amap, open(amap_path, 'w'), indent=1)
print('scanned %d asset refs; textures written + identity map updated' % len(refs))
