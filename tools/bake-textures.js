'use strict';
// bake-textures.js  -  decode the uGUI textures into raw RGBA8 blobs the engine can load directly.
//
// The C++ engine must never decode an image format: the iOS build stays dependency-free. So pixels
// are baked here, exactly like the font pages were (data/font-*.atlas). Output:
//   data/tex/<guid>.raw       raw RGBA8, width*height*4 bytes, row 0 = image TOP
//   data/textures-index.json  { guid: { rel, w, h } }  -- the engine's lookup
//
// Scope: textures used by uGUI Image/RawImage AND by MeshRenderer materials (the world pass).
// The 3D character billboards
// (Crafters/Playtime sprite sheets, ~120MB raw) are still excluded until
// the world renderer needs them -- baking everything would be 149MB.
//
// Usage: node tools\bake-textures.js [--max-dim N]
//   --max-dim halves oversized textures (box filter) until they fit; off by default.
const fs = require('fs');
const path = require('path');
const { decodePng } = require('./png-decode.js');

const ASSETS = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/';
const DATA = path.join(__dirname, '..', 'data');
const OUT = path.join(DATA, 'tex');

const argv = process.argv.slice(2);
const maxDimArg = argv.indexOf('--max-dim');
const maxDim = maxDimArg >= 0 ? parseInt(argv[maxDimArg + 1], 10) : 0;

const guidMap = JSON.parse(fs.readFileSync(path.join(DATA, 'manifest.json'), 'utf8')).guidMap;

// ---- collect the guids uGUI actually draws ---------------------------------
const wanted = new Map();                       // guid -> rel
const bakes = fs.readdirSync(DATA).filter((f) => /^(scene|prefab)-.*\.bake\.json$/.test(f));
for (const file of bakes) {
  const doc = JSON.parse(fs.readFileSync(path.join(DATA, file), 'utf8'));
  for (const c of Object.values(doc.components || {})) {
    if (c.t !== 'Script' || !c.f) continue;
    if (c.script !== 'Image' && c.script !== 'RawImage') continue;
    for (const key of ['m_Sprite', 'm_Texture']) {
      const ref = c.f[key];
      if (!ref || !ref.guid) continue;
      const gm = guidMap[ref.guid];
      if (gm && /\.png$/i.test(gm.rel)) wanted.set(ref.guid, gm.rel);
    }
  }
}

// MeshRenderer materials -> their main texture (the 3D world pass). A renderer references a
// MATERIAL, so this needs the material-textures index as a hop. Written as a second source
// feeding the same guid set, which is why the bake was built around a guid list rather than a
// hard-coded uGUI scope.
const matTexPath = path.join(DATA, 'material-textures.json');
const matTex = fs.existsSync(matTexPath) ? JSON.parse(fs.readFileSync(matTexPath, 'utf8')) : {};
for (const file of bakes) {
  const doc = JSON.parse(fs.readFileSync(path.join(DATA, file), 'utf8'));
  for (const c of Object.values(doc.components || {})) {
    if (c.t !== 'MeshRenderer' || !Array.isArray(c.materials)) continue;
    for (const m of c.materials) {
      const e = m && m.guid && matTex[m.guid];
      if (!e || !e.tex) continue;                  // built-in or flat-colour material
      const gm = guidMap[e.tex];
      if (gm && /\.png$/i.test(gm.rel)) wanted.set(e.tex, gm.rel);
    }
  }
}

