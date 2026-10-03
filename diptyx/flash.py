#!/usr/bin/env python3
"""Flash CrossPoint onto a Diptyx dual-screen e-reader. Works on Windows, macOS and Linux.

    python flash.py            check the device, then download the latest release and flash it
    python flash.py check      only inspect the device (read-only, writes nothing)
    python flash.py backup     save a full 16 MB copy of the device's flash (do this first!)
    python flash.py flash --firmware my-build.bin     flash a file you already have

What it writes: ONLY the app region at 0x10000 (the factory app slot of the stock Diptyx layout). It never touches
the bootloader (0x0), the partition table (0x8000), your settings (NVS) or the book/asset storage partitions, and
it refuses to run if the device's partition table is not the stock Diptyx one. Going back to the stock firmware
is the same command with the stock app image (see README.md).

Needs Python 3.10+ (esptool 5 requires it) and esptool 5:  python -m pip install --upgrade "esptool>=5,<6"
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

REPO = os.environ.get("DIPTYX_FLASH_REPO", "casualducko/crosspoint-diptyx")  # override to use a fork
APP_OFFSET = 0x10000  # factory app slot of the stock Diptyx partition table
APP_MAX = 6 * 1024 * 1024  # size of that slot
FLASH_SIZE = 16 * 1024 * 1024
PTABLE_OFFSET, PTABLE_LEN = 0x8000, 0xC00
# SHA-256 of the 3 KB partition table at 0x8000 in the stock Diptyx firmware (1.0.1, 1.0.2 and the factory image).
STOCK_PTABLE_SHA256 = "7a1389f74052c466a758e2d93babc42a1a24738b81748cc3f46ce6606843330d"
ESPRESSIF_VID = 0x303A

DOWNLOAD_MODE_HELP = """
Could not talk to the device. Put the Diptyx in download mode:
  1. Unplug the USB cable and switch the device completely off (hold the power button, wait ~20 seconds).
  2. HOLD the center button pressed in.
  3. While holding it, plug in the USB-C cable. Keep holding for ~3 seconds, then let go.
  4. Run this command again.
