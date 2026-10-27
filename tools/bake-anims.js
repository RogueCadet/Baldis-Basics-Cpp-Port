'use strict';
// bake-anims.js  -  bake Unity AnimationClips into flat sprite-keyframe tracks.
//
// Scoped in session 7z: the NPCs are NOT animated by script sprite arrays (which is why the
// script-field sweep in bake-textures.js never reached their frames). `BaldiSprite` carries an
// ANIMATOR, and the clips it plays are the easy kind -- their only animated curve is
// `attribute: m_Sprite`, i.e. PPtr keyframes that swap a sprite reference over time. There is no
// transform animation, no blending and no state-machine logic to honour for the in-world NPCs.
//
// So this reads each .anim's m_PPtrCurves into (time, sprite guid) tracks. It does NOT attempt to
// model AnimatorControllers: the runtime side plays a clip, and which clip is a separate question.
//
// Output:
//   data/anims-index.json  { <clip name>: { rel, sampleRate, loop, length, tracks: [...] } }
//     tracks: [{ attribute, path, keys: [{ t, guid }] }]
//   It also prints the distinct sprite guids referenced, which is what the texture bake must cover
//   before any of this can actually draw.
//
// The .anim files are small and regular, so this parses the two fields it needs by hand rather
// than going through unity-yaml.js -- same judgement as bake-meshes.js makes for its hex blobs.
//
// Usage: node tools\bake-anims.js
const fs = require('fs');
const path = require('path');

const ASSETS = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/';
const DATA = path.join(__dirname, '..', 'data');
const CLIPS = path.join(ASSETS, 'AnimationClip');

function scalar(text, key, def) {
  const m = new RegExp('^\\s*' + key + ': (.*)$', 'm').exec(text);
  return m ? m[1].trim() : def;
}

// m_PPtrCurves is a list of curves, each with its own `curve:` list of {time, value{guid}} keys,
// then the attribute/path/classID that say WHAT it drives. Indentation is the only structure, so
// the walk below keys off the two markers that matter and reads forward.
function pptrCurves(text) {
  const start = text.indexOf('m_PPtrCurves:');
  if (start < 0) return [];
  const end = text.indexOf('\n  m_SampleRate:', start);
  const block = text.slice(start, end < 0 ? text.length : end);
  const out = [];
  // split on the curve entries ("  - curve:")
  const parts = block.split(/\n  - curve:/).slice(1);
  for (const part of parts) {
    const keys = [];
    const re = /- time: ([-\d.eE+]+)\s*\n\s*value: \{fileID: (\d+)(?:, guid: ([0-9a-f]+))?/g;
    let m;
    while ((m = re.exec(part))) {
      if (!m[3]) continue;                       // {fileID: 0} = a null sprite key; nothing to draw
      keys.push({ t: parseFloat(m[1]), guid: m[3] });
    }
    const attribute = (/\n\s*attribute: (.*)/.exec(part) || [, ''])[1].trim();
    const p = (/\n\s*path: (.*)/.exec(part) || [, ''])[1].trim();
    if (keys.length) out.push({ attribute: attribute, path: p, keys: keys });
  }
  return out;
}

if (!fs.existsSync(CLIPS)) {
  console.log('no Assets/AnimationClip directory -- nothing to bake');
  process.exit(0);
}

const index = {};
const spriteGuids = new Set();
let totalKeys = 0, skipped = 0;
for (const file of fs.readdirSync(CLIPS).filter(function (f) { return f.endsWith('.anim'); })) {
  const text = fs.readFileSync(path.join(CLIPS, file), 'utf8');
  const name = scalar(text, 'm_Name', path.basename(file, '.anim'));
  const tracks = pptrCurves(text);
  if (!tracks.length) {
    // A clip with no sprite keys animates something else (transform, a component field). Report
    // it rather than silently dropping it -- the next person needs to know the gap exists.
    console.log('  no m_Sprite keys: ' + file + ' (animates something this baker does not model)');
    skipped++;
    continue;
  }
  let length = 0;
  for (const tr of tracks) {
    for (const k of tr.keys) { if (k.t > length) length = k.t; spriteGuids.add(k.guid); }
    totalKeys += tr.keys.length;
  }
  index[name] = {
    rel: 'AnimationClip/' + file,
    sampleRate: parseFloat(scalar(text, 'm_SampleRate', '60')),
    loop: scalar(text, 'm_LoopTime', '0') === '1',
    length: length,
    tracks: tracks
  };
  console.log('  ' + name.padEnd(16) + ' ' + String(tracks.length) + ' track(s), ' +
              String(tracks.reduce(function (s, t) { return s + t.keys.length; }, 0)).padStart(4) +
              ' keys, ' + length.toFixed(2) + 's' + (index[name].loop ? ', LOOPS' : ''));
}

fs.writeFileSync(path.join(DATA, 'anims-index.json'), JSON.stringify(index, null, 1));
console.log('baked ' + Object.keys(index).length + ' clips (' + skipped + ' skipped), ' +
            totalKeys + ' sprite keys -> data/anims-index.json');

// What the texture bake must cover before any of this can draw. Reported, not acted on: growing
// the bake is a decision with a memory cost (see the --report cap table), not a side effect.
const texIndexPath = path.join(DATA, 'textures-index.json');
if (fs.existsSync(texIndexPath)) {
  const baked = JSON.parse(fs.readFileSync(texIndexPath, 'utf8'));
  const missing = [...spriteGuids].filter(function (g) { return !baked[g]; });
  console.log('  ' + spriteGuids.size + ' distinct sprites referenced; ' + missing.length +
              ' of them are NOT in textures-index.json yet');
}
