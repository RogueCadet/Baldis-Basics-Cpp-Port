'use strict';
// bake-meshes.js  -  bake the real .asset meshes (CombinedMeshes_* desks / lockers / BSODA sides)
// into flat binary blobs the engine can load and draw directly. Same contract as the textures and
// the font atlas: the C++ side NEVER parses an asset format, because the iOS build takes no
// third-party dependencies.
//
// These are the only meshes in the game that are not a Unity built-in primitive. Every MeshFilter
// pointing at fileID 4300000 is one of these, identified by the reference's GUID -- the fileID is
// 4300000 for ALL of them, which is exactly why the renderer needs the guid and not just the id.
//
// Output:
//   data/mesh/<guid>.mesh    binary, see FORMAT below
//   data/meshes-index.json   { guid: { rel, name, verts, indices, stride, center, extent, uses } }
//
// FORMAT (little-endian, matches MeshBlob in shell-win32.cpp):
//   char   magic[4]   'B','B','M','1'
//   u32    vertCount
//   u32    indexCount        (always a multiple of 3; triangle list)
//   f32    center[3]         localAABB straight from the asset (sanity / culling)
//   f32    extent[3]
//   vert   vertCount * { f32 pos[3]; f32 uv[2] }   = 20 bytes each
//   u16    indexCount        (one u16 of padding appended if that would leave the file odd-aligned)
//
// Normals/tangents/uv1 are dropped: the game is flat-lit and the world pass draws unlit textured
// triangles. Add them here (and bump the magic) if lighting ever lands.
//
// Usage: node tools\bake-meshes.js
const fs = require('fs');
const path = require('path');

const ASSETS = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/';
const DATA = path.join(__dirname, '..', 'data');
const OUT = path.join(DATA, 'mesh');

const guidMap = JSON.parse(fs.readFileSync(path.join(DATA, 'manifest.json'), 'utf8')).guidMap;

// ---- which guids do the scenes/prefabs actually reference? ------------------
// Walk the bakes rather than globbing Assets/Mesh: an unreferenced asset should not be baked, and a
// referenced guid that resolves to nothing must be REPORTED (there is one such in School) instead
// of silently leaving a hole in the room.
const wanted = new Map();                       // guid -> number of MeshFilters using it
const bakes = fs.readdirSync(DATA).filter(function (f) { return /^(scene|prefab)-.*\.bake\.json$/.test(f); });
for (const file of bakes) {
  const doc = JSON.parse(fs.readFileSync(path.join(DATA, file), 'utf8'));
  for (const c of Object.values(doc.components || {})) {
    if (c.t !== 'MeshFilter' || !c.mesh || !c.mesh.guid) continue;
    if (String(c.mesh.fileID) !== '4300000') continue;   // built-in primitives need no blob
    wanted.set(c.mesh.guid, (wanted.get(c.mesh.guid) || 0) + 1);
  }
}

// ---- minimal field readers -------------------------------------------------
// The mesh asset is flat YAML with two very large scalars (a bare hex run for m_IndexBuffer, a
// double-quoted multi-line hex run for _typelessdata). The general reader in unity-yaml.js is
// document/mapping oriented and would hand these back as mangled strings, so they are pulled out by
// name here -- far less machinery than teaching it block scalars for two keys.
function scalar(text, key) {
  const m = new RegExp('^\\s*' + key + ': (.*)$', 'm').exec(text);
  return m ? m[1].trim() : null;
}
function hexRun(text, key) {
  // Bare form:   m_IndexBuffer: 0000020003000000...
  // Quoted form: _typelessdata: "ac5572bf...(newline+indent)...9a99993e"
  // The quoted form is a YAML double-quoted scalar wrapped every 1024 hex characters with a
  // trailing backslash line-fold ("...3333d33f\" + newline + indent). The backslash and the
  // indentation are folding syntax, not data, so both are skipped -- stopping at the backslash
  // truncated every desk to the first 512 bytes.
  const i = text.indexOf(key + ': ');
  if (i < 0) return null;
  let j = i + key.length + 2;
  if (text[j] === '"') j++;
  let out = '';
  for (; j < text.length; j++) {
    const ch = text[j];
    if (ch === '"') break;
    if (ch === '\n' || ch === '\r' || ch === ' ' || ch === '\t' || ch === '\\') continue;
    if (/[0-9a-fA-F]/.test(ch)) out += ch;
    else break;                                  // a bare run ended at a real token
  }
  return out;
}
function channels(text) {
  // m_Channels is a list of {stream, offset, format, dimension} in Unity's FIXED channel order:
  // 0 position, 1 normal, 2 tangent, 3 color, 4 uv0, 5 uv1, ... Unused channels have dimension 0,
  // so the slot index is what identifies a channel -- never its position among the used ones.
  const block = text.slice(text.indexOf('m_Channels:'));
  const out = [];
  const re = /- stream: (\d+)\s+offset: (\d+)\s+format: (\d+)\s+dimension: (\d+)/g;
  let m;
  while ((m = re.exec(block)) && out.length < 14) {
    out.push({ stream: +m[1], offset: +m[2], format: +m[3], dim: +m[4] });
  }
  return out;
}

