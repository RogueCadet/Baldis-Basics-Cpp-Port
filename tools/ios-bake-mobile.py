# Convert the genuine iOS MobileControls hierarchy (ios-ref/ios-mobilecontrols.json, dumped from the
# iOS School level by tools/ios-dump-hierarchy.py) into the port's prefab bake format:
# data/prefab-MobileControls.bake.json. The engine instantiates it into School when running as the
# iOS build (eng::State::mobile).
#
# Ids: the iOS objects' own pathIDs (unique within the level), so every in-level object reference in
# a component's fields ({m_FileID 0, m_PathID N} -> {fileID N}) carries over unchanged.
# Asset refs (sprites/textures in other files) become {guid "ios_<file>_<pathID>"}, the keys
# tools/ios-export-assets.py writes into data/tex + textures-index.json.
# Field names are Unity's serialized names, the same ones the port's own bakes use.
import json
import os

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dump = json.load(open(os.path.join(PORT, 'ios-ref', 'ios-mobilecontrols.json')))
_ap = os.path.join(PORT, 'data', 'ios-asset-map.json')
AMAP = json.load(open(_ap)) if os.path.exists(_ap) else {}   # tools/ios-export-assets.py
# level2 externals (fileID 1..6), as listed by the dump tool's source file
EXTERNALS = ['globalgamemanagers.assets', 'sharedassets2.assets', 'library/unity default resources',
             'sharedassets0.assets', 'sharedassets1.assets', 'resources.assets']
SCRIPT_NAMES = {
    'UnityEngine.UI.Image': 'Image', 'UnityEngine.UI.RawImage': 'RawImage', 'UnityEngine.UI.Text': 'Text',
    'UnityEngine.UI.CanvasScaler': 'CanvasScaler', 'UnityEngine.UI.GraphicRaycaster': 'GraphicRaycaster',
    'MaterialKit.DpCanvasScaler': 'DpCanvasScaler',
    'Rewired.ComponentControls.TouchController': 'RewiredTouchController',
    'Rewired.ComponentControls.TouchJoystick': 'RewiredTouchJoystick',
    'Rewired.ComponentControls.TouchPad': 'RewiredTouchPad',
    'Rewired.ComponentControls.TouchButton': 'RewiredTouchButton',
    'Rewired.ComponentControls.TouchRegion': 'RewiredTouchRegion',
    'Rewired.ComponentControls.Effects.TouchInteractableTransitioner': 'RewiredTouchInteractableTransitioner',
}


def conv(v, key=None):
    """Unity PPtr dicts -> port refs; everything else verbatim."""
    if isinstance(v, dict):
        if set(v.keys()) == {'m_FileID', 'm_PathID'}:
            fid, pid = v['m_FileID'], v['m_PathID']
            if pid == 0:
                return {'fileID': 0}
            if fid == 0 and key not in ('m_Sprite', 'm_Texture', 'm_Material'):
                return {'fileID': pid}
            fname = 'level2' if fid == 0 else EXTERNALS[fid - 1].split('/')[-1]
            key = 'ios_%s_%d' % (fname.replace('.', '_'), pid)
            return {'fileID': 21300000, 'guid': AMAP.get(key, key), 'type': 3}   # PC guid if byte-identical
        return {k: conv(x, k) for k, x in v.items()}
    if isinstance(v, list):
        return [conv(x, key) for x in v]
    return v


objects, comps = {}, {}


def emit(rec, father):
    go = str(rec['pathID'])
    o = {'n': rec['name'], 'tag': 'Untagged', 'layer': rec.get('layer', 5), 'active': rec['active'], 'comps': []}
    objects[go] = o
    xf = rec['xform']
    tid = str(xf['pathID'])
    t = {'t': 'RectTransform' if xf['rect'] else 'Transform', 'go': go,
         'pos': dict(zip('xyz', xf['pos'])), 'rot': dict(zip('xyzw', xf['rot'])), 'scale': dict(zip('xyz', xf['scale'])),
         'father': father, 'children': [], 'rootOrder': 0}
    if xf['rect']:
        r = rec['rect']
        t.update({'anchorMin': dict(zip('xy', r['amin'])), 'anchorMax': dict(zip('xy', r['amax'])),
                  'anchoredPosition': dict(zip('xy', r['pos'])), 'sizeDelta': dict(zip('xy', r['size'])),
                  'pivot': dict(zip('xy', r['pivot']))})
    comps[tid] = t
    o['comps'].append(tid)
    for c in rec['comps']:
        s = c['script']
        cid = str(c['pathID'])
        f = c.get('fields') or {}
        if s == 'Canvas':
            comps[cid] = {'t': 'Canvas', 'go': go, 'enabled': bool(f.get('m_Enabled', 1)), 'renderMode': f.get('m_RenderMode', 0),
                          'camera': '0', 'sortingLayer': 0, 'sortingOrder': f.get('m_SortingOrder', 0)}
        elif s == 'CanvasRenderer':
            comps[cid] = {'t': 'CanvasRenderer', 'go': go}
        else:
            comps[cid] = {'t': 'Script', 'go': go, 'script': SCRIPT_NAMES.get(s, s), 'f': conv(f)}
        o['comps'].append(cid)
    for i, ch in enumerate(rec['children']):
        cid_t = emit(ch, tid)
        comps[cid_t]['rootOrder'] = i
        t['children'].append(cid_t)
    return tid


