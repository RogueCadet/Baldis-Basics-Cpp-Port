'use strict';
// bake-controllers.js  -  bake Unity AnimatorController assets into a flat clip table.
//
// The scene bakes carry Animator components with only the controller guid (7 of them in
// School/Secret/MainMenu + the MathGame prefabs). Which clip each controller plays -- and at what
// state speed -- lives in the .controller YAML under Assets/AnimationClip/AnimatorController/.
// Without this table the sim could pin the anim-index bake but nothing could resolve "the
// BaldiTutor controller plays BaldiWave at speed 0.5", so baldiTutor stood frozen (Baldi said hi
// but did NOT wave, user report).
//
// Structure parsed (a strict subset the hand-writer always emits):
//   --- !u!1102 &<stateId>   AnimatorState   -> m_Name, m_Speed, m_Motion {fileID, guid}
//   --- !u!1107 &<smId>      AnimatorStateMachine -> m_DefaultState {fileID}
// Clip guids resolve through data/manifest.json (guid -> rel) to the .anim clip name, which is also
// the key in data/anims-index.json.
//
// Output: data/anims-controllers.json
//   { <controller guid>: { default: {clip, speed}, states: { <state name>: {clip, speed} } } }
// Usage: node tools\bake-controllers.js
const fs = require('fs');
const path = require('path');

const ASSETS = 'F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/';
const DATA = path.join(__dirname, '..', 'data');
const CTRL_DIR = path.join(ASSETS, 'AnimationClip', 'AnimatorController');
const manifest = JSON.parse(fs.readFileSync(path.join(DATA, 'manifest.json'), 'utf8')).guidMap;

function docId(line) {
  const m = /&(-?\d+)/.exec(line);
  return m ? m[1] : null;
}
function scalarLine(text, key) {
  const m = new RegExp('^\\s*' + key + ': (.*)$', 'm').exec(text);
  return m ? m[1].trim() : '';
}

