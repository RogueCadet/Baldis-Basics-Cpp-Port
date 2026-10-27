// font-bake.js  -  TMP font asset -> engine-loaded font data (no TTF parsing or PNG decoding at
// runtime; the iOS target stays dependency-free: it reads the emitted JSON + raw RGBA blob).
//
// Unity TMP in this build is the LEGACY font asset shape (fontAssetType: 0): a plain Unity YAML
// with m_fontInfo (face metrics) + m_glyphInfoList (per-glyph atlas rect + metrics) + optional
// m_kerningInfo. The glyph atlas is a separate .png ("COMIC_Pro Atlas.png"); only fonts with a
// shipped PNG get pixel data, the rest bake metrics-only (draw pass renders advance-only blank
// quads for those, exactly correct geometry, invisible ink).
//
// The tool AUTO-PICKS the atlas y convention by counting ink under two candidate mappings and
// keeping the one whose glyph rects contain ink. Outputs (written to data/):
//   data/font-<Name>.json     metrics + glyph table with FINAL atlas UVs (engine does no math)
//   data/font-<Name>.atlas    raw RGBA8 blob, rows top-to-bottom (PNG scanline order)
//   data/fonts-index.json     { "<guid>": "<Name>" } for the engine's guid -> font lookup
// Usage: node tools\font-bake.js     (BALDI_DECOMP env overrides the decompile root)
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const DECOMP = process.env.BALDI_DECOMP || 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5';
const OUT = path.join(__dirname, '..', 'data');

// ---- YAML-lite parsing of the font asset -------------------------------------
// The asset may be a MULTI-DOCUMENT Unity YAML (old TMP assets carry an inline Texture2D
// !u!28 + Material !u!21). Only the !u!114 document declaring the legacy keys (fontAssetType: 0
// + m_glyphInfoList) is a bakeable "static" font; fontAssetType 2 assets are DYNAMIC fonts
// (glyphs generated at runtime from the system TTF -- COMIC.asset) with no glyph table at all,
// so they are reported and skipped.
function parseFontAsset(file) {
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
  const f = {
    name: '', info: {}, glyphs: [], kernPairs: 0,
    fontAssetType: -1, docs: 0, isStatic: false,
  };
  let mode = null;
  let pending = null;
  let docIsFont = false;
  for (const line of lines) {
    const docMarker = /^--- !u!(\d+) /.exec(line);
    if (docMarker) {
      if (mode === 'glyph' && pending) { f.glyphs.push(pending); pending = null; }
      f.docs++;
      docIsFont = (docMarker[1] === '114');
      mode = docIsFont ? null : 'other';
      continue;
    }
    if (!docIsFont) continue; // Texture2D / Material documents: ignore entirely
    const typeLine = /^\s*fontAssetType:\s*(\d+)/.exec(line);
    if (typeLine) { f.fontAssetType = parseInt(typeLine[1], 10); continue; }
    const nameLine = /^\s*m_Name:\s*(.+)\s*$/.exec(line);
    if (nameLine && f.name === '') { f.name = nameLine[1].trim(); continue; }
    const section = /^\s*(m_fontInfo|m_glyphInfoList|m_kerningInfo)\s*:/.exec(line);
    if (section) {
      if (mode === 'glyph' && pending) { f.glyphs.push(pending); pending = null; }
      mode = section[1] === 'm_fontInfo' ? 'info'
           : section[1] === 'm_glyphInfoList' ? 'glyph'
           : 'kern';
      continue;
    }
    if (mode === 'info') {
      const kv = /^\s+([A-Za-z]\w*):\s*(.*)\s*$/.exec(line);
      if (kv) f.info[kv[1]] = kv[2];
      continue;
    }
    if (mode === 'glyph') {
      const id = /^\s*- id:\s*(-?\d+)/.exec(line);
      if (id) {
        if (pending) f.glyphs.push(pending);
        pending = { id: parseInt(id[1], 10) };
        continue;
      }
      const kv = /^\s+([a-zA-Z]\w*):\s*(-?[\d.eE+]+)\s*$/.exec(line);
      if (kv && pending) pending[kv[1]] = parseFloat(kv[2]);
      continue;
    }
    if (mode === 'kern') {
      if (/^\s*- \w+:/.test(line)) f.kernPairs++;
    }
  }
  if (pending) f.glyphs.push(pending);
  f.isStatic = (f.fontAssetType === 0 && f.glyphs.length > 0);
  return f;
}