// Materials referenced by SCRIPT FIELDS, not by any renderer. DoorScript / SwingingDoorScript
// hold `open`, `openOutside`, `locked`... and swap them onto the faces at runtime, so the loop
// above never saw them and their textures were never baked: an opened door drew the renderer's
// flat grey "no texture" fallback. Measured before this pass: 21 materials in
// material-textures.json had an unbaked texture (Door_80, FacultyDoor_80, SwingDoor60[_Dark],
// SwingDoor0_Locked[_Dark], the four exit signs, BaldiDoorOpen, and SDF font materials whose
// atlases are not PNGs and are skipped by the .png test). Any {guid} anywhere in a component
// that names a known material counts.
{
  const visit = (v) => {
    if (!v || typeof v !== 'object') return;
    if (Array.isArray(v)) { for (const x of v) visit(x); return; }
    if (typeof v.guid === 'string' && matTex[v.guid] && matTex[v.guid].tex) {
      const gm = guidMap[matTex[v.guid].tex];
      if (gm && /\.png$/i.test(gm.rel)) wanted.set(matTex[v.guid].tex, gm.rel);
    }
    for (const k of Object.keys(v)) visit(v[k]);
  };
  for (const file of bakes) {
    const doc = JSON.parse(fs.readFileSync(path.join(DATA, file), 'utf8'));
    for (const c of Object.values(doc.components || {})) visit(c);
  }
}

// TEXTURES referenced DIRECTLY by script fields (Texture / Texture2D refs, not via a material).
// MathGameScript.incorrect (X.png) is assigned to a result RawImage only when an answer is wrong,
// so nothing on screen at load referenced it and it was never baked -- the X never appeared while
// the check mark (also on screen elsewhere) did. Any {guid} in any component that names a PNG.
{
  const visit = (v) => {
    if (!v || typeof v !== 'object') return;
    if (Array.isArray(v)) { for (const x of v) visit(x); return; }
    if (typeof v.guid === 'string' && guidMap[v.guid] && /\.png$/i.test(guidMap[v.guid].rel)) {
      wanted.set(v.guid, guidMap[v.guid].rel);
    }
    for (const k of Object.keys(v)) visit(v[k]);
  };
  for (const file of bakes) {
    const doc = JSON.parse(fs.readFileSync(path.join(DATA, file), 'utf8'));
    for (const c of Object.values(doc.components || {})) visit(c);
  }
}

// Character ANIMATION frame PNGs (data/anims-index.json, from bake-anims.js). The NPC clips'
// PPtr keys reference per-frame PNGs (Baldi_Wave0000.png and friends) that no scene component or
// script field touches -- an Animator plays them at runtime. Without this block the wave frames
// were the "111 unbaked NPC animation PNGs" and the intro greeting rendered nothing (Baldi said
// hi with no waving sprite, reported 2025-10-24).
const animIndexPath = path.join(DATA, 'anims-index.json');
if (fs.existsSync(animIndexPath)) {
  const anims = JSON.parse(fs.readFileSync(animIndexPath, 'utf8'));
  for (const clip of Object.values(anims)) {
    for (const track of (clip.tracks || [])) {
      for (const key of (track.keys || [])) {
        const gm = guidMap[key.guid];
        if (gm && /\.png$/i.test(gm.rel)) wanted.set(key.guid, gm.rel);
      }
    }
  }
}

// Skybox faces (data/skybox.json, from bake-material-textures.js). A six-sided skybox's
// textures are referenced by a SKYBOX material's TexEnv slots (_FrontTex/... ) -- NO MeshRenderer
// or script field ever touches them, so this is the only source that can reach them. School and
// Secret point RenderSettings.m_SkyboxMaterial at one such material; at draw time the shell
// reads the same file through eng's skybox accessor.
const skyPath = path.join(DATA, 'skybox.json');
if (fs.existsSync(skyPath)) {
  const skyIndex = JSON.parse(fs.readFileSync(skyPath, 'utf8'));
  for (const m of Object.values(skyIndex)) {
    for (const face of ['front', 'back', 'left', 'right', 'up', 'down']) {
      const g = m[face];
      if (!g || !guidMap[g] || !/\.png$/i.test(guidMap[g].rel)) continue;
      wanted.set(g, guidMap[g].rel);
    }
  }
}

