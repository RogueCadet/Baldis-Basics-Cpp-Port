'use strict';
// bake-material-textures.js  -  map each material guid to its main texture guid.
//
// The 3D world pass needs wall/floor pixels, and a MeshRenderer references a MATERIAL, not a
// texture. data/materials.json (Phase 0) records only shader names, so this fills the gap:
//   data/material-textures.json  { <material guid>: { name, shader, tex, texRel } }
// `tex` is the texture guid (absent when the material has no main map), `texRel` its asset path.
//
// Unity stores these under m_SavedProperties.m_TexEnvs as a LIST of single-key maps, one per
// shader property. Property name varies by pipeline, so the first of _MainTex / _BaseMap /
// _BaseColorMap that carries a real texture wins -- checked in that order.
//
// Usage: node tools\bake-material-textures.js
const fs = require('fs');
const path = require('path');
const { splitDocs, parseBody } = require('./unity-yaml.js');

const ASSETS = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/';
const DATA = path.join(__dirname, '..', 'data');
const guidMap = JSON.parse(fs.readFileSync(path.join(DATA, 'manifest.json'), 'utf8')).guidMap;

// asset path -> guid (the manifest is keyed the other way)
const guidByRel = {};
for (const [guid, v] of Object.entries(guidMap)) guidByRel[v.rel] = guid;

function walk(dir, out) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.name.endsWith('.mat')) out.push(p);
  }
  return out;
}