// ---- tiny PNG decoder (8-bit, non-interlaced; color types 0/2/3/6) --------------
// Embedded TMP atlas (see the call site). Only TextureFormat 1 (Alpha8) occurs, and it is a SIGNED
// DISTANCE FIELD (the font's material is TMP's SDF shader): texel 0.5 = glyph edge. The engine's
// text pass treats atlas alpha as COVERAGE, so the field is turned into coverage here with a narrow
// smooth step around 0.5 (crisp at UI sizes; an approximation of TMP's screen-space SDF shading).
function readEmbeddedAtlas(assetFile) {
  const text = fs.readFileSync(assetFile, 'utf8');
  const ref = /\n  atlas: \{fileID: (-?\d+)\}/.exec(text);
  if (!ref) return null;
  const start = text.indexOf('--- !u!28 &' + ref[1]);
  if (start < 0) return null;
  const end = text.indexOf('\n--- !u!', start + 10);
  const doc = text.slice(start, end < 0 ? text.length : end);
  const w = parseInt((/m_Width: (\d+)/.exec(doc) || [])[1] || '0', 10);
  const h = parseInt((/m_Height: (\d+)/.exec(doc) || [])[1] || '0', 10);
  const fmt = parseInt((/m_TextureFormat: (\d+)/.exec(doc) || [])[1] || '-1', 10);
  const hex = (/_typelessdata: ([0-9a-f]+)/.exec(doc) || [])[1] || '';
  if (fmt !== 1 || !w || !h || hex.length < w * h * 2) return null;
  const rgba = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) {
    const srcRow = h - 1 - y;                      // bottom-first -> top-first
    for (let x = 0; x < w; x++) {
      // RAW distance in alpha: the renderer alpha-TESTS at 0.5 with linear filtering (the
      // classic shaderless SDF technique), crisp at every size. A pre-thresholded coverage map
      // fell apart into slivers when minified ~6x to UI sizes.
      const o = (y * w + x) * 4;
      rgba[o] = 255; rgba[o + 1] = 255; rgba[o + 2] = 255; rgba[o + 3] = parseInt(hex.substr((srcRow * w + x) * 2, 2), 16);
    }
  }
  return { w: w, h: h, rgba: rgba };
}

function decodePng(file) {
  const buf = fs.readFileSync(file);
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  if (buf.slice(0, 8).toString('hex') !== sig.toString('hex')) throw new Error('not a PNG');
  let p = 8;
  let w = 0, h = 0, bitDepth = 0, colorType = 0, interlace = 0;
  const idat = [];
  let palette = null;
  let trns = null;
  while (p < buf.length) {
    const len = buf.readUInt32BE(p);
    const type = buf.slice(p + 4, p + 8).toString('ascii');
    const data = buf.slice(p + 8, p + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0);
      h = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'PLTE') {
      palette = data;
    } else if (type === 'tRNS') {
      trns = data;
    } else if (type === 'IEND') {
      break;
    }
    p += 12 + len;
  }
  if (bitDepth !== 8 || interlace !== 0) throw new Error('unsupported PNG (bit depth/interlace)');
  const channels = (colorType === 6 ? 4 : colorType === 2 ? 3 : (colorType === 0 || colorType === 3) ? 1 : -1);
  if (channels < 0) throw new Error('unsupported PNG color type ' + colorType);
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * channels;
  if (raw.length < h * (stride + 1)) throw new Error('truncated PNG data');
  const px = Buffer.alloc(w * h * channels);
  for (let row = 0; row < h; row++) {
    const filter = raw[row * (stride + 1)];
    const srcRow = row * (stride + 1) + 1;
    for (let col = 0; col < stride; col++) {
      const ax = col >= channels ? px[row * stride + col - channels] : 0;
      const bx = row > 0 ? px[(row - 1) * stride + col] : 0;
      const cx = (row > 0 && col >= channels) ? px[(row - 1) * stride + col - channels] : 0;
      const rawVal = raw[srcRow + col];
      let pred = 0;
      if (filter === 1) pred = ax;
      else if (filter === 2) pred = bx;
      else if (filter === 3) pred = (ax + bx) >> 1;
      else if (filter === 4) {
        const pp = ax + bx - cx;
        const pa = Math.abs(pp - ax);
        const pb = Math.abs(pp - bx);
        const pc = Math.abs(pp - cx);
        pred = (pa <= pb && pa <= pc) ? ax : (pb <= pc ? bx : cx);
      }
      px[row * stride + col] = (rawVal + pred) & 255;
    }
  }
  let rgba = px;
  if (colorType === 3) {
    if (!palette || palette.length % 3 !== 0) throw new Error('palette PNG without usable PLTE');
    rgba = Buffer.alloc(w * h * 4);
    const n = palette.length / 3;
    for (let i = 0; i < w * h; i++) {
      const idx = px[i];
      if (idx >= n) throw new Error('palette index out of range');
      rgba[i * 4] = palette[idx * 3];
      rgba[i * 4 + 1] = palette[idx * 3 + 1];
      rgba[i * 4 + 2] = palette[idx * 3 + 2];
      rgba[i * 4 + 3] = (trns && idx < trns.length) ? trns[idx] : 255;
    }
  } else if (channels === 3) {
    rgba = Buffer.alloc(w * h * 4);
    for (let i = 0; i < w * h; i++) {
      rgba[i * 4] = px[i * 3];
      rgba[i * 4 + 1] = px[i * 3 + 1];
      rgba[i * 4 + 2] = px[i * 3 + 2];
      rgba[i * 4 + 3] = 255;
    }
  } else if (colorType === 0) {  // grayscale (palette indices were already expanded above)
    rgba = Buffer.alloc(w * h * 4);
    for (let i = 0; i < w * h; i++) {
      rgba[i * 4] = px[i];
      rgba[i * 4 + 1] = px[i];
      rgba[i * 4 + 2] = px[i];
      rgba[i * 4 + 3] = 255;
    }
  }
  return { w: w, h: h, rgba: rgba };
}