const out = {};
const files = fs.readdirSync(CTRL_DIR).filter((f) => f.endsWith('.controller'));
for (const file of files) {
  const inPath = path.join(CTRL_DIR, file);
  const text = fs.readFileSync(inPath, 'utf8');
  // controller guid from its .meta (same scheme bake-anims.js avoids, but controllers are few and
  // the scene bakes key the Animator binding BY guid, so the meta guid is required)
  const metaPath = inPath + '.meta';
  const gm = /guid: ([0-9a-f]{32})/.exec(fs.readFileSync(metaPath, 'utf8'));
  const ctrlGuid = gm ? gm[1] : '';
  if (!ctrlGuid) { console.error('  no guid in meta for ' + file); continue; }

  // split documents: iterate lines, each `--- !u!<klass> &<id>` starts a document
  const states = {};                                      // stateId -> {name, speed, motionGuid}
  let defaultStateId = '';
  // TRANSITIONS + PARAMETERS. The table used to carry states only, so the runtime could never leave
  // the default state: the ThinkPad Baldi stayed on Baldi_Idle while talking (BaldiFeed.controller:
  // Idle -> Talk when `talking`, Talk -> Idle when not, Any State -> Frown on `angry`), and Baldi's
  // `slap`, Playtime's `disappointed` and the jump rope's `ActivateJumpRope` did nothing either.
  const transitions = {};                                 // transitionId -> {dstId, conds, hasExitTime, exitTime}
  const stateTransIds = {};                               // stateId -> [transitionId]
  let anyTransIds = [];
  const params = {};                                      // name -> {type, def}
  const idList = (body, key) => {
    const i = body.indexOf(key + ':');
    if (i < 0) return [];
    const out = [];
    const lines = body.slice(i).split('\n').slice(1);
    for (const l of lines) {
      const m = /^\s*- \{fileID: (-?\d+)\}/.exec(l);
      if (!m) break;
      out.push(m[1]);
    }
    return out;
  };
  let curKlass = 0, curId = '', curBody = [];
  const flush = function () {
    if (!curKlass) return;
    const body = curBody.join('\n');
    if (curKlass === 1102) {                               // AnimatorState
      const name = scalarLine(body, 'm_Name');
      const sp = parseFloat(scalarLine(body, 'm_Speed') || '1');
      const mo = /m_Motion: \{fileID: (\d+), guid: ([0-9a-f]{32})?/.exec(body);
      states[curId] = { name: name, speed: isFinite(sp) ? sp : 1.0, motionGuid: mo ? (mo[2] || '') : '' };
      stateTransIds[curId] = idList(body, 'm_Transitions');
    } else if (curKlass === 1107) {                        // AnimatorStateMachine
      const ds = scalarLine(body, 'm_DefaultState');
      const m = /fileID: (-?\d+)/.exec(ds || '');
      if (m) defaultStateId = m[1];
      anyTransIds = anyTransIds.concat(idList(body, 'm_AnyStateTransitions'));
    } else if (curKlass === 1101) {                        // AnimatorStateTransition
      const dst = /m_DstState: \{fileID: (-?\d+)\}/.exec(body);
      const conds = [];
      const re = /m_ConditionMode: (\d+)\s*\n\s*m_ConditionEvent: ([^\n]+?)\s*\n\s*m_EventTreshold: ([-\d.eE]+)/g;
      let cm;
      while ((cm = re.exec(body)) !== null) conds.push([parseInt(cm[1], 10), cm[2].trim(), parseFloat(cm[3])]);
      transitions[curId] = {
        dstId: dst ? dst[1] : '0',
        conds: conds,
        hasExitTime: (scalarLine(body, 'm_HasExitTime') || '0').trim() === '1',
        exitTime: parseFloat(scalarLine(body, 'm_ExitTime') || '0') || 0,
        canTransitionToSelf: (scalarLine(body, 'm_CanTransitionToSelf') || '1').trim() === '1',
      };
    } else if (curKlass === 91) {                          // AnimatorController: parameters
      const re = /- m_Name: ([^\n]+?)\s*\n\s*m_Type: (\d+)\s*\n\s*m_DefaultFloat: ([-\d.eE]+)\s*\n\s*m_DefaultInt: (-?\d+)\s*\n\s*m_DefaultBool: (\d)/g;
      let pm;
      while ((pm = re.exec(body)) !== null) {
        const type = parseInt(pm[2], 10);               // 1 float, 3 int, 4 bool, 9 trigger
        params[pm[1].trim()] = { type: type, def: type === 1 ? parseFloat(pm[3]) : type === 3 ? parseInt(pm[4], 10) : parseInt(pm[5], 10) };
      }
    }
  };
  for (const line of text.split('\n')) {
    const m = /^--- !u!(\d+) &(-?\d+)/.exec(line);
    if (m) {
      flush();
      curKlass = parseInt(m[1], 10);
      curId = m[2];
      curBody = [];
    } else if (curKlass) {
      curBody.push(line);
    }
  }
  flush();
  const defaultState = states[defaultStateId];
  const entry = { states: {} };
  let anyState = false;
  for (const stateId of Object.keys(states)) {
    const s = states[stateId];
    const rel = s.motionGuid && manifest[s.motionGuid] ? manifest[s.motionGuid].rel : '';
    const clip = rel ? path.basename(rel, '.anim') : '';
    if (!clip) {
      console.error('  ' + file + ' state "' + s.name + '" motion unresolved (guid ' + s.motionGuid.slice(0, 8) + ')');
      continue;
    }
    entry.states[s.name || clip] = { clip: clip, speed: s.speed };
    if (stateId === defaultStateId) { entry.default = { clip: clip, speed: s.speed }; }
    anyState = true;
  }
  if (!anyState) { console.error('  no clip states resolved for ' + file); continue; }
  if (!entry.default) {
    // fall back to the first state (Unity would play the default; those controllers have exactly one)
    const first = Object.keys(entry.states)[0];
    entry.default = { clip: entry.states[first].clip, speed: entry.states[first].speed };
    console.error('  ' + file + ' had no default state; using "' + first + '"');
  }
  // transitions by state NAME (the runtime keys states by name); a transition with no destination
  // state (exit / sub-machine) is dropped -- none of this game's controllers use one.
  const pack = (ids) => ids.map((id) => transitions[id]).filter((t) => t && states[t.dstId]).map((t) => ({
    to: states[t.dstId].name, conds: t.conds, hasExitTime: t.hasExitTime, exitTime: t.exitTime,
    canTransitionToSelf: t.canTransitionToSelf }));
  for (const stateId of Object.keys(states)) {
    const nm = states[stateId].name;
    if (entry.states[nm]) entry.states[nm].transitions = pack(stateTransIds[stateId] || []);
  }
  entry.anyState = pack(anyTransIds);
  entry.params = params;
  if (states[defaultStateId]) entry.defaultState = states[defaultStateId].name;
  out[ctrlGuid] = entry;
  console.log('  %-24s guid %s default "%s" speed %s states %d' % (
    path.basename(file, '.controller'), ctrlGuid.slice(0, 8),
    entry.default.clip, entry.default.speed, Object.keys(entry.states).length));
}

fs.writeFileSync(path.join(DATA, 'anims-controllers.json'), JSON.stringify(out, null, 1));
console.log('  controllers: ' + Object.keys(out).length + ' -> data/anims-controllers.json');
