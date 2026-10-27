'use strict';
// tags.js  -  dump TagManager.asset (tags, layers, sorting layers) for the collision matrix + camera setup.
const fs = require('fs');
const { splitDocs, parseBody } = require('./unity-yaml');
const F = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/ProjectSettings/TagManager.asset';

const str = (x) => (x && typeof x === 'object' && '__scalar' in x) ? x.__scalar : x;

const docs = splitDocs(fs.readFileSync(F, 'utf8'));
for (const d of docs) {
  const root = parseBody(d.body);
  const t = root && Object.prototype.hasOwnProperty.call(root, d.className) ? root[d.className] : root;
  console.log(`class: ${d.className}`);
  for (const k of Object.keys(t)) {
    const v = t[k];
    if (Array.isArray(v)) {
      const vals = v.map(str);
      const allScalar = vals.every((x) => typeof x === 'string' || typeof x === 'number' || x == null);
      console.log(`${k}: ${allScalar ? vals.join(' | ') || '(empty)' : JSON.stringify(v).slice(0, 800)}`);
    } else if (v && typeof v === 'object') {
      console.log(`${k}: ${JSON.stringify(v).slice(0, 800)}`);
    } else {
      console.log(`${k}: ${v}`);
    }
  }
}