// ---- ink probing: which atlas y convention do the baked glyph rects follow? -----
// Candidate A: y is the BOTTOM edge (v-up); rect grows upward: [y .. y + h].
// Candidate B: rect is the mirrored window [atlasH - y - h .. atlasH - y].
function inkInRect(rgba, w, h, x0, yTop, x1, yBottom) {
  let ink = 0;
  for (let yy = Math.max(0, yTop); yy < Math.min(h, yBottom); yy++) {
    for (let xx = Math.max(0, x0); xx < Math.min(w, x1); xx++) {
      if (rgba[(yy * w + xx) * 4 + 3] >= 8) ink++;
    }
  }
  return ink;
}

// SDF atlases: "any ink in the rect" cannot vote -- a distance field is non-zero around every glyph,
// so both candidate cells nearly always "have ink" (LiberationSans: both=227, and the pick flipped
// between bakes; the A pick sampled every character from the wrong cell). TMP packs glyphs with
// PADDING, so in the right convention a glyph's 1-px border lies outside the glyph (distance < 0.5,
// alpha < 128) while a wrong cell cuts through a neighbour. Vote by clean borders instead.
function borderClean(rgba, w, h, x0, yTop, x1, yBottom) {
  // The glyph rect is the TIGHT bounds (the shape touches it); TMP's padding lies OUTSIDE it, so
  // test a ring 2 px out, which must be clear of ink in the right convention.
  x0 = Math.max(0, Math.round(x0) - 2); x1 = Math.min(w - 1, Math.round(x1) + 1);
  yTop = Math.max(0, Math.round(yTop) - 2); yBottom = Math.min(h - 1, Math.round(yBottom) + 1);
  if (x1 <= x0 || yBottom <= yTop) return false;
  for (let xx = x0; xx <= x1; xx++) {
    if (rgba[(yTop * w + xx) * 4 + 3] >= 128 || rgba[(yBottom * w + xx) * 4 + 3] >= 128) return false;
  }
  for (let yy = yTop; yy <= yBottom; yy++) {
    if (rgba[(yy * w + x0) * 4 + 3] >= 128 || rgba[(yy * w + x1) * 4 + 3] >= 128) return false;
  }
  return true;
}
function validateConventionSdf(png, ga) {
  const { w, h, rgba } = png;
  let inkA = 0, inkB = 0, both = 0, none = 0;
  for (const g of ga) {
    if (!g.width || !g.height) { none++; continue; }
    const a = borderClean(rgba, w, h, g.x, h - g.y - g.height, g.x + g.width, h - g.y);
    const b = borderClean(rgba, w, h, g.x, g.y, g.x + g.width, g.y + g.height);
    if (a && b) both++; else if (a) inkA++; else if (b) inkB++; else none++;
  }
  return { inkA, inkB, both, none };
}