// SpriteRenderer sprites -> the billboard pass (Baldi, the NPCs, pickups, the wall clutter).
// A SpriteRenderer references a SPRITE sub-asset (fileID 21300000) of a PNG, not a material, so
// this is a third source feeding the same guid set. The world-space SIZE of the quad is not in the
// scene: Unity derives it as pixels / spritePixelsToUnits, with spritePivot as the origin, both of
// which live in the PNG's .meta. They are read here and written to data/sprites-index.json --
// nothing downstream may assume 100 PPU or a centred pivot just because every sprite in this game
// happens to use them today.
const spriteMeta = {};
// Sprite references do NOT only live on SpriteRenderer.sprite. Scripts hold whole SETS of them in
// serialized fields and swap at runtime: FirstPrizeSpriteScript.sprites[] is 1st Prize's rotation
// frames, and the menus/number pad keep their pressed/selected states the same way. Those are
// still SCENE-referenced (fileID 21300000), small (3.5 MB for 112 of them), and invisible if
// unbaked -- the sim check world-sprites-all-baked found the first one. So every 21300000
// reference anywhere in a script's fields is collected, at any nesting depth.
function collectSpriteRefs(node, out) {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) {
    for (const v of node) collectSpriteRefs(v, out);
    return;
  }
  if (String(node.fileID) === '21300000' && node.guid) out.push(node.guid);
  for (const v of Object.values(node)) collectSpriteRefs(v, out);
}

for (const file of bakes) {
  const doc = JSON.parse(fs.readFileSync(path.join(DATA, file), 'utf8'));
  const refs = [];
  for (const c of Object.values(doc.components || {})) {
    if (c.t === 'SpriteRenderer' && c.sprite && c.sprite.guid) refs.push(c.sprite.guid);
    else if (c.t === 'Script') collectSpriteRefs(c.f, refs);
  }
  // the anim-clips' per-frame sprites (Baldi waving, talking, shaking...) swap on at runtime via
  // the Animator; each key IS a sprite guid that drawBillboards must resolve in sprites-index.
  const animIndexPath = path.join(DATA, 'anims-index.json');
  if (fs.existsSync(animIndexPath)) {
    const anims = JSON.parse(fs.readFileSync(animIndexPath, 'utf8'));
    for (const clip of Object.values(anims)) {
      for (const track of (clip.tracks || [])) {
        for (const key of (track.keys || [])) if (key.guid) refs.push(key.guid);
      }
    }
  }
  for (const guid of refs) {
    const gm = guidMap[guid];
    if (!gm || !/\.png$/i.test(gm.rel)) continue;
    wanted.set(guid, gm.rel);
    if (spriteMeta[guid]) continue;
    const metaPath = ASSETS + gm.rel + '.meta';
    let ppu = 100, px = 0.5, py = 0.5, mode = 1;
    if (fs.existsSync(metaPath)) {
      const t = fs.readFileSync(metaPath, 'utf8');
      const mp = /spritePixelsToUnits: ([\d.]+)/.exec(t);
      const mm = /spriteMode: (\d+)/.exec(t);
      const pv = /spritePivot: \{x: ([-\d.]+), y: ([-\d.]+)\}/.exec(t);
      if (mp) ppu = parseFloat(mp[1]);
      if (mm) mode = parseInt(mm[1], 10);
      if (pv) { px = parseFloat(pv[1]); py = parseFloat(pv[2]); }
    } else {
      console.log('  no .meta for ' + gm.rel + ' -- assuming 100 PPU, centred pivot');
    }
    // spriteMode 2 is a multi-sprite atlas, where 21300000 would NOT be the whole image and the
    // sub-rects would have to be read. Report rather than silently drawing the full sheet.
    if (mode !== 1) console.log('  SPRITE SHEET (spriteMode ' + mode + '): ' + gm.rel + ' -- only the full image is baked');
    spriteMeta[guid] = { rel: gm.rel, ppu: ppu, pivotX: px, pivotY: py, mode: mode };
  }
}

