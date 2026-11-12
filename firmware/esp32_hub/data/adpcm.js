'use strict';
// IMA ADPCM WAV decoder for the student page. Browsers' decodeAudioData does not support
// WAVE_FORMAT_IMA_ADPCM (0x11), which the cloud uses to keep each language stream at ~8 KB/s.
// Same algorithm as supabase/functions/_shared/audio.ts and the Hub firmware.
(function (root) {
  var STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767];
  var INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

  function str(b, o) { return String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]); }
  function u16(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }

  // Returns {format, sampleRate, samples: Float32Array} for an IMA ADPCM WAV, or null for any
  // other file (the caller then uses the browser's own decoder).
  function decode(arrayBuffer) {
    var b = new Uint8Array(arrayBuffer);
    if (b.length < 12 || str(b, 0) !== 'RIFF' || str(b, 8) !== 'WAVE') return null;
    var fmt = null, total = null, data = null, o = 12;
    while (o + 8 <= b.length) {
      var id = str(b, o), size = u32(b, o + 4), body = o + 8;
      if (id === 'fmt ') {
        fmt = { format: u16(b, body), channels: u16(b, body + 2), rate: u32(b, body + 4),
          align: u16(b, body + 12), bits: u16(b, body + 14),
          spb: size >= 20 && u16(b, body + 16) >= 2 ? u16(b, body + 18) : 0 };
      } else if (id === 'fact' && size >= 4) {
        total = u32(b, body);
      } else if (id === 'data') {
        data = b.subarray(body, Math.min(b.length, body + size));
        break;
      }
      o = body + size + (size & 1);
    }
    if (!fmt || !data || fmt.format !== 0x11 || fmt.channels !== 1 || fmt.bits !== 4 || fmt.align < 5) return null;
    var align = fmt.align, spb = fmt.spb || (align - 4) * 2 + 1;
    var blocks = Math.ceil(data.length / align), cap = 0, blk, len;
    for (blk = 0; blk < blocks; blk++) {
      len = Math.min(align, data.length - blk * align);
      if (len >= 4) cap += Math.min(spb, 1 + (len - 4) * 2);
    }
    var n = total === null ? cap : Math.min(total, cap);
    var out = new Float32Array(n), k = 0;
    for (blk = 0; blk < blocks && k < n; blk++) {
      var base = blk * align;
      len = Math.min(align, data.length - base);
      if (len < 4) break;
      var pred = (data[base] | (data[base + 1] << 8)) << 16 >> 16;
      var index = Math.min(88, data[base + 2]);
      out[k++] = pred / 32768;
      var inBlock = Math.min(spb, 1 + (len - 4) * 2);
      for (var i = 1; i < inBlock && k < n; i++) {
        var byte = data[base + 4 + ((i - 1) >> 1)];
        var nib = (i - 1) & 1 ? byte >> 4 : byte & 15;
        var step = STEP[index], delta = step >> 3;
        if (nib & 4) delta += step;
        if (nib & 2) delta += step >> 1;
        if (nib & 1) delta += step >> 2;
        pred += nib & 8 ? -delta : delta;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        index += INDEX[nib];
        if (index < 0) index = 0; else if (index > 88) index = 88;
        out[k++] = pred / 32768;
      }
    }
    return { format: 0x11, sampleRate: fmt.rate, samples: out };
  }

  root.ElingoAdpcm = { decode: decode };
})(typeof window !== 'undefined' ? window : globalThis);