function validateConvention(f, png, ga) {
  const { w, h, rgba } = png;
  let inkA = 0, inkB = 0, both = 0, none = 0;
  for (const g of ga) {
    if (!g.width || !g.height) { none++; continue; }
    const rectA = inkInRect(rgba, w, h, g.x, h - g.y - g.height, g.x + g.width, h - g.y);
    const rectB = inkInRect(rgba, w, h, g.x, g.y, g.x + g.width, g.y + g.height);
    if (rectA > 0 && rectB > 0) both++;
    else if (rectA > 0) inkA++;
    else if (rectB > 0) inkB++;
    else none++;
  }
  return { inkA, inkB, both, none };
}

// ---- discover font assets -------------------------------------------------------
function walk(dir, out) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}

const files = walk(path.join(DECOMP, 'Assets'), []).filter((p) => p.toLowerCase().endsWith('.asset'));
const fontFiles = [];
for (const f of files) {
  let body;
  try { body = fs.readFileSync(f, 'utf8'); } catch (e) { continue; }
  if (body.indexOf('m_fontInfo:') === -1 || body.indexOf('m_glyphInfoList:') === -1) continue;
  fontFiles.push(f);
}
if (!fontFiles.length) throw new Error('no legacy TMP font assets found under the decompile');
console.log('Legacy TMP font assets (' + fontFiles.length + '):');

// ---- emit ----------------------------------------------------------------------
// Glyph records go to the engine in FINAL, engine-ready form:
//   u0/u1: atlas x fraction (0 = left column of the emitted blob)
//   v0/v1: atlas row fraction (0 = row 0 of the emitted blob = image TOP; v0 < v1 always)
//   w/h:   atlas pixel size; xo/yo/adv/sc: TMP metrics verbatim (baseline-relative pen math)
// The y convention of the asset's glyph rects is resolved HERE by the ink vote below, so the
// C++ runtime never touches pixel math.
const H = (v) => (v > 0 ? 1 : 0); // exclusive-count winner helper: nonzero beats zero

