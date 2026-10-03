import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync, existsSync } from 'node:fs';
import { md5Hex, sha256Hex, validateAppImage, parseManifest, explainError, FlasherError, STOCK_PTABLE_SHA256, APP_SLOT_SIZE } from '../js/core.js';
import { makeImage } from './helpers.mjs';

test('md5Hex matches node:crypto for many lengths', () => {
  for (const n of [0, 1, 3, 55, 56, 63, 64, 65, 119, 120, 1000, 4096, 100003]) {
    const b = new Uint8Array(n); for (let i = 0; i < n; i++) b[i] = (i * 37 + 11) & 0xff;
    assert.equal(md5Hex(b), createHash('md5').update(b).digest('hex'), `length ${n}`);
  }
});

test('sha256Hex matches node:crypto', async () => {
  const b = new Uint8Array(5000).map((_, i) => i & 0xff);
  assert.equal(await sha256Hex(b), createHash('sha256').update(b).digest('hex'));
});

test('a well-formed image (with and without appended digest) validates', async () => {
  for (const hash of [true, false]) {
    const info = await validateAppImage(await makeImage({ hash }));
    assert.equal(info.segments, 2);
  }
});

test('broken images are refused with a clear reason', async () => {
  const good = await makeImage();
  const reject = async (bytes, re) => await assert.rejects(validateAppImage(bytes), (e) => e instanceof FlasherError && e.code === 'bad-image' && re.test(e.message));
  await reject(new Uint8Array(10), /too small/);
  const noMagic = good.slice(); noMagic[0] = 0x00; await reject(noMagic, /magic/);
  await reject(await makeImage({ chipId: 5 }), /another chip/);
  await reject(good.slice(0, good.length - 40), /cut short|digest|checksum/);
  const flip = good.slice(); flip[40] ^= 1; await reject(flip, /checksum|digest/);
  const tooBig = new Uint8Array(APP_SLOT_SIZE + 1); tooBig[0] = 0xe9; await reject(tooBig, /larger than/);
  const noDigest = good.slice(); noDigest[noDigest.length - 5] ^= 1; await reject(noDigest, /digest/);
});

test('parseManifest', () => {
  const sha = 'a'.repeat(64);
  assert.deepEqual(parseManifest({ version: '1', firmware: { path: 'firmware/x.bin', size: 5, sha256: sha } }).firmware, { path: 'firmware/x.bin', size: 5, sha256: sha });
  assert.equal(parseManifest({ version: 'dev' }).firmware, null);
  assert.throws(() => parseManifest({ firmware: { path: 'https://evil/x.bin', size: 5, sha256: sha } }), /not allowed/);
  assert.throws(() => parseManifest({ firmware: { path: '../x.bin', size: 5, sha256: sha } }), /not allowed/);
  assert.throws(() => parseManifest(null));
});

test('explainError gives friendly text for the common failures', () => {
  assert.match(explainError(Object.assign(new Error('x'), { name: 'NotFoundError' })).title, /No device/);
  assert.match(explainError(new Error('Failed to open serial port.')).title, /busy/);
  assert.match(explainError(new Error('Failed to connect with the device')).hint, /download mode/);
  assert.match(explainError(new Error('The device has been lost')).title, /disconnected/);
  assert.match(explainError(new Error('Failed to fetch')).title, /downloaded/);
  assert.ok(explainError(new Error('weird')).hint);
});

test('the stock partition-table hash constant matches flash.py', () => {
  const py = readFileSync(new URL('../../flash.py', import.meta.url), 'utf8');
  assert.ok(py.includes(STOCK_PTABLE_SHA256));
});

test('real firmware validates when FIRMWARE_BIN is set', { skip: !process.env.FIRMWARE_BIN || !existsSync(process.env.FIRMWARE_BIN) }, async () => {
  const info = await validateAppImage(new Uint8Array(readFileSync(process.env.FIRMWARE_BIN)));
  assert.ok(info.size > 1_000_000);
});
