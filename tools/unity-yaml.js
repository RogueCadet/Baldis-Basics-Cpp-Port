'use strict';
// unity-yaml.js  -  reader for the YAML serialization subset Unity 2018.3 emits in scenes,
// prefabs, materials, meshes, anims, controllers, .asset files.
// This is NOT a general YAML parser; it targets exactly the constructs Unity writes.

function unquote(v) {
  if (v.length >= 2 && ((v[0] === '"' && v[v.length - 1] === '"') || (v[0] === "'" && v[v.length - 1] === "'"))) {
    let s = v.slice(1, -1);
    s = s.replace(/\\'/g, "'").replace(/\\"/g, '"').replace(/\\\\/g, '\\')
      .replace(/\\n/g, '\n').replace(/\\t/g, '\t');
    return s;
  }
  return v;
}

// Unity fileIDs routinely exceed 2^53 (prefab ids run to ~9.06e18, and MainMenu.unity carries 192
// of them), so parseInt would silently ROUND them: distinct objects collapse onto one id and the
// GameObject <-> component links break. Integers that cannot round-trip through a JS number are
// therefore kept as exact decimal STRINGS. Consumers downstream treat a fileID as an opaque
// 64-bit key (JSON object keys and C++ atoll), never as arithmetic, so a string is lossless.
function scalarNum(raw) {
  if (!/[.eE]/.test(raw)) {
    const n = parseInt(raw, 10);
    return String(n) === raw.replace(/^\+/, '') ? n : raw;
  }
  return parseFloat(raw);
}

function scalar(v) {
  v = v.trim();
  if (v === '' || v === 'null' || v === 'Null') return null;
  if (v === 'true') return true;
  if (v === 'false') return false;
  if (v[0] === '{' || v[0] === '[') return inline(v);
  if (v[0] === '"' || v[0] === "'") return unquote(v);
  // A Unity guid is exactly 32 lowercase hex chars and must stay a STRING. Unity's built-in asset
  // guids look numeric to the test below -- `0000000000000000e000000000000000` contains an 'e', so
  // it was read as scientific notation and collapsed to the number 0, silently erasing the guid
  // from every built-in mesh/material reference in every scene. (The f0... built-ins fared no
  // better: parseInt stops at the 'f' and also yields 0.)
  if (/^[0-9a-f]{32}$/.test(v)) return v;
  if (/^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$|^[-+]?\d+$/.test(v)) return scalarNum(v);
  return v;
}

// inline maps/lists: {fileID: 123, guid: abc, type: 3} / [1, 2, 3]
function inline(s) {
  let i = 0;
  const n = s.length;
  const ws = () => { while (i < n && s[i] === ' ') i++; };
  function until(ch) {
    const st = i;
    while (i < n && s[i] !== ch) i++;
    return s.slice(st, i);
  }
  function val() {
    ws();
    if (i < n && s[i] === '{') {
      i++; const o = {}; ws();
      if (s[i] === '}') { i++; return o; }
      for (;;) {
        ws();
        const key = until(':');
        i++; // ':'
        o[key.trim()] = val();
        ws();
        if (s[i] === ',') { i++; continue; }
        if (s[i] === '}') { i++; break; }
        break; // malformed tail; bail best-effort
      }
      return o;
    }
    if (i < n && s[i] === '[') {
      i++; const a = []; ws();
      if (s[i] === ']') { i++; return a; }
      for (;;) {
        a.push(val());
        ws();
        if (s[i] === ',') { i++; continue; }
        if (s[i] === ']') { i++; break; }
        break;
      }
      return a;
    }
    if (i < n && (s[i] === '"' || s[i] === "'")) {
      const q = s[i]; const st = i; i++;
      while (i < n) {
        if (s[i] === '\\') { i += 2; continue; }
        if (s[i] === q) { i++; break; }
        i++;
      }
      return unquote(s.slice(st, i));
    }
    const st = i;
    while (i < n && s[i] !== ',' && s[i] !== '}' && s[i] !== ']') i++;
    return scalar(s.slice(st, i));
  }
  return val();
}

function lead(s) {
  let n = 0;
  while (n < s.length && s[n] === ' ') n++;
  return n;
}

