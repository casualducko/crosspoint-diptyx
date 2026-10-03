// The install / restore / backup sequences. They talk to the device only through a `driver` object (see esp-driver.js),
// so the same code runs against the real chip in the browser and against a fake one in the tests.
//
// driver: { connect(), chipName(), flashSizeBytes(), read(addr, len, onProgress), write(addr, bytes, onProgress),
//           md5(addr, len), reset(), disconnect() }
import {
  APP_OFFSET, APP_SLOT_SIZE, FLASH_SIZE, PTABLE_OFFSET, PTABLE_LEN, STOCK_APP,
  FlasherError, checkPartitionTable, md5Hex, sha256Hex, validateAppImage,
} from './core.js';

const noop = () => {};

// Read-only checks: ESP32-S3, 16 MB flash, stock Diptyx partition table. Throws FlasherError when something is off.
export async function checkDevice(driver, { log = noop, stockSha } = {}) {
  const chip = await driver.chipName();
  if (!/ESP32-S3/i.test(chip)) {
    throw new FlasherError('wrong-chip', `This is not a Diptyx: the chip is ${chip}, not an ESP32-S3.`, 'Only the Diptyx e-reader (ESP32-S3) is supported.');
  }
  const flash = await driver.flashSizeBytes();
  if (flash !== FLASH_SIZE) {
    throw new FlasherError('wrong-flash', `Expected 16 MB of flash, found ${flash ? Math.round(flash / 1048576) + ' MB' : 'an unknown size'}.`, 'Only the 16 MB Diptyx is supported.');
  }
  const pt = await driver.read(PTABLE_OFFSET, PTABLE_LEN, noop);
  const res = await checkPartitionTable(pt, stockSha);
  log(`Partition table SHA-256 ${res.sha.slice(0, 16)}...`);
  if (!res.ok) {
    throw new FlasherError(
      'not-stock-layout',
      "This device's partition table is not the stock Diptyx layout, so the app slot may not be at 0x10000.",
      'Nothing was changed. If you installed another firmware or changed the layout, use a full-flash tool of your own instead.',
    );
  }
  return { chip, flash, partitionSha: res.sha };
}

// Writes `bytes` to the app slot and checks it landed (MD5 of the flash range vs. the image).
// onStep(id, label) announces phases; onProgress(fraction) reports the write/read progress of the current phase.
export async function installApp(driver, bytes, { onStep = noop, onProgress = noop, log = noop, stockSha } = {}) {
  onStep('check', 'Checking your Diptyx');
  const info = await checkDevice(driver, { log, stockSha });
  onStep('verify-file', 'Checking the firmware file');
  const img = await validateAppImage(bytes);
  log(`Image OK: ${img.size.toLocaleString()} bytes, ${img.segments} segments`);
  // The loader pads the image to a multiple of 4 with 0xFF before writing; compare what is on the flash, so do the same.
  const padded = new Uint8Array(Math.ceil(bytes.length / 4) * 4).fill(0xff);
  padded.set(bytes);
  onStep('write', 'Writing the firmware (do not unplug)');
  await driver.write(APP_OFFSET, bytes, onProgress);
  onStep('verify-write', 'Verifying what was written');
  const want = md5Hex(padded);
  const got = String(await driver.md5(APP_OFFSET, padded.length)).toLowerCase();
  log(`Flash MD5 ${got}, expected ${want}`);
  if (got !== want) {
    throw new FlasherError('verify-failed', 'The written data does not match the firmware file.', 'The write did not complete cleanly. Try again; if it keeps failing, try another USB cable or port, or restore the stock firmware.');
  }
  onStep('reset', 'Restarting the Diptyx');
  await driver.reset();
  return { ...info, size: bytes.length };
}

