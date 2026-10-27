'use strict';
// mats.js  -  census of materials and their shaders (resolves the LWRP/postprocessing open item).
const fs = require('fs');
const path = require('path');
const { splitDocs, parseBody } = require('./unity-yaml');

const OUT = 'F:/baldidecomp/baldi_cpp_port/data';
const ASSETS = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets';

const manifest = JSON.parse(fs.readFileSync(path.join(OUT, 'manifest.json'), 'utf8'));

const files = [];
(function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else files.push(p);
  }
})(ASSETS);
const relOf = (f) => f.startsWith(ASSETS + '/') ? f.slice(ASSETS.length + 1).replace(/\\/g, '/') : path.basename(f);

// shader guid -> declared name (from the .shader source text)
const shaderName = {};
for (const f of files) {
  if (!f.toLowerCase().endsWith('.shader')) continue;
  const t = fs.readFileSync(f, 'utf8');
  const m = /Shader\s+"([^"]+)"/.exec(t.slice(0, 2000));
  if (m) shaderName[relOf(f)] = m[1];
}

const mats = [];
const hist = {};
for (const f of files) {
  if (!f.toLowerCase().endsWith('.mat')) continue;
  const docs = splitDocs(fs.readFileSync(f, 'utf8'));
  for (const d of docs) {
    if (d.className !== 'Material') continue;
    const root = parseBody(d.body);
    const m = root && root.Material ? root.Material : root;
    const name = (m && m.m_Name) || path.basename(f, '.mat');
    let sh = '<none>';
    if (m && m.m_Shader && m.m_Shader.guid) {
      const gm = manifest.guidMap[m.m_Shader.guid];
      const rel = gm ? gm.rel : `<unresolved:${m.m_Shader.guid}>`;
      sh = shaderName[rel] || rel;
    }
    mats.push({ material: name, file: relOf(f), shader: sh });
    const key = sh;
    hist[key] = (hist[key] || 0) + 1;
  }
}
fs.writeFileSync(path.join(OUT, 'materials.json'), JSON.stringify(mats, null, 1));
console.log(`materials: ${mats.length}`);
console.log(Object.entries(hist).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}: ${v}`).join('\n'));
