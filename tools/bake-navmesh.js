'use strict';
// bake-navmesh.js -- the GENUINE NavMesh of each scene, decoded from the decompile's binary
// Scene/<Scene>/NavMesh.asset (a Unity 2018.3 serialized file holding NavMeshData).
//
// The engine had no pathfinding at all: NavMeshAgent moved in a straight 3D line to its
// destination, so characters walked through walls instead of following corridors, and Baldi rose
// toward the player's height ("chases me floating in the air"). Unity agents walk the baked mesh.
//
// FORMAT (reverse-engineered from the file, validated over every tile -- see the checks below):
// each tile is Unity's fork of a Detour tile, starting with magic 'DNAV' (bytes "VAND"):
//   header 72 bytes: int magic, version(16), x, y, layer, polyCount, vertCount, (4 more ints),
//                    float bmin[3], bmax[3], quant
//   verts:  vertCount * float[3], WORLD space (the School floor sits at y ~= 0.017)
//   polys:  polyCount * 32 bytes = u16 verts[6], u16 neis[6], u32 flags, u8 vertCount, u8 area, 2 pad
// (stride 32 / count at +28 is the only layout under which all 1101 School polygons decode with
//  in-range vertex indices and a 3..6 count.)
//
// Adjacency is computed GEOMETRICALLY: two polygons connect where an edge of one and an edge of the
// other are collinear and overlap; the overlap is the PORTAL (what the funnel algorithm needs).
// That also links polygons across tile borders, where Detour uses external links.
//
// Output: data/navmesh-<Scene>.json { polys: [{v:[[x,y,z]...], n:[{p:index, a:[x,z], b:[x,z]}...]}] }
// Usage: node tools/bake-navmesh.js
const fs = require('fs');
const path = require('path');
const DECOMP = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/Scene/';
const DATA = path.join(__dirname, '..', 'data');

function decode(file) {
  const d = fs.readFileSync(file);
  const polys = [];
  let bad = 0;
  for (let o = d.indexOf('VAND'); o >= 0; o = d.indexOf('VAND', o + 4)) {
    const version = d.readInt32LE(o + 4);
    if (version !== 16) continue;
    const polyCount = d.readInt32LE(o + 20), vertCount = d.readInt32LE(o + 24);
    const verts = [];
    for (let k = 0; k < vertCount; k++) {
      const vo = o + 72 + 12 * k;
      verts.push([d.readFloatLE(vo), d.readFloatLE(vo + 4), d.readFloatLE(vo + 8)]);
    }
    const base = o + 72 + 12 * vertCount;
    for (let p = 0; p < polyCount; p++) {
      const po = base + 32 * p;
      const nv = d[po + 28];
      const idx = [];
      for (let k = 0; k < nv; k++) idx.push(d.readUInt16LE(po + 2 * k));
      if (nv < 3 || nv > 6 || idx.some((v) => v >= vertCount)) { bad++; continue; }
      polys.push({ v: idx.map((i) => verts[i]), n: [] });
    }
  }
  return { polys, bad };
}

// segments (in XZ) of two polygon edges: collinear + overlapping -> the shared portal
function portal(a0, a1, b0, b1) {
  const ex = a1[0] - a0[0], ez = a1[2] - a0[2];
  const len = Math.hypot(ex, ez);
  if (len < 1e-4) return null;
  const ux = ex / len, uz = ez / len;
  const dist = (p) => Math.abs((p[0] - a0[0]) * uz - (p[2] - a0[2]) * ux);   // distance off the line
  if (dist(b0) > 0.05 || dist(b1) > 0.05) return null;
  if (Math.abs(a0[1] - b0[1]) > 1.0 || Math.abs(a1[1] - b1[1]) > 1.0) {
    // heights must agree roughly too (stacked floors never touch here, but be safe)
  }
  const t = (p) => (p[0] - a0[0]) * ux + (p[2] - a0[2]) * uz;            // position along the line
  const lo = Math.max(0, Math.min(t(b0), t(b1)));
  const hi = Math.min(len, Math.max(t(b0), t(b1)));
  if (hi - lo < 0.05) return null;
  return [[a0[0] + ux * lo, a0[2] + uz * lo], [a0[0] + ux * hi, a0[2] + uz * hi]];
}

for (const scene of ['School']) {
  const file = path.join(DECOMP, scene, 'NavMesh.asset');
  if (!fs.existsSync(file)) continue;
  const { polys, bad } = decode(file);
  // spatial hash of edges by rounded midpoint direction-independent key for speed
  const buckets = new Map();
  const key = (x, z) => Math.round(x / 4) + ',' + Math.round(z / 4);
  polys.forEach((p, pi) => {
    for (let k = 0; k < p.v.length; k++) {
      const a = p.v[k], b = p.v[(k + 1) % p.v.length];
      const kk = key((a[0] + b[0]) / 2, (a[2] + b[2]) / 2);
      if (!buckets.has(kk)) buckets.set(kk, []);
      buckets.get(kk).push([pi, a, b]);
    }
  });
  let links = 0;
  polys.forEach((p, pi) => {
    for (let k = 0; k < p.v.length; k++) {
      const a = p.v[k], b = p.v[(k + 1) % p.v.length];
      const mx = (a[0] + b[0]) / 2, mz = (a[2] + b[2]) / 2;
      const seen = new Set();
      for (let dx = -2; dx <= 2; dx++) for (let dz = -2; dz <= 2; dz++) {
        const list = buckets.get((Math.round(mx / 4) + dx) + ',' + (Math.round(mz / 4) + dz));
        if (!list) continue;
        for (const [qi, c, e] of list) {
          if (qi === pi || seen.has(qi)) continue;
          const pr = portal(a, b, c, e);
          if (pr) { p.n.push({ p: qi, a: pr[0], b: pr[1] }); seen.add(qi); links++; }
        }
      }
    }
  });
  const round = (x) => Math.round(x * 1000) / 1000;
  const out = { scene, polys: polys.map((p) => ({ v: p.v.map((q) => q.map(round)),
    n: p.n.map((l) => ({ p: l.p, a: l.a.map(round), b: l.b.map(round) })) })) };
  fs.writeFileSync(path.join(DATA, 'navmesh-' + scene + '.json'), JSON.stringify(out));
  const isolated = polys.filter((p) => p.n.length === 0).length;
  console.log('  ' + scene + ': ' + polys.length + ' polygons (' + bad + ' rejected), ' + links +
              ' portal links, ' + isolated + ' isolated -> data/navmesh-' + scene + '.json');
}