const index = {};
let baked = 0, skipped = 0;
for (const file of fontFiles) {
  const f = parseFontAsset(file);
  if (f.glyphs.length === 0) {
    console.log('  SKIP ' + f.name + ' (fontAssetType ' + f.fontAssetType +
      ', no glyph table, ' + f.docs + ' documents) -- nothing layout-usable to consume');
    skipped++;
    continue;
  }
  const metaTxt = fs.readFileSync(file + '.meta', 'utf8');
  const gm = /guid: ([0-9a-f]{32})/.exec(metaTxt);
  const guid = gm ? gm[1] : '';
  if (f.name === '') f.name = path.basename(file, '.asset');

  // face metrics come from m_fontInfo
  const mi = f.info;
  const meta = {
    name: f.name,
    guid: guid,
    source: file.replace(DECOMP, '').replace(/\\/g, '/'),
    fontAssetType: f.fontAssetType,
    pointSize: parseFloat(mi.PointSize || '24'),
    lineHeight: parseFloat(mi.LineHeight || '33'),
    baseline: parseFloat(mi.Baseline || '0'),
    ascender: parseFloat(mi.Ascender || '0'),
    capHeight: parseFloat(mi.CapHeight || '0'),
    descender: parseFloat(mi.Descender || '0'),
    scale: parseFloat(mi.Scale || '1'),
    padding: parseFloat(mi.Padding || '0'),
    atlasW: parseInt(mi.AtlasWidth || '0', 10),
    atlasH: parseInt(mi.AtlasHeight || '0', 10),
    tabWidth: parseFloat(mi.TabWidth || '0'),
    kernPairs: f.kernPairs,
    atlas: null,
    vNote: 'v0/v1 are row fractions of the .atlas blob (0 = first row = image top)',
    glyphs: [],
  };

  // the shipped atlas PNG, when present, sits next to the .asset as "<m_Name> Atlas.png"
  const pngPath = path.join(path.dirname(file), f.name + ' Atlas.png');
  let png = null;
  let convNote = 'atlas-less (metrics only; decompile ships no atlas pixels)';
  if (!fs.existsSync(pngPath)) {
    // EMBEDDED ATLAS. TMP's default font (LiberationSans SDF -- 55 of MainMenu's labels, the whole
    // controls page) keeps its atlas as a Texture2D SUB-OBJECT of the font .asset ("atlas: {fileID}")
    // with the pixels inline as hex `_typelessdata`, not as "<name> Atlas.png" -- so it was baked
    // metrics-only and every one of those labels was dropped as inkless. Decoded here into the
    // PNG path's shape (rows top-first: Unity stores texture rows bottom-first, so they are
    // flipped), after which the existing ink vote picks the glyph y convention as usual.
    const emb = readEmbeddedAtlas(file);
    if (emb) { png = emb; convNote = 'embedded Alpha8 SDF atlas; '; }
  }
  if (png || fs.existsSync(pngPath)) {
    if (!png) png = decodePng(pngPath);
    if (png.w !== meta.atlasW || png.h !== meta.atlasH) {
      throw new Error('atlas png size ' + png.w + 'x' + png.h + ' mismatches ' +
        meta.atlasW + 'x' + meta.atlasH + ' for ' + f.name);
    }
    const v = /SDF$/.test(f.name) ? validateConventionSdf(png, f.glyphs) : validateConvention(f, png, f.glyphs);
    // Vote by EXCLUSIVE ink: rects that have ink ONLY under one convention. both/inkless
    // can dominate neither. With 0/0 the asset wins nothing -> default A is recorded.
    const pickA = v.inkA > 0 ? v.inkA >= v.inkB : v.inkB === 0;
    convNote = 'ink bottom-edge=' + v.inkA + ' mirrored=' + v.inkB +
      ' both=' + v.both + ' inkless=' + v.none + ' -> picked ' + (pickA ? 'A(bottom)' : 'B(top)');
    meta.atlas = 'data/font-' + f.name.replace(/\s+/g, '_') + '.atlas';
    // TMP SDF faces (" SDF" in the asset name; their material is TMP's SDF shader) store a distance
    // field in alpha, not coverage -- the renderer must alpha-test them, not blend them.
    meta.sdf = /SDF$/.test(f.name);
    const W = meta.atlasW, AH = meta.atlasH;
    meta.glyphs = f.glyphs.map((g) => {
      const yTop = pickA ? (AH - g.y - g.height) : g.y;  // row of the glyph's TOP edge
      return {
        id: g.id || 0,
        u0: (g.x || 0) / W,
        u1: ((g.x || 0) + (g.width || 0)) / W,
        v0: yTop / AH,
        v1: (yTop + (g.height || 0)) / AH,
        w: g.width || 0,
        h: g.height || 0,
        xo: g.xOffset || 0,
        yo: g.yOffset || 0,
        adv: g.xAdvance || 0,
        sc: g.scale || 1,
      };
    });
  } else {
    // no pixels: keep the raw authored rect (engine only consumes advances/metrics here)
    meta.glyphs = f.glyphs.map((g) => ({
      id: g.id || 0,
      w: g.width || 0,
      h: g.height || 0,
      xo: g.xOffset || 0,
      yo: g.yOffset || 0,
      adv: g.xAdvance || 0,
      sc: g.scale || 1,
    }));
  }

  const base = 'font-' + f.name.replace(/\s+/g, '_');
  const jsonPath = path.join(OUT, base + '.json');
  fs.writeFileSync(jsonPath, JSON.stringify(meta, null, 1), 'utf8');
  if (png) fs.writeFileSync(path.join(OUT, base + '.atlas'), png.rgba, 'binary');
  if (guid) index[guid] = f.name;
  baked++;

  console.log('  ' + f.name + ': ' + meta.glyphs.length + ' glyphs, atlas ' +
    meta.atlasW + 'x' + meta.atlasH + (png ? ' [' + convNote + ']  png decoded' : ' [metrics only]'));
  console.log('      wrote ' + path.basename(jsonPath) + (png ? ' + ' + base + '.atlas (' +
    png.w * png.h * 4 + ' bytes)' : ''));

  // Visual proof that the convention + decoder are right: ASCII strips of probe glyphs.
  if (png) {
    for (const probeId of [65, 33, 103]) {  // 'A', '!', 'g' -- legibility is obvious or it is not
      const g = f.glyphs.find((q) => q.id === probeId);
      const rec = meta.glyphs.find((q) => q.id === probeId);
      if (!g || !rec || !g.width) continue;
      const rows = Math.min(g.height, 20);
      console.log('      id' + probeId + ' strip (' + g.width + 'x' + g.height + '):');
      for (let r = 0; r < rows; r++) {
        let line = '        ';
        for (let c = 0; c < g.width; c++) {
          const px = ((Math.round(rec.v0 * png.h) + r) * png.w + Math.round(rec.u0 * png.w) + c) * 4;
          line += png.rgba[px + 3] >= 8 ? '#' : '.';
        }
        console.log(line);
      }
    }
  }
}

fs.writeFileSync(path.join(OUT, 'fonts-index.json'), JSON.stringify(index, null, 1), 'utf8');
console.log('wrote fonts-index.json (' + Object.keys(index).length + ' guid -> font mappings); ' +
  baked + ' baked, ' + skipped + ' skipped');