// body = lines after the "<ClassName>:" root line
function parseBody(lines) {
  let idx = 0;
  const skipBlank = () => {
    while (idx < lines.length && lines[idx].trim() === '') idx++;
  };

  // value under a key written at parentIndent
  function parseValue(parentIndent) {
    skipBlank();
    if (idx >= lines.length) return null;
    const ind = lead(lines[idx]);
    const c0 = lines[idx].slice(ind);
    if (ind > parentIndent) return parseNode(ind);
    if (ind === parentIndent && (c0.startsWith('- ') || c0 === '-')) return parseList(ind); // YAML seq at key's indent
    return null;
  }

  function parseNode(firstIndent) {
    const content = lines[idx].slice(firstIndent);
    if (content.startsWith('- ') || content === '-') return parseList(firstIndent);
    if (content.startsWith('{') || content.startsWith('[')) { idx++; return inline(content); } // flow value (defensive; Unity normally inlines on the key line)
    return parseMap(firstIndent);
  }

  function parseList(li) {
    const arr = [];
    for (;;) {
      skipBlank();
      if (idx >= lines.length) break;
      const l = lines[idx];
      const ind = lead(l);
      if (ind < li) break;
      if (ind > li) { idx++; continue; } // defensive
      const c = l.slice(ind);
      if (!(c.startsWith('- ') || c === '-')) break;
      if (c === '-') { idx++; arr.push(parseValue(li + 1)); continue; }
      const rest = c.slice(2);
      if (rest[0] === '{' || rest[0] === '[') { arr.push(inline(rest)); idx++; continue; } // flow list entry: {fileID: N} / {fileID, guid, type}
      if (/^[-+0-9.eE]+$/.test(rest)) { arr.push(scalarNum(rest)); idx++; continue; } // fast path for huge numeric lists
      lines[idx] = ' '.repeat(li + 2) + rest; // normalize entry to a value line
      arr.push(parseValue(li + 1));
    }
    return arr;
  }

  // ---- multi-line QUOTED scalars ------------------------------------------------------------
  // Unity wraps any long string this way, and the menu's mode descriptions are exactly that:
  //
  //     m_text: 'Story Mode:
  //
  //         Collect all 7 notebooks, and then exit the school, to win!'
  //
  // Reading only the key's own line kept the OPENING QUOTE as text and silently discarded every
  // continuation line, so the selection menu rendered "'Story Mode:" with no description -- the
  // leading apostrophe the user spotted was the YAML quote itself.
  //
  // YAML flow folding, which is what makes the result two lines rather than one:
  //   - continuation lines are stripped of their indentation;
  //   - a SINGLE line break between two non-empty lines folds to a SPACE;
  //   - a run of N line breaks (i.e. blank lines between them) yields N-1 newlines.
  // Inside single quotes, '' is a literal apostrophe; inside double quotes, backslash escapes
  // apply and are left to unquote().
  function quoteCloses(text) {
    const q = text[0];
    for (let i = 1; i < text.length; i++) {
      if (text[i] !== q) continue;
      if (q === "'" && text[i + 1] === "'") { i++; continue; }   // '' escape, not the end
      if (q === '"' && text[i - 1] === '\\') continue;           // escaped quote
      return true;
    }
    return false;
  }

  function readMultilineQuoted(firstLine) {
    const q = firstLine[0];
    const parts = [firstLine.slice(1)];          // drop the opening quote, keep the rest verbatim
    while (idx < lines.length) {
      const raw = lines[idx];
      idx++;
      const body = raw.trim();                   // continuation indentation is not content
      parts.push(body);
      if (body !== '' && quoteClosesTail(body, q)) {
        parts[parts.length - 1] = body.slice(0, body.lastIndexOf(q));
        break;
      }
    }
    // Fold: walk the collected lines, turning blank-line runs into newlines and single breaks
    // into spaces.
    let out = '';
    let pendingBreaks = 0;
    for (let i = 0; i < parts.length; i++) {
      const piece = parts[i];
      if (i > 0) {
        if (piece === '') { pendingBreaks++; continue; }
        out += pendingBreaks > 0 ? '\n'.repeat(pendingBreaks) : ' ';
        pendingBreaks = 0;
      }
      out += piece;
    }
    if (q === "'") out = out.split("''").join("'");
    return out;
  }

  function quoteClosesTail(text, q) {
    for (let i = 0; i < text.length; i++) {
      if (text[i] !== q) continue;
      if (q === "'" && text[i + 1] === "'") { i++; continue; }
      if (q === '"' && i > 0 && text[i - 1] === '\\') continue;
      return true;
    }
    return false;
  }

  function parseMap(mi) {
    const obj = {};
    for (;;) {
      skipBlank();
      if (idx >= lines.length) break;
      const l = lines[idx];
      const ind = lead(l);
      if (ind < mi) break;
      if (ind > mi) { idx++; continue; } // defensive
      const c = l.slice(ind);
      if (c.startsWith('- ')) break;
      const m = /^(\S.*?):(.*)$/.exec(c);
      if (!m) { // bare scalar value (e.g. TagManager tag lists)
        idx++;
        obj.__scalar = scalar(c);
        break;
      }
      const key = m[1];
      const after = m[2].trim();
      idx++;
      if (after !== '' && (after[0] === "'" || after[0] === '"') && !quoteCloses(after)) {
        obj[key] = readMultilineQuoted(after);   // see below: a YAML flow scalar spanning lines
      } else {
        obj[key] = after === '' ? parseValue(mi) : scalar(after);
      }
    }
    return obj;
  }

  return parseValue(0);
}

module.exports = { splitDocs, parseBody, scalar, inline };

function splitDocs(text) {
  const lines = text.split(/\r?\n/);
  const docs = [];
  for (let i = 0; i < lines.length; i++) {
    if (!lines[i].startsWith('--- !u!')) continue;
    const m = /^--- !u!(\d+) &(-?\d+)/.exec(lines[i]);
    if (!m) continue;
    let j = i + 1;
    while (j < lines.length && !lines[j].startsWith('--- !u!')) j++;
    let k = i + 1;
    while (k < j && lines[k].trim() === '') k++;
    const nm = /^(\S+):\s*$/.exec(lines[k] || '');
    docs.push({
      classId: Number(m[1]),
      // The document anchor is a fileID: keep it EXACT as a decimal string (see scalarNum).
      // Number() here rounded every prefab id past 2^53 into collisions.
      fileId: m[2],
      className: nm ? nm[1] : String(m[1]),
      body: lines.slice(k + 1, j),
    });
    i = j - 1;
  }
  return docs;
}
