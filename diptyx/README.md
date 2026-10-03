# CrossPoint for the Diptyx

An **unofficial** port of the [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) e-reader firmware (MIT) to the
[Diptyx](https://diptyx.dev) dual-screen e-reader. It is not affiliated with, or supported by, the Diptyx makers or the CrossPoint
project. Use it at your own risk; you can always go back to the stock firmware (see [Going back to stock](#going-back-to-stock)).

## What you get

- The CrossPoint reader on the **left** screen: library, file browser, settings, EPUB reading, bookmarks, sleep screens, USB drive mode.
- **Two-page spread** in the EPUB reader: left screen = page *N*, right screen = page *N+1*, a page number on each. Turn a page, both
  screens move on by two. Switch it off in *Settings > Reader > Two-Page Spread*.
- The **right screen** shows the cover/title card of your current book on the home screen, and your own
  `idle_screen_right.jpg` (the same file the stock firmware uses) while the device sleeps.
- **Crisp text on a black-and-white panel:** the built-in reading font is **Literata, mono-hinted** (10 to 18 pt, 10 pt by default), and
  the optional [font pack](fonts/README.md) adds more mono-hinted families (Source Serif 4, Spectral, Bitter, ChareInk, Crimson Pro, Inter, Atkinson
  Hyperlegible Next, Noto Sans and a dyslexia-friendly face).
- Black-and-white rendering tuned for the Diptyx panels, standby and real power-off, battery level, USB mass storage (use the SD card
  like a USB drive).

Not there yet: Wi-Fi features (file transfer, OPDS) are **untested** on the Diptyx; TXT/XTC books have no two-page spread; footnote and link
selection works on the left page of a spread only; the status LED
and the rumble motor are unused; no grayscale (the panels are black and white); no over-the-air updates (reflash with the steps below).

## Before you start

- A Diptyx running the **stock firmware layout** (1.0.x). Both the web flasher and the flash script check this and refuse to flash anything else.
- A USB-C **data** cable (a charge-only cable will not work) and a computer running Windows 10/11, macOS or Linux.
- Your books on the SD card as usual. CrossPoint keeps its own files in a `.crosspoint` folder there.
- **A backup.** Flashing only writes the app slot, but keep a copy of what you have. The flash script makes one for you (about 3 minutes).

## Put the Diptyx into download mode

Flashing needs the chip's built-in bootloader:

1. Unplug USB and switch the device completely off (hold the power button, wait about 20 seconds).
2. **Hold the center button pressed in.**
3. While holding it, plug in the USB-C cable. Keep holding for about 3 seconds, then let go. The screens stay as they were; that is normal.

It now shows up on your computer as an *Espressif / USB JTAG/serial debug unit*. To leave download mode, unplug the cable and plug it
in again **without** touching the center button.

## Option A: the web flasher (easiest)

Open **https://casualducko.github.io/crosspoint-diptyx/** in **Chrome or Edge** (Windows, macOS, Linux), put the Diptyx in download mode
as above, click *Connect to my Diptyx*, choose the *USB JTAG/serial* port, and follow the page. Safari, Firefox and phones cannot flash devices;
use option B.

The page does the same safety checks as the script before it writes anything: it must be an ESP32-S3 with 16 MB of flash and the **stock
Diptyx partition table**, or it refuses and changes nothing. It then downloads the firmware, checks its checksum and structure, writes
**only the app slot** (0x10000), verifies the written data, and restarts the device. Under *Other options* you can **go back to the stock
firmware** (downloaded from the makers' repository and checked against a known checksum) or install a firmware file of your own, and a
button saves a copy of your current app first (about 3 minutes). Everything runs in your browser; the only download is the firmware.

> The web flasher is newer than the script and has had less testing on real hardware, especially on Windows and Linux. If anything looks wrong,
> the script (option B) gives more detail.

## Option B: the flash script

Works the same on Windows, macOS and Linux. It needs **Python 3.10 or newer** (https://www.python.org/downloads/; the Python that ships with macOS is too old, install a newer one). Get the `diptyx` folder
(download the repository as a ZIP from GitHub, or clone it), open a terminal in it, put the device in download mode, then:

| System | Command |
|---|---|
| macOS / Linux | `./flash.sh` |
| Windows | `flash.bat` (double-click it, or run it in *Command Prompt* / *PowerShell*) |
| Any (if you manage Python yourself) | `python -m pip install --upgrade "esptool>=5,<6"` then `python flash.py` |

The first run sets up a private Python environment inside the folder (nothing is installed system-wide). Then it:

1. checks the device (ESP32-S3, 16 MB flash, **stock Diptyx partition table**; it refuses to continue otherwise),
2. downloads the newest release and verifies its checksum,
3. offers to back up your current flash first (`diptyx-flash-backup-*.bin`; keep it private, it contains your settings),
4. writes the app image to `0x10000`, the app slot, and nothing else.

Other commands: `./flash.sh check` (inspect only, writes nothing), `./flash.sh backup`, `./flash.sh flash --firmware my-build.bin`.

## After flashing

Unplug the cable and plug it in again without touching the center button. The left screen shows the CrossPoint home screen; the right screen
fills in with the card for your current book after a few seconds.

The Diptyx has five controls: a left button, a center rocker that presses and moves up and down, and a right button (plus the
power button).

| Control | What it does |
|---|---|
| Left button (page-left) | Back |
| Center button press | Select / open the reader menu |
| Right button (page-right) | Next / down (next page in a book) |
| Center button up / down | Move up / down; in a book, previous / next page |
| Power button | Hold: power off. Hold ~3 s when off: power on. |

- **Sleep:** it sleeps after the idle timeout (or from the menu) and any of the seven buttons wakes it. Holding the power button powers it off
  completely; press and **hold** it for about 3 seconds to turn it on again (a quick tap is not enough).
- **USB drive:** *File Transfer > USB Drive*, connect the cable, and the SD card appears on your computer. Eject it to return to the home screen.
- **Idle images:** put `idle_screen_right.jpg` (about 480x648, portrait) on the SD card root for the right screen while asleep.
- **Refresh:** a full screen flash happens every N page turns (*Settings > Reader > refresh frequency*, default 15). Lower it if you see ghosting.

## Going back to stock

The stock firmware is published by the Diptyx makers in <https://github.com/MartijndenHoed/Diptyx> (folder `firmware_release`). Download the
`diptyx_firmware_1.0.2_patch.bin` (the *patch* file, which only holds the app) and flash it with the same tool:

```
./flash.sh flash --firmware diptyx_firmware_1.0.2_patch.bin          (Windows: flash.bat flash --firmware ...)
```

Your stock settings are untouched by CrossPoint, so the stock firmware comes back as you left it. If you made a backup with
`flash.sh backup`, you can also restore it completely with esptool: `python -m esptool --chip esp32s3 write-flash 0x0 diptyx-flash-backup-....bin`.
(Download mode as above; this rewrites everything, including the bootloader, so use your own backup only.)

## Troubleshooting

- **"No Espressif USB device found" / cannot connect:** redo the download-mode steps; use a different cable (it must carry data) and port.
  Plug in the cable *while* holding the center button, and make sure the device was fully off first.
- **Linux, "permission denied":** add yourself to the serial group (`sudo usermod -aG dialout $USER` on Debian/Ubuntu, `uucp` on Arch/Fedora,
  then log out and in) or install the udev rule: `sudo cp 70-diptyx.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger` (then re-plug).
- **Windows:** no driver is needed on Windows 10/11. If *Device Manager* shows an unknown device, run Windows Update or install the
  Espressif USB JTAG/serial driver.
- **macOS:** nothing to install. If it asks for permission to access removable devices, allow it.
- **After flashing it sits in download mode:** unplug, switch off for 20 seconds, plug in again *without* touching the center button.
- **Screen contrast looks washed out or too dark:** the display voltage (VCOM) is read from your unit's own settings left by the stock
  firmware (`vcomLeft` / `vcomRight`); if there are none (a unit that never ran the stock firmware) it uses the stock default, 23. The serial
  log shows the values used (`[DIPTYX] left panel VCOM ...`). Open an issue with your stock values if the picture still looks wrong.
- **It will not boot at all:** download mode always works (it is in the chip's ROM, not in flash). Flash the stock app as shown above.

## Safety, in plain words

The flasher writes a single region of flash (the app slot at `0x10000`). It never touches the bootloader, the partition table or your
saved settings, and it refuses to run if the partition table is not the stock Diptyx one. Download mode cannot be removed by flashing the
app region, so a bad build cannot permanently brick the device. Still, firmware flashing carries risk and this is provided as is, without warranty.

## Building from source

```
git clone --recursive https://github.com/casualducko/crosspoint-diptyx
cd crosspoint-diptyx
pio run -e diptyx          # PlatformIO; produces .pio/build/diptyx/firmware.bin (the app image)
```

The Diptyx board support lives in a fork of the FreeInk SDK (`freeink-sdk`, the submodule). `diptyx-<release>` branches carry our small patch
on top of each CrossPoint release; see `CHANGES.md` in the port repository for every place we differ from upstream.

## Credits and licence

CrossPoint is MIT licensed (see `LICENSE`); the FreeInk SDK is MIT licensed in its own repository; the stock Diptyx firmware (MIT, Copyright (c) 2026
Diptyx) and hardware are by the Diptyx makers, and the Diptyx driver and settings in this port derive from it. **The compiled firmware also contains
third-party libraries, notably wolfSSL under the GPL-2.0, so the binary as a whole is distributed under the GPL-2.0, and the complete corresponding
source is this repository at the release tag.** See [`NOTICE`](../NOTICE) for every licence, the written source offer, and the licence texts in
[`licenses/`](licenses/). This is an unofficial port: no endorsement by the CrossPoint project, FreeInk or the Diptyx makers is implied.
