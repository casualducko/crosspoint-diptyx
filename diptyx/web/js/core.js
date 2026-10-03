// Pure helpers for the Diptyx web flasher: constants, checks and checksums. No DOM, no device access, so they run in
// the browser and in Node tests alike.

export const APP_OFFSET = 0x10000; // factory app slot of the stock Diptyx partition table
export const APP_SLOT_SIZE = 0x600000; // 6 MB
export const FLASH_SIZE = 16 * 1024 * 1024;
export const PTABLE_OFFSET = 0x8000;
export const PTABLE_LEN = 0xc00;
// SHA-256 of the 3 KB partition table at 0x8000 in the stock Diptyx firmware (1.0.1, 1.0.2 and the factory image).
export const STOCK_PTABLE_SHA256 = '7a1389f74052c466a758e2d93babc42a1a24738b81748cc3f46ce6606843330d';
export const ESP32_S3_CHIP_ID = 9;
export const APP_DESC_MAGIC = 0xabcd5432; // esp_app_desc_t magic at offset 32 of every app image (bootloaders and merged images lack it)

// The makers' stock app image, used by "Go back to the stock firmware". Pinned by SHA-256, so a changed file is refused.
export const STOCK_APP = {
  version: '1.0.2',
  url: 'https://raw.githubusercontent.com/MartijndenHoed/Diptyx/e7bbafd63a4a0da1c894ab2282051ee9dd4285b9/firmware_release/diptyx_firmware_1.0.2_patch.bin', // pinned to a commit
  sha256: '211749666378da5121fb21ecea99686af8a034ef138a82eb6cd7bde5e6386854',
};

export class FlasherError extends Error {
  constructor(code, message, hint = '') {
    super(message);
    this.name = 'FlasherError';
    this.code = code;
    this.hint = hint;
  }
}

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');

export async function sha256Hex(bytes) {
  return hex(await globalThis.crypto.subtle.digest('SHA-256', bytes));
}