// The Player Settings DEFAULT CURSOR. Nothing in the scenes or the C# references it -- Unity sets
// it from ProjectSettings.asset (`defaultCursor` + `cursorHotspot`), which is why the game showed
// the player's own Windows arrow instead of Baldi's cursor. Pulled in here so the shell has pixels
// to build an HCURSOR from, and the hotspot is written to data/cursor.json beside it.
{
  const psPath = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/ProjectSettings/ProjectSettings.asset';
  if (fs.existsSync(psPath)) {
    const ps = fs.readFileSync(psPath, 'utf8');
    const dc = /defaultCursor: \{fileID: \d+(?:, guid: ([0-9a-f]+))?/.exec(ps);
    const hs = /cursorHotspot: \{x: ([-\d.]+), y: ([-\d.]+)\}/.exec(ps);
    if (dc && dc[1] && guidMap[dc[1]] && /\.png$/i.test(guidMap[dc[1]].rel)) {
      wanted.set(dc[1], guidMap[dc[1]].rel);
      fs.writeFileSync(path.join(DATA, 'cursor.json'), JSON.stringify({
        guid: dc[1],
        rel: guidMap[dc[1]].rel,
        hotspotX: hs ? parseFloat(hs[1]) : 0,
        hotspotY: hs ? parseFloat(hs[2]) : 0
      }, null, 1));
      console.log('  default cursor: ' + guidMap[dc[1]].rel +
                  ' hotspot (' + (hs ? hs[1] + ',' + hs[2] : '0,0') + ') -> data/cursor.json');
    }
  }
}

// Halve by a box filter until both axes fit. Only used with --max-dim; full resolution otherwise,
// because UI art is authored at the size it is drawn.
function halve(img) {
  const w = Math.max(1, img.width >> 1), h = Math.max(1, img.height >> 1);
  const out = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      for (let k = 0; k < 4; k++) {
        const a = img.rgba[((y * 2) * img.width + x * 2) * 4 + k];
        const b = img.rgba[((y * 2) * img.width + Math.min(img.width - 1, x * 2 + 1)) * 4 + k];
        const c = img.rgba[(Math.min(img.height - 1, y * 2 + 1) * img.width + x * 2) * 4 + k];
        const d = img.rgba[(Math.min(img.height - 1, y * 2 + 1) * img.width +
                            Math.min(img.width - 1, x * 2 + 1)) * 4 + k];
        out[(y * w + x) * 4 + k] = (a + b + c + d + 2) >> 2;
      }
    }
  }
  return { width: w, height: h, rgba: out };
}

// --report: measure only, write NOTHING. The raw bake is 167.9 MB and that is the open blocker on
// the armv7 iOS target, but "too big" is not a decision -- this prints what each --max-dim cap
// would actually cost and which textures dominate, so the trade is made on numbers. Reading the
// PNG headers is enough for the sizes; no pixels are decoded, so it runs in a second.
if (argv.indexOf('--report') >= 0) {
  const rows = [];
  for (const [guid, rel] of wanted) {
    const buf = fs.readFileSync(ASSETS + rel, { encoding: null });
    // IHDR is always the first chunk: 8-byte signature, 4 length, 4 type, then width/height.
    if (buf.length < 24 || buf.toString('latin1', 12, 16) !== 'IHDR') continue;
    const w = buf.readUInt32BE(16), h = buf.readUInt32BE(20);
    rows.push({ rel, w, h, bytes: w * h * 4, isSprite: !!spriteMeta[guid] });
  }
  rows.sort((a, b) => b.bytes - a.bytes);
  const total = rows.reduce((s, r) => s + r.bytes, 0);
  console.log(`${rows.length} textures, ${(total / 1048576).toFixed(1)} MB raw RGBA8 at full size\n`);
  console.log('  cap      total     saved   textures downscaled');
  for (const cap of [2048, 1024, 512, 256, 128]) {
    let t = 0, n = 0;
    for (const r of rows) {
      let w = r.w, h = r.h;
      while (w > cap || h > cap) { w = Math.max(1, w >> 1); h = Math.max(1, h >> 1); }
      if (w !== r.w) n++;
      t += w * h * 4;
    }
    console.log(`  ${String(cap).padStart(4)}  ${(t / 1048576).toFixed(1).padStart(8)} MB  ` +
                `${((total - t) / 1048576).toFixed(1).padStart(6)} MB  ${n}`);
  }
  console.log('\n  largest textures (these are the whole problem):');
  for (const r of rows.slice(0, 10)) {
    console.log(`  ${(r.bytes / 1048576).toFixed(1).padStart(6)} MB  ${String(r.w)}x${r.h}` +
                `${r.isSprite ? '  [sprite]' : ''}  ${r.rel}`);
  }
  console.log('\n  (report only -- nothing was written. Re-run without --report to bake.)');
  process.exit(0);
}

