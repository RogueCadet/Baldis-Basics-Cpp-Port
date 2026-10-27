'use strict';
// pixel-sanity.js  -  fail a --shot capture that is visually empty.
//
// Written after session 8a, where EVERY 3D scene rendered solid black for a whole session and
// `shell-smoke.ps1` reported "ok" for all seven of them. The smoke only ever checked that the
// process exited 0 and that the scene loaded some GameObjects -- both true of a completely black
// frame. A gate that cannot see the picture cannot catch a renderer regression.
//
// The checks are deliberately crude, because a crude check that runs every build beats a precise
// one that needs a human:
//   1. the frame must not be ONE uniform colour (that is the failure that actually happened);
//   2. it must have at least `--min-colours` distinct colours (default 2);
//   3. no single colour may cover more than `--max-share` of it (default 0.995).
//
// THE DEFAULTS COME FROM MEASURING THE SEVEN SCENES, not from taste. The first version of this
// tool used 12 colours / 97% and FAILED three correct renders, because it assumed a real frame is
// visually rich:
//     School  84 colours, top 10.3%      MainMenu 114, 57.5%     Secret  16, 56.8%
//     TestRoom 16 colours, top 17.9%     GameOver  88, 99.0%     Results  9, 60.0%
//     Warning  2 colours, top 90.6%
// Warning is legitimately white-on-black (2 colours) and GameOver is a music box on black (one
// colour over 99% of the frame). Both are CORRECT. So the only defensible floor is "not uniform",
// and the share ceiling has to sit above GameOver's 99.0%. A solid-black frame is 1 colour at
// 100% and fails both.
//
// It says nothing about whether the picture is RIGHT; `run/ref-*.png` and a human eye still do
// that. It only asserts that something was drawn.
//
// Usage: node tools\pixel-sanity.js <shot.raw> [--min-colours N] [--max-share F] [--quiet]
//   Reads the "<w> <h>" sidecar that --shot writes next to the blob.
//   Exit 0 = plausible, 1 = empty/uniform, 2 = usage or unreadable input.
const fs = require('fs');

const argv = process.argv.slice(2);
const inPath = argv.find(function (a) { return !a.startsWith('--'); });
if (!inPath) {
  console.error('usage: node tools\\pixel-sanity.js <shot.raw> [--min-colours N] [--max-share F]');
  process.exit(2);
}
function numArg(name, def) {
  const i = argv.indexOf(name);
  return i >= 0 ? parseFloat(argv[i + 1]) : def;
}
const minColours = numArg('--min-colours', 2);
const maxShare = numArg('--max-share', 0.995);
const quiet = argv.indexOf('--quiet') >= 0;

let w = 0, h = 0;
try {
  const side = fs.readFileSync(inPath + '.txt', 'utf8').trim().split(/\s+/).map(Number);
  w = side[0];
  h = side[1];
} catch (e) {
  console.error('pixel-sanity: no sidecar ' + inPath + '.txt');
  process.exit(2);
}
let px;
try {
  px = fs.readFileSync(inPath);
} catch (e) {
  console.error('pixel-sanity: cannot read ' + inPath);
  process.exit(2);
}
if (px.length !== w * h * 4) {
  console.error('pixel-sanity: ' + inPath + ' is ' + px.length + ' bytes, expected ' +
                (w * h * 4) + ' for ' + w + 'x' + h);
  process.exit(2);
}

// Sample on a stride rather than every pixel: a 1920x1056 frame is 2M pixels and this runs per
// scene per build. A prime stride avoids landing on a regular grid of a tiled texture.
const STRIDE = 101;
const counts = new Map();
let n = 0;
for (let i = 0; i + 3 < px.length; i += 4 * STRIDE) {
  const key = (px[i] << 16) | (px[i + 1] << 8) | px[i + 2];
  counts.set(key, (counts.get(key) || 0) + 1);
  n++;
}
let topKey = 0, topCount = 0;
counts.forEach(function (c, k) { if (c > topCount) { topCount = c; topKey = k; } });
const share = n ? topCount / n : 1;
const rgb = ((topKey >> 16) & 255) + ',' + ((topKey >> 8) & 255) + ',' + (topKey & 255);

const problems = [];
if (counts.size <= 1) problems.push('the entire frame is ONE colour (' + rgb + ')');
if (counts.size < minColours) {
  problems.push(counts.size + ' distinct colours, expected at least ' + minColours);
}
if (share > maxShare) {
  problems.push('one colour (' + rgb + ') covers ' + (share * 100).toFixed(1) +
                '% of the frame, limit ' + (maxShare * 100).toFixed(1) + '%');
}

if (problems.length) {
  console.error('PIXEL SANITY FAILED  ' + inPath + '  (' + w + 'x' + h + ')');
  for (const p of problems) console.error('   ' + p);
  console.error('   A capture this empty means the render path drew nothing. shell-smoke used to');
  console.error('   pass exactly this: exit 0, scene loaded, frame solid black.');
  process.exit(1);
}
if (!quiet) {
  console.log('  pixels ok  ' + inPath + '  ' + counts.size + ' colours, top ' + rgb +
              ' at ' + (share * 100).toFixed(1) + '%');
}
