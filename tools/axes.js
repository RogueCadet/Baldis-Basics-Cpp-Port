'use strict';
// axes.js  -  dump InputManager.asset axes (dead keyboard map of the original; feeds testbed + touch design).
const fs = require('fs');
const path = require('path');
const { splitDocs, parseBody } = require('./unity-yaml');

const OUT = 'F:/baldidecomp/baldi_cpp_port/data';
const F = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/ProjectSettings/InputManager.asset';

const docs = splitDocs(fs.readFileSync(F, 'utf8'));
const axes = [];
for (const d of docs) {
  const root = parseBody(d.body);
  const t = root && Object.prototype.hasOwnProperty.call(root, d.className) ? root[d.className] : root;
  if (!t || !Array.isArray(t.m_Axes)) continue;
  for (const a of t.m_Axes) {
    axes.push({
      name: a.name || a.m_Name || '',
      positive: a.positiveButton || '',
      negative: a.negativeButton || '',
      altPositive: a.altPositiveButton || '',
      altNegative: a.altNegativeButton || '',
      gravity: a.gravity, sensitivity: a.sensitivity, dead: a.dead,
      type: a.type, axis: a.axis, joyNum: a.joyNum,
    });
  }
}
fs.writeFileSync(path.join(OUT, 'input-axes.json'), JSON.stringify(axes, null, 1));
console.log(`axes: ${axes.length}`);
console.log(axes.map((a) => `${a.name} [type ${a.type}] keys: ${a.positive}/${a.negative} alt: ${a.altPositive}/${a.altNegative} axis:${a.axis}`).join('\n'));
