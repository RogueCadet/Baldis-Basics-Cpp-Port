'use strict';
// scan.js  -  Phase 0 pipeline: GUID registry + scene/prefab/material/anim parse + graph emit.
const fs = require('fs');
const path = require('path');
const { splitDocs, parseBody } = require('./unity-yaml');

const ROOTS = [
  'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets',
  'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/ProjectSettings',
];
const OUT = 'F:/baldidecomp/baldi_cpp_port/data';

// Canonical fileID spelling: an exact decimal STRING, or null when the value is not an id.
// unity-yaml hands back a number for ids that fit in a JS double and a string for the ones that
// do not (prefab ids reach ~9.06e18), so every consumer must accept both and normalise. fileIDs
// are only ever used as opaque 64-bit keys -- never arithmetic -- so a string is lossless.
function fidStr(v) {
  if (typeof v === 'string') return /^-?\d+$/.test(v) ? v : null;
  if (typeof v === 'number' && Number.isInteger(v)) return String(v);
  return null;
}

const PARSE_EXTS = new Set(['.unity', '.prefab', '.mat', '.asset', '.anim', '.controller']);
const GAME_SCENES = new Set(['School', 'MainMenu', 'Secret', 'TestRoom', 'GameOver', 'Results', 'Warning']);
const MAX_ARR = 128; // arrays longer than this are collapsed in emitted graphs

const files = [];
function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else files.push(p);
  }
}
for (const r of ROOTS) walk(r);

// Asset-root-relative path with forward slashes, e.g. "Audio/NPC/1PR_AmComing.wav".
//
// NOTE: `files` is built with path.join, which emits BACKSLASHES on Windows, while ROOTS are
// written with forward slashes. Comparing the two raw made every startsWith() fail, so this
// silently fell through to path.basename() for all 1231 assets -- the directory was dropped from
// every manifest rel. That broke the engine's `assetRoot + rel` WAV reads (no file at
// Assets/1PR_AmComing.wav), so parseWavDuration returned its 1.0s fallback for EVERY clip and
// nothing ever reported an error. Normalise both sides before comparing.
function relOf(f) {
  const n = f.replace(/\\/g, '/');
  for (const r of ROOTS) {
    const root = r.replace(/\\/g, '/');
    if (n.startsWith(root + '/')) return n.slice(root.length + 1);
  }
  return path.basename(n);
}

// ---- GUID registry from .meta files -------------------------------------
const guidMap = {};
for (const f of files) {
  if (!f.endsWith('.meta')) continue;
  let text;
  try { text = fs.readFileSync(f, 'utf8'); } catch (e) { continue; }
  const m = /guid:\s*([0-9a-f]{32})/.exec(text);
  if (!m) continue;
  const target = f.slice(0, -5);
  guidMap[m[1]] = { rel: relOf(target), ext: path.extname(target).toLowerCase() };
}

// ---- helpers -------------------------------------------------------------
function stripBig(v, depth) {
  if (depth > 24) return { __cut: 'depth' };
  if (Array.isArray(v)) {
    if (v.length > MAX_ARR) return { __arrayLen: v.length, __sample: v.slice(0, 3) };
    return v.map((x) => stripBig(x, depth + 1));
  }
  if (v && typeof v === 'object') {
    const o = {};
    for (const k of Object.keys(v)) o[k] = stripBig(v[k], depth + 1);
    return o;
  }
  return v;
}

function collectGuids(v, depth, set) {
  if (depth > 24 || v == null || typeof v !== 'object') return;
  if (Array.isArray(v)) { for (const x of v) collectGuids(x, depth + 1, set); return; }
  // fileID may be a number or (past 2^53) an exact decimal string -- see unity-yaml scalarNum.
  if ((typeof v.fileID === 'number' || typeof v.fileID === 'string') && typeof v.guid === 'string') {
    set.add(v.guid); return;
  }
  for (const k of Object.keys(v)) collectGuids(v[k], depth + 1, set);
}

// ---- parse targets -------------------------------------------------------
const stats = {
  files: {}, errors: [], classHist: {}, sceneSummaries: {}, scriptUsage: {}, typeBins: {},
};

