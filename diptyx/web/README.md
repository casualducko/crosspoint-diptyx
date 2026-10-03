# Diptyx web flasher

A static page (no build step) that installs the app image over WebSerial. Layout:

| File | Role |
|---|---|
| `index.html`, `style.css` | the page |
| `js/ui.js` | buttons, progress, results |
| `js/flow.js` | the install / restore / backup sequences (talk to the device only through a driver object) |
| `js/esp-driver.js` | the real driver, a thin adapter over `esptool-js` |
| `js/core.js` | pure helpers: constants, partition-table and image checks, MD5/SHA-256, error explanations |
| `vendor/esptool-js.bundle.js` | esptool-js 0.7.0 (Apache-2.0), unmodified |
| `manifest.json` | version and the bundled firmware (path, size, SHA-256); the release workflow fills it in |

Safety rules (keep them): write only at 0x10000, refuse anything but ESP32-S3 + 16 MB + the stock partition table
(`STOCK_PTABLE_SHA256`, also in `flash.py`), validate and checksum the image before writing, verify with the loader's MD5 after,
never use 921600 baud on this device.

## Tests

```
node --test diptyx/web/test/*.test.mjs            # Node 20+
FIRMWARE_BIN=path/to/firmware.bin node --test ...  # also validates a real image
```

The flow tests run against a fake in-memory Diptyx (`test/helpers.mjs`), so they cover the refusal cases without hardware.
To try the page itself without a device, serve the folder (`python3 -m http.server`) and set `window.__driverFactory` to a function
returning a fake driver before clicking *Connect*; it is a test seam only.

## Updating esptool-js

`npm pack esptool-js@<version>`, copy `bundle.js` to `vendor/esptool-js.bundle.js` and `LICENSE` to `vendor/esptool-js.LICENSE.txt`,
update the version here and in `vendor/README.txt` and `NOTICE`, run the tests, and test a real flash.
