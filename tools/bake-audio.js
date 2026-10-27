// bake-audio.js  -  copy the decompile's shipped WAV assets into the port's data tree.
//
// The engine resolves audio guids through data/manifest.json ("AudioClip/.../BAL_Slap.wav",
// authored by the earlier asset scan) and opens them at <assetRoot><rel>. Until now assetRoot
// pointed at the DECOMPILE tree, which made the dev runs work but left the packaged game without
// any audio files. This bake copies every shipped *.wav into data\Assets\AudioClip\... with its
// original relative path, so:
//   dev tree:    assetRoot "data/Assets/"  ->  data/Assets/AudioClip/...
//   packaged:    package.ps1 copies data\* into content\data\..., assetRoot still "data/Assets/"
// Both self-contained; the decompile tree stops being a runtime dependency.
//
// Fidelity note: tags verified across all 127 clips = plain PCM16 mono/stereo at
// 8k/22.05k/44.1k/48k (python RIFF scan, 2026-10-24), so the engine loader needs no ADPCM path.
//
// Usage: node tools\bake-audio.js        (idempotent: overwrites, reports count + bytes)
const fs = require('fs');
const path = require('path');

const SRC = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/AudioClip';
const DST = 'data/Assets/AudioClip';

function walk(dir) {
  let out = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) out = out.concat(walk(p));
    else if (e.name.endsWith('.wav')) out.push(p);
  }
  return out;
}

const files = walk(SRC);
let n = 0, bytes = 0;
for (const src of files) {
  const rel = path.relative(SRC, src);
  const dst = path.join(DST, rel);
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  fs.copyFileSync(src, dst);
  n++; bytes += fs.statSync(dst).size;
}
console.log('AUDIO BAKE: ' + n + ' wav files, ' + (bytes / 1048576).toFixed(1) + ' MB -> ' + DST);
