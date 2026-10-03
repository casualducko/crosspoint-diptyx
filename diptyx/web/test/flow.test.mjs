import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { checkDevice, installApp, backupApp, fetchVerified } from '../js/flow.js';
import { FlasherError, md5Hex } from '../js/core.js';
import { makeImage, fakeDriver, stockPtable } from './helpers.mjs';

const writes = (d) => d.calls.filter((c) => Array.isArray(c) && c[0] === 'write');

test('a good install writes only the app slot, verifies, then resets', async () => {
  const pt = await stockPtable();
  const d = fakeDriver({ ptable: pt.bytes });
  const img = await makeImage();
  const steps = [];
  const res = await installApp(d, img, { stockSha: pt.sha, onStep: (id) => steps.push(id) });
  assert.deepEqual(steps, ['check', 'verify-file', 'write', 'verify-write', 'reset']);
  assert.equal(writes(d).length, 1);
  assert.equal(writes(d)[0][1], 0x10000);
  assert.deepEqual(d.mem.slice(0x8000, 0x8000 + 0xc00), pt.bytes, 'partition table untouched');
  assert.ok(d.mem.slice(0, 0x8000).every((b) => b === 0xff), 'bootloader area untouched');
  assert.equal(md5Hex(d.mem.subarray(0x10000, 0x10000 + img.length)), createHash('md5').update(img).digest('hex'));
  assert.equal(res.size, img.length);
  assert.ok(d.calls.includes('reset'));
});

test('refuses a non-stock partition table and writes nothing', async () => {
  const pt = await stockPtable();
  const other = pt.bytes.slice(); other[3] ^= 1;
  const d = fakeDriver({ ptable: other });
  await assert.rejects(installApp(d, await makeImage(), { stockSha: pt.sha }), (e) => e.code === 'not-stock-layout');
  assert.equal(writes(d).length, 0);
});

test('refuses the wrong chip and the wrong flash size', async () => {
  const pt = await stockPtable();
  await assert.rejects(checkDevice(fakeDriver({ chip: 'ESP32-C3', ptable: pt.bytes }), { stockSha: pt.sha }), (e) => e.code === 'wrong-chip');
  await assert.rejects(checkDevice(fakeDriver({ flash: 8 * 1024 * 1024, ptable: pt.bytes }), { stockSha: pt.sha }), (e) => e.code === 'wrong-flash');
});

test('a damaged image is refused before anything is written', async () => {
  const pt = await stockPtable();
  const d = fakeDriver({ ptable: pt.bytes });
  const img = await makeImage(); img[60] ^= 1;
  await assert.rejects(installApp(d, img, { stockSha: pt.sha }), (e) => e.code === 'bad-image');
  assert.equal(writes(d).length, 0);
});

test('a write that does not verify is reported and the device is not reset', async () => {
  const pt = await stockPtable();
  const d = fakeDriver({ ptable: pt.bytes, failVerify: true });
  await assert.rejects(installApp(d, await makeImage(), { stockSha: pt.sha }), (e) => e.code === 'verify-failed');
  assert.ok(!d.calls.includes('reset'));
});

test('a write error surfaces unchanged', async () => {
  const pt = await stockPtable();
  const d = fakeDriver({ ptable: pt.bytes, failWrite: true });
  await assert.rejects(installApp(d, await makeImage(), { stockSha: pt.sha }), /disconnected/);
});

test('backup refuses a non-stock table, reads the app slot, verifies it, and never writes', async () => {
  const pt = await stockPtable();
  const refused = fakeDriver({ ptable: pt.bytes });
  const err = await backupApp(refused, {}).catch((e) => e);
  assert.equal(err.code, 'not-stock-layout', 'the real stock hash does not match the test table, so it refuses');
  const d = fakeDriver({ ptable: pt.bytes });
  const img = await makeImage();
  await installApp(d, img, { stockSha: pt.sha });
  const data = await backupApp(d, { stockSha: pt.sha });
  assert.equal(data.length, 0x600000);
  assert.deepEqual(data.slice(0, img.length), img);
  assert.equal(writes(d).length, 1, 'only the earlier install wrote');
  // a read that disagrees with the chip's own MD5 is not handed back as a backup
  const flaky = fakeDriver({ ptable: pt.bytes });
  const realRead = flaky.read.bind(flaky);
  flaky.read = async (a, l, p) => { const r = await realRead(a, l, p); if (a === 0x10000) r[7] ^= 1; return r; };
  await assert.rejects(backupApp(flaky, { stockSha: pt.sha }), (e) => e.code === 'verify-failed');
});

test('an image whose length is not a multiple of 4 verifies (0xFF padding like esptool-js)', async () => {
  const pt = await stockPtable();
  const d = fakeDriver({ ptable: pt.bytes });
  const base = await makeImage({ hash: false });
  const odd = new Uint8Array(base.length + 3).fill(0xff); odd.set(base); // erased-flash tail, 3 bytes over a 4 multiple
  assert.notEqual(odd.length % 4, 0);
  await installApp(d, odd, { stockSha: pt.sha });
  assert.ok(d.calls.includes('reset'));
});

const respond = (body, ok = true, status = 200) => async () => ({
  ok, status, headers: { get: () => String(body.length) },
  body: { getReader() { let sent = false; return { async read() { if (sent) return { done: true }; sent = true; return { done: false, value: body }; } }; } },
});

test('fetchVerified refuses oversize files and gives up on a stalled download', async () => {
  const big = new Uint8Array(1000);
  await assert.rejects(fetchVerified('x', { maxBytes: 500, fetchImpl: respond(big) }), (e) => e.code === 'download-failed' && /larger/.test(e.message));
  const stalled = (_url, { signal }) => new Promise((_res, rej) => signal.addEventListener('abort', () => rej(Object.assign(new Error('aborted'), { name: 'AbortError' }))));
  await assert.rejects(fetchVerified('x', { fetchImpl: stalled, timeoutMs: 40 }), (e) => e.code === 'download-failed' && /too long/.test(e.message));
});

test('fetchVerified accepts a matching checksum and rejects a mismatch', async () => {
  const body = new Uint8Array(1000).map((_, i) => i & 0xff);
  const sha = createHash('sha256').update(body).digest('hex');
  assert.deepEqual(await fetchVerified('x', { sha256: sha, expectedSize: 1000, fetchImpl: respond(body) }), body);
  await assert.rejects(fetchVerified('x', { sha256: '0'.repeat(64), fetchImpl: respond(body) }), (e) => e.code === 'checksum-mismatch');
  await assert.rejects(fetchVerified('x', { expectedSize: 999, fetchImpl: respond(body) }), (e) => e.code === 'download-failed');
  await assert.rejects(fetchVerified('x', { fetchImpl: respond(body, false, 404) }), (e) => e.code === 'download-failed' && /404/.test(e.message));
});