# INVISIBLE CONTROLS (Rewired_Core TouchInteractable / TouchInteractableTransitioner, decompiled):
# a control with _visible 0 hides (1) its _targetGraphic -- the ColorTint transition writes alpha 0
# while invisible -- and (2) the graphic of every TouchInteractableTransitioner in its hierarchy,
# which receives the visibility broadcast. Emitted per control as paths relative to it ("" = self).
comp_owner = {}


def index_tree(r, path):
    for c in r['comps']:
        comp_owner[c['pathID']] = (r, path)
    for ch in r['children']:
        index_tree(ch, path + [ch['name']])


index_tree(dump, [])


def hide_list(ctrl_rec, ctrl_comp, ctrl_path):
    out = []
    tg = (ctrl_comp.get('fields') or {}).get('_targetGraphic') or {}
    if tg.get('m_PathID') in comp_owner:
        out.append('/'.join(comp_owner[tg['m_PathID']][1][len(ctrl_path):]))

    def scan(r, rel):
        if any(c['script'].endswith('TouchInteractableTransitioner') for c in r['comps']):
            out.append('/'.join(rel))
        for ch in r['children']:
            scan(ch, rel + [ch['name']])
    scan(ctrl_rec, [])
    return sorted(set(out))


def annotate(r, path):
    for c in r['comps']:
        if c['script'].startswith('Rewired.ComponentControls.Touch') and isinstance(c.get('fields'), dict):
            c['fields']['_hideOnInvisible'] = hide_list(r, c, path)
    for ch in r['children']:
        annotate(ch, path + [ch['name']])


annotate(dump, [])


# COLOUR TINT (Rewired TouchInteractable / TouchInteractableTransitioner, _transitionType bit 1):
# the control tints its _targetGraphic, and every ColorTint transitioner in its hierarchy tints its
# own _targetGraphic, with the colours serialized on whichever component does the tinting. Unity
# multiplies the graphic's colour by the state colour (CanvasRenderer colour); emitted per control
# as {path relative to the control, normal/highlighted/pressed/disabled RGBA, multiplier, fade}.
def tint_entry(comp, path_from_ctrl):
    ct = comp['fields'].get('_transitionColorTint') or {}
    col = lambda k: [ct[k][x] for x in 'rgba'] if isinstance(ct.get(k), dict) else [1, 1, 1, 1]
    return {'path': path_from_ctrl, 'normal': col('m_NormalColor'), 'highlighted': col('m_HighlightedColor'),
            'pressed': col('m_PressedColor'), 'disabled': col('m_DisabledColor'),
            'multiplier': ct.get('m_ColorMultiplier', 1.0), 'fade': ct.get('m_FadeDuration', 0.1)}


def tint_list(ctrl_rec, ctrl_comp, ctrl_path):
    out = []
    f = ctrl_comp['fields']
    tg = f.get('_targetGraphic') or {}
    if (f.get('_transitionType', 0) & 1) and tg.get('m_PathID') in comp_owner:
        out.append(tint_entry(ctrl_comp, '/'.join(comp_owner[tg['m_PathID']][1][len(ctrl_path):])))

    def scan(r, rel):
        for c in r['comps']:
            if c['script'].endswith('TouchInteractableTransitioner') and isinstance(c.get('fields'), dict):
                cf = c['fields']
                ttg = cf.get('_targetGraphic') or {}
                if (cf.get('_transitionType', 0) & 1) and ttg.get('m_PathID') in comp_owner:
                    out.append(tint_entry(c, '/'.join(comp_owner[ttg['m_PathID']][1][len(ctrl_path):])))
        for ch in r['children']:
            scan(ch, rel + [ch['name']])
    scan(ctrl_rec, [])
    return out


def annotate_tint(r, path):
    for c in r['comps']:
        if c['script'].startswith('Rewired.ComponentControls.Touch') and not c['script'].endswith(('Controller', 'Region')) \
                and isinstance(c.get('fields'), dict):
            c['fields']['_tintTargets'] = tint_list(r, c, path)
    for ch in r['children']:
        annotate_tint(ch, path + [ch['name']])


annotate_tint(dump, [])
emit(dump, '0')
out = {'name': 'MobileControls', 'roots': [str(dump['pathID'])], 'objects': objects, 'components': comps, 'assets': {}}
json.dump(out, open(os.path.join(PORT, 'data', 'prefab-MobileControls.bake.json'), 'w'), indent=1)
kinds = {}
for c in comps.values():
    k = c.get('script', c['t'])
    kinds[k] = kinds.get(k, 0) + 1
print('prefab-MobileControls: %d objects, %d components %s' % (len(objects), len(comps), kinds))