// Downloads `url` with progress and returns the bytes. If `sha256` is given, the download must match it.
export async function fetchVerified(url, { sha256, expectedSize, onProgress = noop, fetchImpl = globalThis.fetch, signal, maxBytes = APP_SLOT_SIZE, timeoutMs = 180000 } = {}) {
  // A stalled connection must not leave the page busy forever: abort after timeoutMs (and if the caller aborts).
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  if (signal) signal.addEventListener('abort', () => ctrl.abort(), { once: true });
  try {
    return await fetchBytes(url, { sha256, expectedSize, onProgress, fetchImpl, signal: ctrl.signal, maxBytes });
  } catch (e) {
    if (ctrl.signal.aborted && !(e instanceof FlasherError)) throw new FlasherError('download-failed', 'The download took too long and was stopped.', 'Check your internet connection and try again.');
    throw e;
  } finally {
    clearTimeout(timer);
  }
}

async function fetchBytes(url, { sha256, expectedSize, onProgress, fetchImpl, signal, maxBytes }) {
  const res = await fetchImpl(url, { cache: 'no-store', signal });
  if (!res.ok) throw new FlasherError('download-failed', `The firmware could not be downloaded (HTTP ${res.status}).`, 'Check your internet connection and try again.');
  const total = Number(res.headers.get('content-length')) || expectedSize || 0;
  if (total > maxBytes) throw new FlasherError('download-failed', 'The file is larger than the 6 MB app slot, so it was not used.', 'Try again, or use the flash script.');
  let bytes;
  if (res.body && res.body.getReader) {
    const reader = res.body.getReader();
    const parts = [];
    let got = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
      got += value.length;
      if (got > maxBytes) throw new FlasherError('download-failed', 'The file is larger than the 6 MB app slot, so it was not used.', 'Try again, or use the flash script.');
      if (total) onProgress(Math.min(1, got / total));
    }
    bytes = new Uint8Array(got);
    let o = 0;
    for (const p of parts) { bytes.set(p, o); o += p.length; }
  } else {
    bytes = new Uint8Array(await res.arrayBuffer());
  }
  if (expectedSize && bytes.length !== expectedSize) {
    throw new FlasherError('download-failed', `The downloaded file has the wrong size (${bytes.length} instead of ${expectedSize} bytes).`, 'Try again.');
  }
  if (sha256) {
    const got = await sha256Hex(bytes);
    if (got !== sha256) throw new FlasherError('checksum-mismatch', 'The downloaded firmware does not match its checksum, so it was not used.', 'Try again. If it still fails, the download source has changed; use the flash script instead.');
  }
  return bytes;
}

export async function installFromUrl(driver, { url, sha256, size }, hooks = {}) {
  hooks.onStep && hooks.onStep('download', 'Downloading the firmware');
  const bytes = await fetchVerified(url, { sha256, expectedSize: size, onProgress: hooks.onProgress, fetchImpl: hooks.fetchImpl });
  return installApp(driver, bytes, hooks);
}

export function restoreStock(driver, hooks = {}) {
  return installFromUrl(driver, { url: STOCK_APP.url, sha256: STOCK_APP.sha256 }, hooks);
}

// Reads the whole app slot (6 MB) so the user can keep a copy of what they had. Read-only.
export async function backupApp(driver, { onStep = noop, onProgress = noop, log = noop, stockSha } = {}) {
  onStep('check', 'Checking your Diptyx');
  await checkDevice(driver, { log, stockSha });
  onStep('read', 'Reading your current firmware (about 3 minutes)');
  const data = await driver.read(APP_OFFSET, APP_SLOT_SIZE, onProgress);
  // The copy is only worth keeping if it is what the chip holds: compare with the loader's MD5 of the same range.
  const got = String(await driver.md5(APP_OFFSET, APP_SLOT_SIZE)).toLowerCase();
  if (got !== md5Hex(data)) {
    throw new FlasherError('verify-failed', 'The copy that was read does not match the flash, so it was not saved.', 'Try again with another USB cable or port.');
  }
  return data;
}