Also try a different cable (it must carry data, not only power) and a different USB port.
"""


def die(msg, code=1):
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def esptool(*args, capture=False, stay=False):
    """Run esptool 5 as `python -m esptool ...`. Returns (returncode, combined output).

    stay=True adds `--after no-reset` for READ-ONLY commands: the chip stays in the bootloader between steps. Without it
    every command hard-resets the chip, which then boots the installed app (the stock firmware re-enumerates as another
    USB device) and the next step can no longer find it. The final write keeps the default reset so the new app starts.
    """
    cmd = [sys.executable, "-m", "esptool"]
    if stay:
        # global options go before the sub-command: ... --chip X --port P --after no-reset <command>
        i = len(args)
        for k, a in enumerate(args):
            if not a.startswith("-") and (k == 0 or args[k - 1] not in ("--chip", "--port", "--baud", "--after", "--before")):
                i = k
                break
        args = (*args[:i], "--after", "no-reset", *args[i:])
    cmd += list(args)
    if capture:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    return subprocess.run(cmd).returncode, ""


def require_esptool():
    try:
        import esptool  # noqa: F401
        version = getattr(esptool, "__version__", "0")
    except ImportError:
        die('esptool is not installed. Run:  python -m pip install --upgrade "esptool>=5,<6"')
    major = int(version.split(".")[0])
    if major < 5 or major >= 6:  # this script relies on the v5 command line (hyphenated commands, --after no-reset)
        die(f'esptool {version} is not supported (need 5.x). Run:  python -m pip install --upgrade "esptool>=5,<6"')


def find_port(wanted):
    if wanted:
        return wanted
    try:
        from serial.tools import list_ports
    except ImportError:
        die("pyserial is missing (it normally comes with esptool). Reinstall esptool.")
    ports = [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        print(DOWNLOAD_MODE_HELP)
        die("No Espressif USB device found (looking for vendor id 303A).")
    names = ", ".join(p.device for p in ports)
    die(f"More than one Espressif device found ({names}). Unplug the others or pass --port.")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_app_image(path):
    """Cheap sanity checks so a wrong file can never be written."""
    size = os.path.getsize(path)
    if size > APP_MAX:
        die(f"{path} is {size:,} bytes; the app slot is only {APP_MAX:,} bytes.")
    with open(path, "rb") as f:
        head = f.read(16)
    if len(head) < 16 or head[0] != 0xE9:
        die(f"{path} is not an ESP32 app image (bad magic byte). Is it a merged/full-flash image? Use the *-app.bin file.")
    chip_id = int.from_bytes(head[12:14], "little")
    if chip_id != 9:
        die(f"{path} is built for another chip (chip id {chip_id}, expected 9 = ESP32-S3).")
    with open(path, "rb") as f:
        f.seek(32)
        magic = int.from_bytes(f.read(4), "little")
    if magic != 0xABCD5432:  # esp_app_desc_t: present in every ESP-IDF/Arduino application image, not in a bootloader
        die(f"{path} does not look like an application image (no app descriptor). Is it a bootloader or partition image?")
    with open(path, "rb") as f:
        data = f.read()
    seg_count = data[1]
    pos, xor = 24, 0xEF
    for _ in range(seg_count):
        if pos + 8 > len(data):
            die(f"{path} is cut short (a segment header is missing); the file is damaged.")
        seg_size = int.from_bytes(data[pos + 4:pos + 8], "little")
        pos += 8
        if pos + seg_size > len(data):
            die(f"{path} is cut short (a segment runs past the end of the file); the file is damaged.")
        for b in data[pos:pos + seg_size]:
            xor ^= b
        pos += seg_size
    checksum_pos = pos + ((15 - (pos % 16)) % 16)
    if checksum_pos >= len(data) or data[checksum_pos] != xor:
        die(f"{path} fails its checksum; the file is damaged.")
    end = checksum_pos + 1
    if data[23] == 1:  # SHA-256 digest appended after the checksum byte
        if end + 32 > len(data) or hashlib.sha256(data[:end]).digest() != data[end:end + 32]:
            die(f"{path} fails its SHA-256 digest; the file is damaged.")
        end += 32
    if any(b != 0xFF for b in data[end:]):
        die(f"{path} has extra data after the end of the app image. Is it a merged/full-flash image? Use the *-app.bin file.")
    return size


def preflight(port):
    """Read-only checks: it is an ESP32-S3 with 16 MB flash and the stock Diptyx partition table."""
    print(f"Checking the device on {port} (read-only)...")
    rc, out = esptool("--chip", "esp32s3", "--port", port, "flash-id", capture=True, stay=True)
    if rc != 0:
        print(out[-600:])
        print(DOWNLOAD_MODE_HELP)
        die("Could not connect to the device.")
    if "ESP32-S3" not in out:
        die("This does not look like an ESP32-S3 device.")
    m = re.search(r"Detected flash size:\s*(\d+)\s*MB", out)
    if not m or int(m.group(1)) * 1024 * 1024 != FLASH_SIZE:
        die(f"Expected 16 MB of flash, found: {m.group(0) if m else 'unknown'}.")
    mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    with tempfile.TemporaryDirectory() as tmp:
        pt = os.path.join(tmp, "ptable.bin")
        rc, out2 = esptool("--chip", "esp32s3", "--port", port, "read-flash", hex(PTABLE_OFFSET), hex(PTABLE_LEN), pt, capture=True, stay=True)
        if rc != 0:
            print(out2[-600:])
            die("Could not read the partition table.")
        got = sha256_file(pt)
    print("  chip: ESP32-S3, flash: 16 MB" + (f", MAC {mac.group(1)}" if mac else ""))
    if got != STOCK_PTABLE_SHA256:
        return False, mac.group(1) if mac else ""
    print("  partition table: matches the stock Diptyx layout")
    return True, mac.group(1) if mac else ""


def http_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "diptyx-flasher", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "diptyx-flasher"})
    with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length", 0))
        done = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if total:
                print(f"\r  downloading {done * 100 // total:3d}%", end="", flush=True)
    print()


def fetch_latest(tmp):
    print(f"Looking up the latest release of {REPO} ...")
    try:
        rel = http_json(f"https://api.github.com/repos/{REPO}/releases/latest")
    except (urllib.error.URLError, OSError, ValueError) as e:
        die(f"Could not get the latest release from GitHub ({e}). Download the *-app.bin and its .sha256 from the "
            "Releases page and use --firmware.")
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    app = next((n for n in assets if n.endswith("-app.bin")), None)
    if not app or app + ".sha256" not in assets:
        die("The latest release has no *-app.bin with a .sha256 file.")
    print(f"  release {rel.get('tag_name')}: {app}")
    bin_path, sha_path = os.path.join(tmp, app), os.path.join(tmp, app + ".sha256")
    try:
        download(assets[app], bin_path)
        download(assets[app + ".sha256"], sha_path)
        with open(sha_path) as f:
            parts = f.read().split()
    except (urllib.error.URLError, OSError) as e:
        die(f"Download failed ({e}). Check your connection and try again.")
    if not parts:
        die("The checksum file in the release is empty.")
    expected = parts[0].lower()
    if sha256_file(bin_path) != expected:
        die("Checksum mismatch: the download is corrupt. Try again.")
    print("  checksum OK")
    return bin_path


def do_backup(port, mac, out=None):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = out or f"diptyx-flash-backup-{(mac or 'device').replace(':', '')}-{stamp}.bin"
    print(f"Backing up the full 16 MB flash to {out} (about 2-3 minutes)...")
    rc, _ = esptool("--chip", "esp32s3", "--port", port, "read-flash", "0", hex(FLASH_SIZE), out, stay=True)
    if rc != 0:
        die("Backup failed; nothing was changed on the device.")
    print(f"Backup saved: {out}\nKeep it private: it contains your device's settings.\nSHA-256 {sha256_file(out)}")
    return out


def ask(question, default_yes=True, assume=None):
    if assume is not None:
        return assume
    if not sys.stdin.isatty():
        die("This needs a yes/no answer but input is not interactive. Run it in a terminal, or pass --yes "
            "(and --no-backup to skip the backup).")
    suffix = " [Y/n] " if default_yes else " [y/N] "
    try:
        ans = input(question + suffix).strip().lower()
    except (EOFError, KeyboardInterrupt):
        die("Cancelled; nothing was written.")
    return default_yes if not ans else ans.startswith("y")


def leave_bootloader(port):
    """Read-only commands leave the chip in download mode; reset it so the installed app starts again."""
    esptool("--chip", "esp32s3", "--port", port, "read-mac", capture=True)


def cmd_check(args):
    port = find_port(args.port)
    ok, _ = preflight(port)
    leave_bootloader(port)
    if not ok:
        print("  partition table: DIFFERENT from the stock Diptyx layout; flashing would be refused.")
        return 1
    print("\nThe device is ready to be flashed. Nothing was written.")
    return 0


def cmd_backup(args):
    port = find_port(args.port)
    ok, mac = preflight(port)
    try:
        do_backup(port, mac, args.out)
    finally:
        leave_bootloader(port)
    return 0


def cmd_flash(args):
    with tempfile.TemporaryDirectory() as tmp:
        firmware = args.firmware or fetch_latest(tmp)
        if not os.path.isfile(firmware):
            die(f"Firmware file not found: {firmware}")
        size = check_app_image(firmware)
        print(f"Firmware: {firmware} ({size:,} bytes, sha256 {sha256_file(firmware)[:16]}...)")
        port = find_port(args.port)
        ok, mac = preflight(port)
        written = False
        try:
            if not ok:
                die("The device's partition table is not the stock Diptyx layout, so 0x10000 may not be the app slot.\n"
                    "Nothing was written.")
            if args.backup or (not args.no_backup and ask("Back up your current flash first? (recommended, ~3 min)", True, args.yes or None)):
                do_backup(port, mac)
            print(f"\nAbout to write the app image to {hex(APP_OFFSET)} only. Bootloader, partition table and your data stay as they are.")
            if not ask("Flash now?", True, args.yes or None):
                print("Cancelled; nothing was written.")
                return 1
            written = True  # the write below resets the chip itself (default reset)
            rc, _ = esptool("--chip", "esp32s3", "--port", port, "write-flash", hex(APP_OFFSET), firmware)
            if rc != 0:
                die("Flashing failed. The device is still recoverable: put it in download mode and run this again.")
        finally:
            if not written:
                leave_bootloader(port)  # the read-only steps left the chip in download mode
    print("\nDone. Unplug the USB cable, then plug it in again WITHOUT touching the center button.\n"
          "If the device stays in download mode, switch it fully off for ~20 seconds and try again.")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Flash CrossPoint onto a Diptyx e-reader.")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("check", "backup", "flash"):
        p = sub.add_parser(name)
        p.add_argument("--port", help="serial port (default: auto-detect the Espressif USB device)")
        if name == "backup":
            p.add_argument("--out", help="backup file name")
        if name == "flash":
            p.add_argument("--firmware", help="app image to flash (default: download the latest release)")
            p.add_argument("--yes", action="store_true", help="do not ask questions (backs up first unless --no-backup)")
            p.add_argument("--backup", action="store_true", help="always back up first")
            p.add_argument("--no-backup", action="store_true", help="skip the backup question")
    args = ap.parse_args()
    if args.cmd is None:
        args = ap.parse_args(["flash"])
    require_esptool()
    sys.exit({"check": cmd_check, "backup": cmd_backup, "flash": cmd_flash}[args.cmd](args))


if __name__ == "__main__":
    main()
