#!/usr/bin/env python3
"""Convert bitmap fonts in the YAFF text format (the stock Diptyx firmware's /fonts/*.yaff) to CrossPoint .cpfont files.

Plain Python 3, no dependencies. Run it on YOUR OWN copies of the fonts, for example the ones on your Diptyx SD card:

    python3 yaff2cpfont.py /path/to/sdcard/fonts            # converts every *.yaff in the folder
    python3 yaff2cpfont.py Courier_12.yaff Courier_14.yaff  # or just some files
    python3 yaff2cpfont.py /path/to/sdcard/fonts --scale 2  # pixel-doubled: bigger text from the small bitmap sizes

Result: one folder per family (for example out/Courier/Courier_12.cpfont). Copy the family folders into /fonts/ on the SD
card; they then show up under Settings > Reader > Font Family. Files named Family_Bold_12.yaff / Family_Italic_12.yaff /
Family_Bold_Italic_12.yaff are merged into the same family as its bold and italic styles.

Bitmap fonts exist only at their drawn pixel size: the "12" in Courier_12 is a pixel height, not a true point size, and the
glyphs are never smoothed or scaled (--scale repeats whole pixels). Check the licence of the fonts you convert; this tool
only changes their file format.
"""
import argparse
import os
import re
import struct
import sys

CPFONT_VERSION = 4  # must match lib/EpdFont/scripts/cpfont_version.py
MAGIC = b"CPFONT\x00\x00"
STYLE_IDS = {"regular": 0, "bold": 1, "italic": 2, "bolditalic": 3}


# ---------------------------------------------------------------- YAFF parsing

def parse_yaff(path):
    """Returns (props, glyphs): font properties and {codepoint: (rows, left, right, shift_up)}.

    rows is a list of strings of '.' and '@' (an empty glyph has no rows)."""
    props = {}
    glyphs = {}
    labels = []  # labels of the glyph block being read
    rows = []
    gprops = {}
    in_glyph = False
    empty = False  # the current glyph block was the empty-glyph marker "-"

    def flush():
        nonlocal labels, rows, gprops, in_glyph, empty
        if in_glyph and labels:
            cps = [int(m.group(1), 16) for m in (re.fullmatch(r"u\+([0-9a-fA-F]{4,6})", lab) for lab in labels) if m]
            if not cps:  # no Unicode label: decode a single-byte label as Mac Roman
                for lab in labels:
                    m = re.fullmatch(r"0x([0-9a-fA-F]{2})", lab)
                    if m and int(m.group(1), 16) >= 0x20:
                        try:
                            cps = [ord(bytes([int(m.group(1), 16)]).decode("mac_roman"))]
                        except (UnicodeDecodeError, ValueError):
                            pass
                        break
            for cp in cps:  # every label names the same glyph
                if cp not in glyphs:
                    glyphs[cp] = (list(rows), int(gprops.get("left-bearing", 0)), int(gprops.get("right-bearing", 0)),
                                  int(gprops["shift-up"]) if "shift-up" in gprops else None)
        labels, rows, gprops, in_glyph, empty = [], [], {}, False, False

    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            if not line[0].isspace():
                m = re.fullmatch(r"([^\s:][^:]*):\s*(.*)", line)
                if not m:
                    continue
                key, value = m.group(1), m.group(2)
                if value == "":  # a glyph label; several labels in a row name the same glyph
                    if in_glyph and (rows or gprops or empty):
                        flush()
                    in_glyph = True
                    labels.append(key)
                else:  # a font property
                    if in_glyph:
                        flush()
                    props[key] = value
            else:
                body = line.strip()
                if re.fullmatch(r"[.@]+", body):
                    rows.append(body)
                elif body == "-":  # an empty glyph: the next label starts a new one
                    empty = True
                else:
                    m = re.fullmatch(r"([\w-]+):\s*(-?\d+)", body)
                    if m:
                        gprops[m.group(1)] = m.group(2)
    flush()
    return props, glyphs


# ------------------------------------------------------------- cpfont building

def pack_bitmap(rows, scale):
    """2-bit glyph bitmap, row-major, MSB first, 4 pixels per byte: 3 = ink, 0 = none."""
    pixels = []
    for row in rows:
        expanded = "".join(ch * scale for ch in row)
        for _ in range(scale):
            pixels.extend(3 if ch == "@" else 0 for ch in expanded)
    out = bytearray()
    for i in range(0, len(pixels), 4):
        chunk = pixels[i:i + 4] + [0] * (4 - len(pixels[i:i + 4]))
        out.append((chunk[0] << 6) | (chunk[1] << 4) | (chunk[2] << 2) | chunk[3])
    return bytes(out)


