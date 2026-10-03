import { sha256Hex } from '../js/core.js';

// Builds a structurally valid ESP32-S3 app image: header, segments, XOR checksum, appended SHA-256.
export async function makeImage({ chipId = 9, segments = [[0x3c000020, 100], [0x40080000, 37]], hash = true } = {}) {
  const parts = [];
  const head = new Uint8Array(24);
  head[0] = 0xe9; head[1] = segments.length;
  new DataView(head.buffer).setUint32(4, 0x40080100, true);
  new DataView(head.buffer).setUint16(12, chipId, true);
  head[23] = hash ? 1 : 0;
  parts.push(head);
  let sum = 0xef;
  let len = 24;
  segments.forEach(([addr, size], i) => {
    const seg = new Uint8Array(8 + size);
    new DataView(seg.buffer).setUint32(0, addr, true);
    new DataView(seg.buffer).setUint32(4, size, true);
    for (let j = 0; j < size; j++) { seg[8 + j] = (i * 31 + j * 7 + 1) & 0xff; sum ^= seg[8 + j]; }
    parts.push(seg); len += seg.length;
  });
  const padLen = (15 - (len % 16)) % 16;
  const pad = new Uint8Array(padLen + 1); pad[padLen] = sum;
  parts.push(pad);
  let body = concat(parts);
  if (hash) body = concat([body, new Uint8Array(await crypto.subtle.digest('SHA-256', body))]);
  return body;
}

export function concat(arrs) {
  const out = new Uint8Array(arrs.reduce((n, a) => n + a.length, 0));
  let o = 0; for (const a of arrs) { out.set(a, o); o += a.length; }
  return out;
}

// A fake Diptyx: 16 MB of flash in memory, with a partition table of our choosing.
export function fakeDriver({ chip = 'ESP32-S3 (QFN56) (revision v0.2)', flash = 16 * 1024 * 1024, ptable, failVerify = false, failWrite = false } = {}) {
  const mem = new Uint8Array(16 * 1024 * 1024).fill(0xff);
  if (ptable) mem.set(ptable, 0x8000);
  const calls = [];
  return {
    mem, calls,
    async connect() { calls.push('connect'); },
    async chipName() { return chip; },
    async flashSizeBytes() { return flash; },
    async read(addr, len, onProgress) { calls.push(['read', addr, len]); onProgress && onProgress(1); return mem.slice(addr, addr + len); },
    async write(addr, bytes, onProgress) {
      calls.push(['write', addr, bytes.length]);
      if (failWrite) throw new Error('device disconnected');
      const padded = new Uint8Array(Math.ceil(bytes.length / 4) * 4); padded.set(bytes);
      mem.set(padded, addr);
      if (failVerify) mem[addr + 5] ^= 0xff;
      onProgress && onProgress(1);
    },
    async md5(addr, len) { const { md5Hex } = await import('../js/core.js'); return md5Hex(mem.subarray(addr, addr + len)); },
    async reset() { calls.push('reset'); },
    async disconnect() { calls.push('disconnect'); },
  };
}

export async function stockPtable() {
  const pt = new Uint8Array(0xc00); for (let i = 0; i < pt.length; i++) pt[i] = (i * 13 + 5) & 0xff;
  return { bytes: pt, sha: await sha256Hex(pt) };
}
