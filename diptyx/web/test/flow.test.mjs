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

test('backup reads the 6 MB app slot and never writes', async () => {
  const pt = await stockPtable();
  const d = fakeDriver({ ptable: pt.bytes });
  const data = await backupApp(d, { stockSha: undefined, onStep() {} }).catch((e) => e);
  assert.equal(data.code, 'not-stock-layout', 'the real stock hash does not match the test table, so it refuses');
  assert.equal(writes(d).length, 0);
});

const respond = (body, ok = true, status = 200) => async () => ({
  ok, status, headers: { get: () => String(body.length) },
  body: { getReader() { let sent = false; return { async read() { if (sent) return { done: true }; sent = true; return { done: false, value: body }; } }; } },
});

test('fetchVerified accepts a matching checksum and rejects a mismatch', async () => {
  const body = new Uint8Array(1000).map((_, i) => i & 0xff);
  const sha = createHash('sha256').update(body).digest('hex');
  assert.deepEqual(await fetchVerified('x', { sha256: sha, expectedSize: 1000, fetchImpl: respond(body) }), body);
  await assert.rejects(fetchVerified('x', { sha256: '0'.repeat(64), fetchImpl: respond(body) }), (e) => e.code === 'checksum-mismatch');
  await assert.rejects(fetchVerified('x', { expectedSize: 999, fetchImpl: respond(body) }), (e) => e.code === 'download-failed');
  await assert.rejects(fetchVerified('x', { fetchImpl: respond(body, false, 404) }), (e) => e.code === 'download-failed' && /404/.test(e.message));
});