for (const f of files) {
  const ext = path.extname(f).toLowerCase();
  if (!PARSE_EXTS.has(ext)) continue;
  const rel = relOf(f);
  let text;
  try { text = fs.readFileSync(f, 'utf8'); } catch (e) { stats.errors.push(`read ${rel}: ${e.message}`); continue; }

  let docs;
  try { docs = splitDocs(text); }
  catch (e) { stats.errors.push(`split ${rel}: ${e.message}`); continue; }

  const objs = new Map();
  const localClassHist = {};
  const guidRefs = new Set();
  let parseErr = 0;
  for (const d of docs) {
    let fieldsRaw = null;
    try {
      const root = parseBody(d.body);
      fieldsRaw = root && Object.prototype.hasOwnProperty.call(root, d.className) ? root[d.className] : root;
    } catch (e) { parseErr++; continue; }
    localClassHist[d.className] = (localClassHist[d.className] || 0) + 1;
    stats.classHist[d.className] = (stats.classHist[d.className] || 0) + 1;
    collectGuids(fieldsRaw, 0, guidRefs);
    objs.set(d.fileId, { classId: d.classId, cls: d.className, fields: fieldsRaw });
  }
  if (parseErr) stats.errors.push(`${rel}: ${parseErr} doc(s) failed to parse`);

  stats.files[rel] = { ext, size: text.length, docs: docs.length, objs: objs.size, classHist: localClassHist, guidRefs: [...guidRefs] };

  const scripts = new Set();
  for (const o of objs.values()) {
    if (o.cls === 'MonoBehaviour' && o.fields && o.fields.m_Script && o.fields.m_Script.guid) {
      const gm = guidMap[o.fields.m_Script.guid];
      if (gm) scripts.add(path.posix.basename(gm.rel, '.cs'));
    }
  }
  if (scripts.size) stats.scriptUsage[rel.replace(/^Scene\//, '')] = [...scripts].sort();

  // Graph emission is identical for scenes and prefabs: a .prefab carries the same GameObject /
  // Transform / component documents a .unity does (plus one `Prefab` doc, which has no
  // m_GameObject and is therefore skipped by the same branch that skips scene metadata).
  // `kind` becomes the output prefix: scene-<Name>.graph.json / prefab-<Name>.graph.json.
  function emitGraph(kind, name) {
    const graph = { name, objects: {}, components: {} };
    const usedScripts = new Set();
    for (const [id, o] of objs) {
      const fd = o.fields || {};
      if (o.cls === 'GameObject') {
        graph.objects[id] = {
          n: fd.m_Name || '', layer: fd.m_Layer, active: !!fd.m_IsActive,
          tag: fd.m_TagString, staticFlags: fd.m_StaticEditorFlags,
          comps: Array.isArray(fd.m_Component)
            ? fd.m_Component.map((c) => (c.component ? fidStr(c.component.fileID) : null)) : [],
        };
      } else if (o.cls === 'RenderSettings') {
        // Unity's per-scene render settings. Only the skybox survives: m_SkyboxMaterial as the
        // referenced MATERIAL guid. The built-in default material guid (0000000000000000f0...)
        // is not a textured skybox; the engine keeps its clear colour there, which is Unity's
        // behaviour for scenes that leave it at default too.
        const sb = fd.m_SkyboxMaterial;
        if (sb && sb.guid) graph.settings = { skybox: sb.guid };
      } else if (fd && fd.m_GameObject && fidStr(fd.m_GameObject.fileID) !== null) {
        graph.components[id] = { t: o.cls, go: fidStr(fd.m_GameObject.fileID), f: stripBig(fd, 0) };
        if (o.cls === 'MonoBehaviour' && fd.m_Script && fd.m_Script.guid) {
          const gm = guidMap[fd.m_Script.guid];
          if (gm) usedScripts.add(path.posix.basename(gm.rel, '.cs'));
        }
      }
    }
    fs.writeFileSync(path.join(OUT, `${kind}-${name}.graph.json`), JSON.stringify(graph, null, 1));
    const summary = {
      objects: Object.keys(graph.objects).length,
      components: Object.keys(graph.components).length,
      scripts: [...usedScripts],
    };
    if (kind === 'scene') stats.sceneSummaries[name] = summary;
    else (stats.prefabSummaries = stats.prefabSummaries || {})[name] = summary;
  }

  const sceneName = path.basename(f, '.unity');
  if (ext === '.unity' && GAME_SCENES.has(sceneName)) emitGraph('scene', sceneName);

  // The game's own prefabs all live under Assets/PrefabInstance/. The 3 TMP example prefabs
  // elsewhere are excluded permanently per charter.
  if (ext === '.prefab' && relOf(f).startsWith('PrefabInstance/')) {
    emitGraph('prefab', path.basename(f, '.prefab'));
  }
}

// bins for non-scene yaml (prefabs, materials, anims, controllers, assets)
for (const [rel, info] of Object.entries(stats.files)) {
  (stats.typeBins[info.ext] = stats.typeBins[info.ext] || []).push(rel);
}

fs.writeFileSync(path.join(OUT, 'manifest.json'), JSON.stringify({ generatedAt: new Date().toISOString(), guidCount: Object.keys(guidMap).length, guidMap }, null, 1));
fs.writeFileSync(path.join(OUT, 'stats.json'), JSON.stringify(stats, null, 1));

// ---- console report -------------------------------------------------------
let parsed = 0, docTotal = 0;
for (const s of Object.values(stats.files)) { parsed++; docTotal += s.docs; }
console.log(`parsed ${parsed} files / ${docTotal} documents / guid map ${Object.keys(guidMap).length} / errors ${stats.errors.length}`);
for (const [n, s] of Object.entries(stats.sceneSummaries)) {
  console.log(`scene ${n}: ${s.objects} objects, ${s.components} components, ${s.scripts.length} scripts: ${s.scripts.join(', ')}`);
}
const classPairs = Object.entries(stats.classHist).sort((a, b) => b[1] - a[1]);
console.log(`class histogram (top 20): ${classPairs.slice(0, 20).map(([k, v]) => `${k}:${v}`).join(' ')}`);
if (stats.errors.length) console.log(`errors (first 10):\n  ${stats.errors.slice(0, 10).join('\n  ')}`);