const MAIN_PROPS = ['_MainTex', '_BaseMap', '_BaseColorMap'];
const SKYBOX_FACES = ['_FrontTex', '_BackTex', '_LeftTex', '_RightTex', '_UpTex', '_DownTex'];
const SKYBOX_KEYS = { _FrontTex: 'front', _BackTex: 'back', _LeftTex: 'left', _RightTex: 'right', _UpTex: 'up', _DownTex: 'down' };
const out = {};
const sky = {};                              // skybox material guid -> six face slots
let withTex = 0;
for (const file of walk(ASSETS, [])) {
  const rel = file.split(path.sep).join('/').slice(ASSETS.length);
  const guid = guidByRel[rel];
  if (!guid) continue;                       // not in the manifest (should not happen)
  let mat;
  try {
    const docs = splitDocs(fs.readFileSync(file, 'utf8'));
    const root = parseBody(docs[0].body);
    mat = root.Material || root;
  } catch (e) {
    console.error(`  FAILED ${rel}: ${e.message}`);
    continue;
  }
  const entry = { name: mat.m_Name || path.basename(file, '.mat') };
  const sh = mat.m_Shader;
  if (sh && sh.guid && guidMap[sh.guid]) entry.shader = guidMap[sh.guid].rel;

  // PER-MATERIAL ALPHA POLICY from Unity's own keyword set. m_ShaderKeywords names the shader
  // variant Unity selected for THIS material: _ALPHATEST_ON is Standard's cutout path (alpha
  // test at _Cutoff, default 0.5, depth-tested), _ALPHABLEND_ON / _ALPHAPREMULTIPLY_ON are the
  // blending paths. 11 game materials set _ALPHATEST_ON (both windows, the door materials,
  // fence). The renderer needs this flag: the window pane texture's alpha is BINARY (0 or 255,
  // nothing between), and without the test the world pass paints the transparent texels SOLID
  // -- the "dark far-corridor patch" is a door window doing exactly that.
  const kw = mat.m_ShaderKeywords;
  if (typeof kw === 'string') {
    if (kw.indexOf('_ALPHATEST_ON') >= 0) entry.alphaTest = true;
    if (kw.indexOf('_ALPHABLEND_ON') >= 0 || kw.indexOf('_ALPHAPREMULTIPLY_ON') >= 0) {
      entry.alphaBlend = true;
    }
  }

  // _Color: the material TINT, multiplied into the sampled texel. The schoolhouse materials are
  // 0.96 grey, not white -- measuring a real screenshot against ours showed a uniform 4.2%
  // brightness excess on every surface, which is exactly 1/0.96. Drawing with white ignores it.
  let cprops = null;                       // flattened m_Colors (hoisted for the skybox _Tint)
  const colors = mat.m_SavedProperties && mat.m_SavedProperties.m_Colors;
  if (Array.isArray(colors)) {
    cprops = {};
    for (const slot of colors) {
      if (!slot || typeof slot !== 'object') continue;
      for (const [k, v] of Object.entries(slot)) cprops[k] = v;
    }
    const c = cprops._Color || cprops._BaseColor;
    if (c && typeof c.r === 'number') {
      entry.colorR = c.r;
      entry.colorG = c.g;
      entry.colorB = c.b;
      entry.colorA = typeof c.a === 'number' ? c.a : 1;
    }
  }

  const texEnvs = mat.m_SavedProperties && mat.m_SavedProperties.m_TexEnvs;
  if (Array.isArray(texEnvs)) {
    const props = {};
    for (const slot of texEnvs) {
      if (!slot || typeof slot !== 'object') continue;
      for (const [k, v] of Object.entries(slot)) props[k] = v;
    }
    // SIX-SIDED SKYBOX MATERIALS. Shader 104 (Unity built-in) carries _FrontTex/_BackTex/
    // _LeftTex/_RightTex/_UpTex/_DownTex slots instead of _MainTex, and RenderSettings.
    // m_SkyboxMaterial references SKYBOX.mat (b0605a0225127614f801680af871da69) in School and
    // Secret. Detected BY THE SLOT SET, not by shader guid -- a material that maps all six
    // faces is a skybox regardless of which shader documents it.
    const faces = {};
    let isSkybox = false;
    for (const want of SKYBOX_FACES) {
      const t = props[want] && props[want].m_Texture;
      if (t && t.guid && guidMap[t.guid]) {
        faces[SKYBOX_KEYS[want]] = t.guid;
        isSkybox = true;
      }
    }
    const floats = {};
    if (isSkybox) {
      const mf = mat.m_SavedProperties.m_Floats;
      if (Array.isArray(mf)) {
        for (const slot of mf) {
          if (!slot || typeof slot !== 'object') continue;
          for (const [k, v] of Object.entries(slot)) floats[k] = v;
        }
      }
      // _Tint: the legacy 6-sided shader multiplies ALL faces by it (Skybox.mat authors
      // 0.502 grey). Default matches Unity's legacy shader fallback.
      const tint = cprops ? cprops._Tint : null;
      const num = (v, d) => (v && typeof v === 'number' ? v : d);
      sky[guid] = {
        name: entry.name,
        front: faces.front || '', back: faces.back || '',
        left: faces.left || '', right: faces.right || '',
        up: faces.up || '', down: faces.down || '',
        tintR: num(tint && tint.r, 0.5019608),
        tintG: num(tint && tint.g, 0.5019608),
        tintB: num(tint && tint.b, 0.5019608),
        rotation: num(floats._Rotation, 0),
        exposure: num(floats._Exposure, 1),
      };
    }
    for (const want of MAIN_PROPS) {
      const t = props[want] && props[want].m_Texture;
      if (t && t.guid && guidMap[t.guid]) {
        entry.tex = t.guid;
        entry.texRel = guidMap[t.guid].rel;
        entry.prop = want;
        // TILING AND OFFSET. Unity stores these beside the texture in the SAME TexEnv slot, and
        // the world pass needs them: WhiteBrickWall is m_Scale (2,2), so a quad that samples the
        // image once draws bricks at DOUBLE size. That is exactly the "textures look bigger and
        // lower quality" difference against a real screenshot -- one stretched copy instead of
        // the authored 2x2 tiling.
        const sc = props[want].m_Scale, of = props[want].m_Offset;
        entry.scaleX = sc && typeof sc.x === 'number' ? sc.x : 1;
        entry.scaleY = sc && typeof sc.y === 'number' ? sc.y : 1;
        entry.offsetX = of && typeof of.x === 'number' ? of.x : 0;
        entry.offsetY = of && typeof of.y === 'number' ? of.y : 0;
        withTex++;
        break;
      }
    }
  }
  out[guid] = entry;
}

fs.writeFileSync(path.join(DATA, 'material-textures.json'), JSON.stringify(out, null, 1));
fs.writeFileSync(path.join(DATA, 'skybox.json'), JSON.stringify(sky, null, 1));
const tiled = Object.values(out).filter((e) => e.tex && (e.scaleX !== 1 || e.scaleY !== 1)).length;
const skyCount = Object.keys(sky).length;
console.log(`materials: ${Object.keys(out).length}, with a main texture: ${withTex}, tiled (scale != 1): ${tiled}`);
console.log(`skybox materials: ${skyCount}${skyCount ? ' -> data/skybox.json (' + Object.values(sky).map((s) => s.name).join(', ') + ')' : ''}`);
console.log('  wrote data/material-textures.json [and data/skybox.json]');