def build_style(path, scale):
    props, glyphs = parse_yaff(path)
    if not glyphs:
        raise SystemExit(f"{path}: no glyphs with Unicode labels found")
    font_shift = int(props.get("shift-up", 0))
    ascent = int(props.get("ascent", 0))
    descent = int(props.get("descent", 0))
    line_height = int(props.get("line-height", 0))
    cps = sorted(glyphs)
    entries = []  # (width, height, advance_fp4, left, top, packed)
    for cp in cps:
        rows, left, right, shift = glyphs[cp]
        width = max((len(r) for r in rows), default=0)
        height = len(rows)
        rows = [r.ljust(width, ".") for r in rows]
        shift = font_shift if shift is None else shift
        advance = left + width + right
        if not (0 <= advance * scale < 4096 and width * scale < 256 and height * scale < 256):
            raise SystemExit(f"{path}: glyph U+{cp:04X} does not fit the format (size {width}x{height}, advance "
                             f"{advance} at scale {scale}; width and height must stay under 256 px, advance 0..4095 px)")
        entries.append((width * scale, height * scale, (advance * scale) << 4, left * scale, (height + shift) * scale,
                        pack_bitmap(rows, scale)))
    if not ascent:
        ascent = max((e[4] for e in entries), default=0) // scale
    if not descent:
        descent = max(0, max((e[1] - e[4] for e in entries), default=0) // scale)
    if not line_height:
        line_height = ascent + descent
    intervals = []
    for cp in cps:
        if intervals and cp == intervals[-1][1] + 1:
            intervals[-1][1] = cp
        else:
            intervals.append([cp, cp])
    return {"intervals": intervals, "entries": entries, "advanceY": line_height * scale, "ascender": ascent * scale,
            "descender": -descent * scale}


def style_sections(style):
    intervals = bytearray()
    offset = 0
    for start, end in style["intervals"]:
        intervals += struct.pack("<III", start, end, offset)
        offset += end - start + 1
    glyphs = bytearray()
    bitmaps = bytearray()
    for width, height, advance, left, top, packed in style["entries"]:
        glyphs += struct.pack("<BBHhhH2xI", width, height, advance, left, top, len(packed), len(bitmaps))
        bitmaps += packed
    # no kerning, no ligatures: bitmap fonts carry neither
    return [bytes(intervals), bytes(glyphs), b"", b"", b"", b"", bytes(bitmaps)]


def write_cpfont(path, styles):
    """styles: {style_id: build_style() result}"""
    order = sorted(styles)
    sections = {sid: style_sections(styles[sid]) for sid in order}
    offset = 32 + 32 * len(order)
    toc = bytearray()
    for sid in order:
        st = styles[sid]
        if not 0 < st["advanceY"] <= 255:
            raise SystemExit(f"{path}: line height {st['advanceY']} does not fit the format (try a smaller --scale)")
        toc += struct.pack("<B3xIIBhhHHBBBI4x", sid, len(st["intervals"]), len(st["entries"]), st["advanceY"],
                           st["ascender"], st["descender"], 0, 0, 0, 0, 0, offset)
        offset += sum(len(s) for s in sections[sid])
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<8sHHB19s", MAGIC, CPFONT_VERSION, 1, len(order), bytes(19)))
        fh.write(toc)
        for sid in order:
            for section in sections[sid]:
                fh.write(section)
    return os.path.getsize(path)


# ----------------------------------------------------------------------- main

NAME = re.compile(r"^(?P<family>.+?)(?:_(?P<style>Bold_Italic|BoldItalic|Bold|Italic))?_(?P<size>\d+(?:x\d+)?)$")


def classify(filename):
    stem = os.path.splitext(os.path.basename(filename))[0]
    m = NAME.match(stem)
    if not m:
        return None
    size = m.group("size")
    pixel_height = int(size.split("x")[1]) if "x" in size else int(size)
    style = (m.group("style") or "regular").replace("_", "").lower()
    return m.group("family").replace("_", ""), pixel_height, style


def main():
    ap = argparse.ArgumentParser(description="Convert YAFF bitmap fonts to CrossPoint .cpfont files.")
    ap.add_argument("inputs", nargs="+", help=".yaff files or folders containing them")
    ap.add_argument("-o", "--output-dir", default="cpfont-out", help="where the family folders are written")
    ap.add_argument("--scale", type=int, default=1, choices=(1, 2, 3, 4),
                    help="repeat every pixel this many times (2 = double size)")
    args = ap.parse_args()

    files = []
    for item in args.inputs:
        if os.path.isdir(item):
            files += sorted(os.path.join(item, f) for f in os.listdir(item) if f.lower().endswith(".yaff"))
        else:
            files.append(item)
    groups = {}  # (family, size) -> {style_id: path}
    for f in files:
        info = classify(f)
        if not info:
            print(f"skipping {f}: name is not Family_<size>.yaff", file=sys.stderr)
            continue
        family, size, style = info
        groups.setdefault((family, size), {})[STYLE_IDS[style]] = f
    if not groups:
        raise SystemExit("no .yaff files found")

    total = 0
    for (family, size), by_style in sorted(groups.items()):
        # A family without a regular file (for example only Bold) uses its first style as regular.
        if 0 not in by_style:
            first = sorted(by_style)[0]
            by_style = {0: by_style[first]}
        styles = {sid: build_style(path, args.scale) for sid, path in by_style.items()}
        out = os.path.join(args.output_dir, family, f"{family}_{size * args.scale}.cpfont")
        n = write_cpfont(out, styles)
        total += n
        print(f"{out}  ({n / 1024:.0f} KB, styles: {', '.join(k for k, v in STYLE_IDS.items() if v in styles)})")
    print(f"done: {len(groups)} font files, {total / 1048576:.1f} MB. Copy the family folders into /fonts/ on the SD card.")


if __name__ == "__main__":
    main()
