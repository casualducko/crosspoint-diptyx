// Adapter between flow.js and the vendored esptool-js. It is the only file that touches the serial port.
import { ESPLoader, Transport } from '../vendor/esptool-js.bundle.js';

const ESPRESSIF_USB = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }]; // ESP32-S3 built-in USB JTAG/serial unit
const RTC_CNTL_OPTION1_REG = 0x6000812c; // ESP32-S3
const RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK = 0x1;
const BAUD = 460800; // the CLI default; 921600 does not work on this device

export function webSerialSupported() {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

export class EspDriver {
  constructor({ log = () => {} } = {}) {
    this.log = log;
    this.port = null;
    this.transport = null;
    this.loader = null;
  }

  async connect() {
    this.port = await navigator.serial.requestPort({ filters: ESPRESSIF_USB });
    this.transport = new Transport(this.port, false);
    const log = this.log;
    this.loader = new ESPLoader({
      transport: this.transport,
      baudrate: BAUD,
      romBaudrate: 115200,
      terminal: { clean() {}, writeLine: (s) => log(s), write: (s) => log(s) },
    });
    try {
      this.chip = await this.loader.main();
    } catch (e) {
      await this.disconnect();
      throw e;
    }
  }

  async chipName() {
    return this.loader.chip.CHIP_NAME || this.chip;
  }

  async flashSizeBytes() {
    const s = await this.loader.detectFlashSize(); // e.g. "16MB"
    const m = /^(\d+)\s*MB$/i.exec(s || '');
    return m ? Number(m[1]) * 1024 * 1024 : 0;
  }

  read(addr, len, onProgress) {
    return this.loader.readFlash(addr, len, (_pkt, got, total) => onProgress && onProgress(total ? got / total : 0));
  }

  async write(addr, bytes, onProgress) {
    await this.loader.writeFlash({
      fileArray: [{ data: bytes, address: addr }],
      flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep',
      eraseAll: false, compress: true,
      reportProgress: (_i, written, total) => onProgress && onProgress(total ? written / total : 0),
    });
  }

  md5(addr, len) {
    return this.loader.flashMd5sum(addr, len);
  }

  // The ESP32-S3 ROM can leave a "force download boot" flag set after a USB entry into the bootloader; a plain reset then
  // lands in download mode again and the Diptyx never starts. Clear it first, as the esptool command line does (the register
  // is RTC_CNTL_OPTION1_REG, bit 0), then do the normal RTS reset.
  async reset() {
    try {
      await this.loader.writeReg(RTC_CNTL_OPTION1_REG, 0, RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK);
    } catch (e) {
      this.log('Could not clear the forced download boot flag: ' + (e && e.message ? e.message : e));
    }
    await this.loader.after('hard_reset');
  }

  // Releases the serial port. esptool-js's disconnect() waits for its streams to unlock with no timeout, so it is raced
  // against a timer; if the port is still open afterwards it is closed directly so the next connect does not find it busy.
  async disconnect() {
    const transport = this.transport;
    const port = this.port;
    this.transport = null;
    this.loader = null;
    this.port = null;
    if (transport) {
      try {
        await Promise.race([transport.disconnect(), new Promise((resolve) => setTimeout(resolve, 3000))]);
      } catch { /* already gone */ }
    }
    if (port && (port.readable || port.writable)) {
      try { await port.close(); } catch { /* still locked; the user can replug */ }
    }
  }
}