fs.mkdirSync(OUT, { recursive: true });
const index = {};
let bytes = 0, failed = 0;
for (const [guid, rel] of [...wanted].sort((a, b) => a[1].localeCompare(b[1]))) {
  let img;
  try {
    img = decodePng(fs.readFileSync(ASSETS + rel));
  } catch (e) {
    console.error(`  FAILED ${rel}: ${e.message}`);
    failed++;
    continue;
  }
  while (maxDim > 0 && (img.width > maxDim || img.height > maxDim)) img = halve(img);
  fs.writeFileSync(path.join(OUT, guid + '.raw'), img.rgba);
  // Unity import settings the renderer must honour. This game is point-filtered pixel art:
  // 197 of 205 textures are filterMode 0 (Point) and 204 of 205 have mipmaps OFF. Uploading them
  // with bilinear filtering smears a 142x256 Baldi across the screen and reads as "lower quality"
  // next to a real screenshot -- the source pixels are identical, the FILTER was not.
  let point = 1, mip = 0;
  const metaPath = ASSETS + rel + '.meta';
  if (fs.existsSync(metaPath)) {
    const t = fs.readFileSync(metaPath, 'utf8');
    const fm = /filterMode: (-?\d+)/.exec(t);
    const mm = /enableMipMap: (\d+)/.exec(t);
    if (fm) point = parseInt(fm[1], 10) === 0 ? 1 : 0;   // 0 = Point, 1 = Bilinear, 2 = Trilinear
    if (mm) mip = parseInt(mm[1], 10) ? 1 : 0;
  }
  index[guid] = { rel, w: img.width, h: img.height, point, mip };
  bytes += img.rgba.length;
}
fs.writeFileSync(path.join(DATA, 'textures-index.json'), JSON.stringify(index, null, 1));

// Sprite geometry index: the pixels live in data/tex/<guid>.raw like any other texture, so this
// carries only what makes a sprite a sprite -- the world size and the pivot.
const sprites = {};
for (const guid of Object.keys(spriteMeta)) {
  const e = index[guid];
  if (!e) continue;                          // decode failed; already reported above
  const m = spriteMeta[guid];
  sprites[guid] = {
    rel: m.rel, w: e.w, h: e.h, ppu: m.ppu, pivotX: m.pivotX, pivotY: m.pivotY,
    unitsW: e.w / m.ppu, unitsH: e.h / m.ppu
  };
}
fs.writeFileSync(path.join(DATA, 'sprites-index.json'), JSON.stringify(sprites, null, 1));
console.log('  sprites: ' + Object.keys(sprites).length + ' -> data/sprites-index.json');

const n = Object.keys(index).length;
console.log(`baked ${n} UI + world textures (${failed} failed) -> data/tex/*.raw`);
console.log(`  ${(bytes / 1048576).toFixed(1)} MB raw RGBA8${maxDim ? ` (--max-dim ${maxDim})` : ''}`);
console.log(`  index: data/textures-index.json`);