function bakeOne(guid, rel) {
  const text = fs.readFileSync(path.join(ASSETS, rel), 'utf8');
  const name = scalar(text, 'm_Name');
  const vertCount = parseInt(scalar(text, 'm_VertexCount'), 10);
  const dataSize = parseInt(scalar(text, 'm_DataSize'), 10);
  const indexFormat = parseInt(scalar(text, 'm_IndexFormat'), 10);
  const topology = parseInt(scalar(text, 'topology'), 10);
  const indexCount = parseInt(scalar(text, 'indexCount'), 10);
  if (indexFormat !== 0) throw new Error(name + ': m_IndexFormat ' + indexFormat + ' (only 0 / uint16 handled)');
  if (topology !== 0) throw new Error(name + ': topology ' + topology + ' (only 0 / triangles handled)');
  if (indexCount % 3 !== 0) throw new Error(name + ': indexCount ' + indexCount + ' is not a triangle list');

  const ch = channels(text);
  const pos = ch[0], uv0 = ch[4];
  if (!pos || pos.dim !== 3 || pos.format !== 0) throw new Error(name + ': position channel is not float32x3');
  if (!uv0 || uv0.dim !== 2 || uv0.format !== 0) throw new Error(name + ': uv0 channel is not float32x2');
  // Stride is NOT constant across these assets (56 for the desks, 48 for the lockers, which carry
  // no tangent channel), so it is derived rather than assumed.
  if (dataSize % vertCount !== 0) throw new Error(name + ': m_DataSize ' + dataSize + ' not divisible by ' + vertCount);
  const stride = dataSize / vertCount;

  const vhex = hexRun(text, '_typelessdata');
  const ihex = hexRun(text, 'm_IndexBuffer');
  if (!vhex || vhex.length / 2 !== dataSize) {
    throw new Error(name + ': vertex blob is ' + (vhex ? vhex.length / 2 : 0) + ' bytes, expected ' + dataSize);
  }
  if (!ihex || ihex.length / 2 !== indexCount * 2) {
    throw new Error(name + ': index blob is ' + (ihex ? ihex.length / 2 : 0) + ' bytes, expected ' + indexCount * 2);
  }
  const vbuf = Buffer.from(vhex, 'hex');
  const ibuf = Buffer.from(ihex, 'hex');

  const aabb = /localAABB:\s*m_Center: \{x: ([^,]+), y: ([^,]+), z: ([^}]+)\}\s*m_Extent: \{x: ([^,]+), y: ([^,]+), z: ([^}]+)\}/.exec(
    text.replace(/\r/g, '')
  );
  const box = aabb ? aabb.slice(1).map(Number) : [0, 0, 0, 0, 0, 0];

  // Validate every index before writing: an out-of-range index is a silent GPU fault later.
  let maxIdx = 0;
  for (let i = 0; i < indexCount; i++) maxIdx = Math.max(maxIdx, ibuf.readUInt16LE(i * 2));
  if (maxIdx >= vertCount) throw new Error(name + ': index ' + maxIdx + ' out of range for ' + vertCount + ' verts');

  const head = 4 + 4 + 4 + 24;
  const vbytes = vertCount * 20;
  const ibytes = indexCount * 2;
  const pad = (ibytes % 4) ? 2 : 0;
  const out = Buffer.alloc(head + vbytes + ibytes + pad);
  out.write('BBM1', 0, 'latin1');
  out.writeUInt32LE(vertCount, 4);
  out.writeUInt32LE(indexCount, 8);
  for (let i = 0; i < 6; i++) out.writeFloatLE(box[i], 12 + i * 4);
  for (let v = 0; v < vertCount; v++) {
    const base = v * stride, o = head + v * 20;
    out.writeFloatLE(vbuf.readFloatLE(base + pos.offset + 0), o + 0);
    out.writeFloatLE(vbuf.readFloatLE(base + pos.offset + 4), o + 4);
    out.writeFloatLE(vbuf.readFloatLE(base + pos.offset + 8), o + 8);
    out.writeFloatLE(vbuf.readFloatLE(base + uv0.offset + 0), o + 12);
    out.writeFloatLE(vbuf.readFloatLE(base + uv0.offset + 4), o + 16);
  }
  ibuf.copy(out, head + vbytes, 0, ibytes);
  fs.writeFileSync(path.join(OUT, guid + '.mesh'), out);
  return {
    rel: rel, name: name, verts: vertCount, indices: indexCount, stride: stride,
    center: box.slice(0, 3), extent: box.slice(3)
  };
}

// ---- run -------------------------------------------------------------------
if (!fs.existsSync(OUT)) fs.mkdirSync(OUT, { recursive: true });
const index = {};
const missing = [];
let bytes = 0;
const order = Array.from(wanted).sort(function (a, b) { return b[1] - a[1]; });
for (const pair of order) {
  const guid = pair[0], uses = pair[1];
  const gm = guidMap[guid];
  if (!gm || !/\.asset$/i.test(gm.rel) || !fs.existsSync(path.join(ASSETS, gm.rel))) {
    missing.push({ guid: guid, uses: uses, rel: gm ? gm.rel : null });
    continue;
  }
  const rec = bakeOne(guid, gm.rel);
  rec.uses = uses;
  index[guid] = rec;
  bytes += fs.statSync(path.join(OUT, guid + '.mesh')).size;
  console.log('  ' + rec.name.padEnd(34) + ' ' + String(rec.verts).padStart(5) + ' verts ' +
              String(rec.indices / 3).padStart(5) + ' tris  stride ' + rec.stride + '  x' + uses);
}
fs.writeFileSync(path.join(DATA, 'meshes-index.json'), JSON.stringify(index, null, 1));
console.log('baked ' + Object.keys(index).length + ' meshes, ' + (bytes / 1024).toFixed(1) + ' KB -> data\\mesh\\');
for (const m of missing) {
  console.log('  UNRESOLVED guid ' + m.guid + ' (x' + m.uses + ') rel=' + m.rel +
              ' -- referenced by a MeshFilter but absent from the decompile; the renderer skips it');
}
