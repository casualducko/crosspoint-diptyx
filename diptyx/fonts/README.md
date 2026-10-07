# Diptyx font pack

Thirteen reader font families, all **mono-hinted** and prebuilt as CrossPoint SD-card fonts (`.cpfont`, format version 4) at 10, 12, 14,
16 and 18 pt (about 35 MB for everything; copy only what you want). The firmware also has **Literata Mono built in at 10 to 18 pt** (its
serif family), and 10 pt is the stock reading size, so nothing here is needed to get crisp text; this pack adds more families.

## Why mono-hinted

The Diptyx panels are black and white only. Ordinary fonts keep four anti-aliasing shades per pixel (made for grayscale e-ink), and on
a black-and-white panel those light shades come out as fuzzy, slightly bold edges. These builds are hinted for a black-and-white
grid: each stem is fitted to whole pixels, there is no gray fringe, and the letter spacing is whole pixels, so text is solid and
crisp. Diagonals and curls are still stair-stepped (the panel is about 138 ppi); that is the hardware.

| Family | Kind | Notes |
|---|---|---|
| **Literata Mono** | serif | The stock font. Screen-optimized, solid stems. Latin, Greek, Cyrillic. |
| **Source Serif 4 Mono** | serif | Adobe text serif; wide, sturdy stems. |
| **Spectral Mono** | serif | Elegant screen-first serif, a little lighter than Literata. |
| **Bitter Mono** | slab serif | Made for screens. Latin, Cyrillic. |
| **ChareInk Mono** | serif | An e-ink tuned face based on Charis SIL, from the CrossInk project. |
| **Crimson Pro Mono** | serif | Elegant old-style serif. Latin. |
| **Atkinson Hyperlegible Next Mono** | sans | Designed for legibility. |
| **Inter Mono** | sans | A clean, neutral screen sans. |
| **Noto Sans Monochrome** | sans | Broad script coverage. (Not the monospace "Noto Sans Mono" typeface.) |
| **Dyslexic Mono** | accessibility | OpenDyslexic, for readers who find it easier. Renamed because the original name is reserved. |
| **Noto Naskh Arabic Mono** | serif (naskh) | For Arabic books. Arabic and Latin; regular and bold (no italic: italic text uses regular). |
| **Frank Ruhl Libre Mono** | serif | For Hebrew books, vowel points included. Hebrew and Latin; regular and bold. |
| **Noto Sans Hebrew Mono** | sans | Hebrew sans. Hebrew and Latin; regular and bold. |

## Install

1. Copy the family folders you want into `/fonts/` on the SD card (or `/.fonts/`), so you have for example
   `/fonts/LiterataMono/LiterataMono_12.cpfont`. Each size is a separate file; copy only the sizes you want.
2. On the device: *Settings > Reader*, pick the family as the reader font, then a size. The size list shows the sizes that exist for
   that family.
3. The first time you open a book at a new font or size it is laid out again, which takes a moment.

## Rebuild

`diptyx-fonts.yaml` is the build config for the upstream tool (`mono: true` makes a mono-hinted build):

```
cd lib/EpdFont/scripts
pip install -r requirements.txt
python3 build-sd-fonts.py --config ../../../diptyx/fonts/diptyx-fonts.yaml --output-dir out
```

## Your own bitmap fonts (the stock `.yaff` fonts)

The stock Diptyx firmware keeps its fonts as `.yaff` bitmap fonts in `/fonts/` (Courier, Chicago, Geneva, Times, Espy Sans and so on).
CrossPoint reads `.cpfont`, so those files do not show up as they are. `yaff2cpfont.py` converts them; it is plain Python 3 with no
extra packages, and you run it on your own copies of the fonts, for example straight from the SD card:

```
python3 yaff2cpfont.py /path/to/sdcard/fonts -o converted          # every .yaff in the folder
python3 yaff2cpfont.py Courier_12.yaff Courier_14.yaff -o converted  # or just some
python3 yaff2cpfont.py /path/to/sdcard/fonts -o converted --scale 2  # pixel-doubled, for larger text
```

Copy the family folders it writes (`converted/Courier/`, ...) into `/fonts/` on the SD card. They then appear under *Settings > Reader >
Font Family*. `Family_Bold_12.yaff` and `Family_Italic_12.yaff` are merged into the same family as its bold and italic styles.

Things to know: a bitmap font only exists at the pixel size it was drawn at (the number in `Courier_12` is a pixel height, so 12 is small on
these panels; `--scale 2` doubles it). Nothing is smoothed. Check the licence of the fonts you convert: this tool only changes the file
format, and the pack in this repository does not include the stock fonts.

## Licences

Every family here is licensed under the SIL Open Font License 1.1 (texts in `licenses/`). The `.cpfont` files are converted from those
fonts under that licence. The OFL counts a format conversion as a "Modified Version", which may not use a font's Reserved Font Name,
so the families whose licence reserves their name (Merriweather, Gentium, IBM Plex, OpenDyslexic) are either not included or are
renamed (Dyslexic Mono). ChareInk is a renamed derivative of Charis SIL, distributed by the CrossInk project
(https://github.com/uxjulia/crossink-fonts).