// MD5 (RFC 1321). The ESP bootloader can hash a flash range with MD5, so we compare against that after writing.
export function md5Hex(bytes) {
  const K = new Uint32Array(64);
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0;
  const S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const len = bytes.length;
  const padded = new Uint8Array(((len + 8) >> 6 << 6) + 64);
  padded.set(bytes);
  padded[len] = 0x80;
  const dv = new DataView(padded.buffer);
  dv.setUint32(padded.length - 8, (len << 3) >>> 0, true);
  dv.setUint32(padded.length - 4, Math.floor(len / 536870912), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) M[i] = dv.getUint32(off + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
      else { F = C ^ (B | ~D); g = (7 * i) & 15; }
      F = (F + A + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      const s = S[((i >> 4) << 2) | (i & 3)];
      B = (B + ((F << s) | (F >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const out = new Uint8Array(16);
  const odv = new DataView(out.buffer);
  [a0, b0, c0, d0].forEach((v, i) => odv.setUint32(i * 4, v, true));
  return hex(out);
}

// Structural check of an ESP32-S3 app image: header, segments, XOR checksum and the appended SHA-256 digest.
// Returns { size, segments, entry }; throws FlasherError('bad-image') when something is off.
export async function validateAppImage(bytes) {
  const bad = (m) => new FlasherError('bad-image', m, 'Use a firmware file made for the Diptyx (an "app" .bin, not a full-flash image).');
  if (!(bytes instanceof Uint8Array) || bytes.length < 32) throw bad('That file is too small to be a firmware image.');
  if (bytes.length > APP_SLOT_SIZE) throw bad(`The image is ${bytes.length.toLocaleString()} bytes, larger than the 6 MB app slot.`);
  if (bytes[0] !== 0xe9) throw bad('This is not an ESP firmware image (it does not start with the 0xE9 magic byte).');
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const segCount = bytes[1];
  const chipId = dv.getUint16(12, true);
  if (chipId !== ESP32_S3_CHIP_ID) throw bad(`This image is for another chip (id ${chipId}), not an ESP32-S3.`);
  const hashAppended = bytes[23] === 1;
  let pos = 24;
  let sum = 0xef;
  for (let i = 0; i < segCount; i++) {
    if (pos + 8 > bytes.length) throw bad('The image is cut short (a segment header is missing).');
    const size = dv.getUint32(pos + 4, true);
    pos += 8;
    if (pos + size > bytes.length) throw bad('The image is cut short (a segment runs past the end of the file).');
    for (let j = 0; j < size; j++) sum ^= bytes[pos + j];
    pos += size;
  }
  if (bytes.length < 36 || dv.getUint32(32, true) !== APP_DESC_MAGIC) {
    throw bad('This is not an app image (it has no application descriptor). A bootloader or a merged full-flash image would break the device if written to the app slot.');
  }
  const checksumPos = pos + ((15 - (pos % 16)) % 16);
  if (checksumPos >= bytes.length) throw bad('The image is cut short (the checksum is missing).');
  if (bytes[checksumPos] !== sum) throw bad('The image checksum does not match; the file is damaged.');
  if (hashAppended) {
    const end = checksumPos + 1;
    if (end + 32 > bytes.length) throw bad('The image is cut short (the digest is missing).');
    const want = hex(bytes.subarray(end, end + 32));
    if ((await sha256Hex(bytes.subarray(0, end))) !== want) throw bad('The image digest does not match; the file is damaged.');
  }
  // Nothing but erased flash may follow the image (a merged image carries more data behind the first app).
  const end = checksumPos + 1 + (hashAppended ? 32 : 0);
  for (let i = end; i < bytes.length; i++) {
    if (bytes[i] !== 0xff) throw bad('There is extra data after the end of the app image (is this a merged full-flash image?).');
  }
  return { size: bytes.length, segments: segCount, entry: dv.getUint32(4, true) };
}

export async function checkPartitionTable(ptableBytes, expectedSha = STOCK_PTABLE_SHA256) {
  const sha = await sha256Hex(ptableBytes);
  return { ok: sha === expectedSha, sha };
}

// Validates and normalises the release manifest (manifest.json).
export function parseManifest(m) {
  if (!m || typeof m !== 'object') throw new FlasherError('bad-manifest', 'The firmware list could not be read.');
  const fw = m.firmware;
  if (!fw || typeof fw.path !== 'string' || !/^[0-9a-f]{64}$/.test(fw.sha256 || '') || !Number.isInteger(fw.size)) {
    return { version: String(m.version || 'dev'), name: m.name || '', firmware: null };
  }
  if (/^[a-z][a-z0-9+.-]*:|^[\/\\]|[\\\u0000-\u001f]/i.test(fw.path) || fw.path.includes('..')) throw new FlasherError('bad-manifest', 'The firmware path in the list is not allowed.');
  return { version: String(m.version || ''), name: m.name || '', released: m.released || '', firmware: { path: fw.path, size: fw.size, sha256: fw.sha256 } };
}

// Turns a thrown error into a short title and a plain-language hint.
export function explainError(err) {
  if (err instanceof FlasherError) return { title: err.message, hint: err.hint };
  const msg = String((err && err.message) || err || '');
  const name = (err && err.name) || '';
  if (name === 'NotFoundError' || /no port selected/i.test(msg)) return { title: 'No device was chosen.', hint: 'Click the button again and pick the "USB JTAG/serial debug unit".' };
  if (name === 'NetworkError' || /failed to open serial port|port is already open|device is busy|access denied/i.test(msg)) {
    return { title: 'The USB port is busy or could not be opened.', hint: 'Close any serial monitor or other tab using the Diptyx, unplug and replug it in download mode, then try again.' };
  }
  if (/timeout|no serial data|failed to connect|wrong boot mode|invalid head of packet|sync/i.test(msg)) {
    return { title: 'The Diptyx did not answer.', hint: 'It is probably not in download mode. Switch it fully off (hold power ~20 s), hold the center button, plug in the USB-C cable while holding, release after ~3 s, and try again.' };
  }
  if (/disconnect|lost|device has been lost|the device was disconnected/i.test(msg)) return { title: 'The USB cable was disconnected.', hint: 'Plug it back in (download mode) and start again. A cut-off write leaves the app slot incomplete; flashing again, or restoring the stock firmware, fixes it.' };
  if (/Failed to fetch|NetworkError when|Load failed/i.test(msg)) return { title: 'The firmware could not be downloaded.', hint: 'Check your internet connection and try again.' };
  return { title: msg || 'Something went wrong.', hint: 'Unplug the Diptyx, put it back in download mode and try again. If it keeps happening, the flash script (flash.sh / flash.bat) gives more detail.' };
}
