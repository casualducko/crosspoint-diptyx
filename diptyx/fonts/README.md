# Diptyx font pack

Nine reader font families, all **mono-hinted** and prebuilt as CrossPoint SD-card fonts (`.cpfont`, format version 4) at 10, 12, 14,
16 and 18 pt (about 30 MB for everything; copy only what you want). The firmware also has **Literata Mono at 10 pt built in**, which
is the stock reading font, so nothing here is needed to get crisp text; this pack adds more families and sizes.

## Why mono-hinted

The Diptyx panels are black and white only. Ordinary fonts keep four anti-aliasing shades per pixel (made for grayscale e-ink), and on
a black-and-white panel those light shades come out as fuzzy, slightly bold edges. These builds are hinted for a black-and-white
grid: each stem is fitted to whole pixels, there is no gray fringe, and the letter spacing is whole pixels, so text is solid and
crisp. Diagonals and curls are still stair-stepped (the panel is about 138 ppi); that is the hardware.

| Family | Kind | Notes |
|---|---|---|
| **Literata Mono** | serif | The stock font. Screen-optimized, solid stems. Latin, Greek, Cyrillic. |
| **Vollkorn Mono** | serif | Sturdy, warm book face. |
| **Bitter Mono** | slab serif | Made for screens. Latin, Cyrillic. |
| **ChareInk Mono** | serif | An e-ink tuned face based on Charis SIL, from the CrossInk project. |
| **Crimson Pro Mono** | serif | Elegant old-style serif. Latin. |
| **Atkinson Hyperlegible Next Mono** | sans | Designed for legibility. |
| **Inter Mono** | sans | A clean, neutral screen sans. |
| **Noto Sans Monochrome** | sans | Broad script coverage. (Not the monospace "Noto Sans Mono" typeface.) |
| **Dyslexic Mono** | accessibility | OpenDyslexic, for readers who find it easier. Renamed because the original name is reserved. |

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

## Licences

Every family here is licensed under the SIL Open Font License 1.1 (texts in `licenses/`). The `.cpfont` files are converted from those
fonts under that licence. The OFL counts a format conversion as a "Modified Version", which may not use a font's Reserved Font Name,
so the families whose licence reserves their name (Merriweather, Gentium, IBM Plex, OpenDyslexic) are either not included or are
renamed (Dyslexic Mono). ChareInk is a renamed derivative of Charis SIL, distributed by the CrossInk project
(https://github.com/uxjulia/crossink-fonts).
