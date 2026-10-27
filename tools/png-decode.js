'use strict';
// png-decode.js  -  minimal PNG -> raw RGBA8 decoder, no third-party dependencies.
//
// Needed because the uGUI graphics path has no pixels yet: Sprite/Texture nodes are identity-only,
// so every Image/RawImage draws as a flat tint. The established pattern in this project is to bake
// pixels into raw RGBA8 blobs with a Node tool (that is how data/font-*.atlas was produced) rather
// than teach the C++ engine to decode anything -- the iOS build must stay dependency-free.
//
// Supports what Unity's source textures here actually use: 8- and 16-bit samples, colour types
// 0 (grey), 2 (RGB), 3 (palette + tRNS), 4 (grey+alpha) and 6 (RGBA), all five scanline filters.
// Interlaced (Adam7) PNGs are rejected explicitly rather than decoded wrong.
//
//   const { decodePng } = require('./png-decode');
//   const img = decodePng(fs.readFileSync(file));   // { width, height, rgba: Buffer }
const zlib = require('zlib');

function decodePng(buf) {
  const SIG = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
  for (let i = 0; i < 8; i++) {
    if (buf[i] !== SIG[i]) throw new Error('not a PNG (bad signature)');
  }
  let p = 8;
  let width = 0, height = 0, depth = 8, colorType = 6, interlace = 0;
  let palette = null, trns = null;
  const idat = [];
  while (p + 8 <= buf.length) {
    const len = buf.readUInt32BE(p);
    const type = buf.toString('ascii', p + 4, p + 8);
    const body = buf.slice(p + 8, p + 8 + len);
    p += 12 + len;                                  // length + type + data + crc
    if (type === 'IHDR') {
      width = body.readUInt32BE(0);
      height = body.readUInt32BE(4);
      depth = body[8];
      colorType = body[9];
      interlace = body[12];
    } else if (type === 'PLTE') palette = body;
    else if (type === 'tRNS') trns = body;
    else if (type === 'IDAT') idat.push(body);
    else if (type === 'IEND') break;
  }
  if (!width || !height) throw new Error('PNG has no IHDR');
  if (interlace) throw new Error('interlaced (Adam7) PNG not supported');
  if (depth !== 8 && depth !== 16 && !(colorType === 3 && (depth === 1 || depth === 2 || depth === 4))) {
    throw new Error(`unsupported bit depth ${depth} for colour type ${colorType}`);
  }

  const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };
  const ch = CHANNELS[colorType];
  if (ch === undefined) throw new Error(`unsupported colour type ${colorType}`);

  const raw = zlib.inflateSync(Buffer.concat(idat));
  const bitsPerPixel = ch * depth;
  const bpp = Math.max(1, bitsPerPixel >> 3);        // filter unit: bytes per pixel, min 1
  const rowBytes = Math.ceil((width * bitsPerPixel) / 8);

  // ---- un-filter (PNG spec 9.2): each scanline carries a filter byte ----
  const lines = Buffer.alloc(rowBytes * height);
  let sp = 0;
  for (let y = 0; y < height; y++) {
    const ft = raw[sp++];
    const cur = lines.slice(y * rowBytes, (y + 1) * rowBytes);
    raw.copy(cur, 0, sp, sp + rowBytes);
    sp += rowBytes;
    const prev = y > 0 ? lines.slice((y - 1) * rowBytes, y * rowBytes) : null;
    for (let x = 0; x < rowBytes; x++) {
      const a = x >= bpp ? cur[x - bpp] : 0;         // left
      const b = prev ? prev[x] : 0;                  // up
      const c = (prev && x >= bpp) ? prev[x - bpp] : 0; // up-left
      switch (ft) {
        case 0: break;                               // None
        case 1: cur[x] = (cur[x] + a) & 0xff; break; // Sub
        case 2: cur[x] = (cur[x] + b) & 0xff; break; // Up
        case 3: cur[x] = (cur[x] + ((a + b) >> 1)) & 0xff; break; // Average
        case 4: {                                    // Paeth
          const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
          const pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          cur[x] = (cur[x] + pr) & 0xff;
          break;
        }
        default: throw new Error(`bad filter type ${ft} on row ${y}`);
      }
    }
  }

  // ---- expand to RGBA8 ----
  const rgba = Buffer.alloc(width * height * 4);
  const step = depth === 16 ? 2 : 1;                 // 16-bit: take the high byte
  for (let y = 0; y < height; y++) {
    const row = y * rowBytes;
    for (let x = 0; x < width; x++) {
      const o = (y * width + x) * 4;
      if (colorType === 3) {
        // indexed: sub-byte depths pack several pixels per byte, MSB first
        let idx;
        if (depth === 8) idx = lines[row + x];
        else {
          const per = 8 / depth;
          const byte = lines[row + ((x / per) | 0)];
          const shift = 8 - depth * ((x % per) + 1);
          idx = (byte >> shift) & ((1 << depth) - 1);
        }
        rgba[o] = palette[idx * 3];
        rgba[o + 1] = palette[idx * 3 + 1];
        rgba[o + 2] = palette[idx * 3 + 2];
        rgba[o + 3] = (trns && idx < trns.length) ? trns[idx] : 255;
        continue;
      }
      const s = row + x * ch * step;
      if (colorType === 0) {                          // grey
        const v = lines[s];
        rgba[o] = rgba[o + 1] = rgba[o + 2] = v; rgba[o + 3] = 255;
      } else if (colorType === 4) {                   // grey + alpha
        const v = lines[s];
        rgba[o] = rgba[o + 1] = rgba[o + 2] = v; rgba[o + 3] = lines[s + step];
      } else if (colorType === 2) {                   // RGB
        rgba[o] = lines[s]; rgba[o + 1] = lines[s + step]; rgba[o + 2] = lines[s + 2 * step];
        rgba[o + 3] = 255;
      } else {                                        // RGBA
        rgba[o] = lines[s]; rgba[o + 1] = lines[s + step]; rgba[o + 2] = lines[s + 2 * step];
        rgba[o + 3] = lines[s + 3 * step];
      }
    }
  }
  return { width, height, rgba, colorType, depth };
}

module.exports = { decodePng };
