# Diptyx font pack

Four serif families, prebuilt as CrossPoint SD-card fonts (`.cpfont`, format version 4) at 10, 12, 14, 16 and 18 pt.
The Diptyx panels are about 138 ppi, so the 10 pt size looks like a normal book size on them.

| Family | Notes |
|---|---|
| **Bitter** | Slab serif made for screens. Sturdy on e-ink. Latin and Cyrillic. |
| **ChareInk** | An e-ink tuned serif based on Charis SIL, from the CrossInk project. Latin and Cyrillic. |
| **Literata** | Screen-optimized serif by Google Fonts. Latin, Greek and Cyrillic. |
| **Crimson Pro** | Elegant old-style serif. Latin. |

## Install

1. Copy the `Bitter`, `ChareInk`, `Literata` and/or `CrimsonPro` folders into `/fonts/` on the SD card (or `/.fonts/`), so you have for example
   `/fonts/ChareInk/ChareInk_10.cpfont`. Each size is a separate file; copy only the sizes you want.
2. On the device: *Settings > Reader*, pick the family as the reader font, then a size. The size list shows the sizes
   that exist for that family.
3. The first time you open a book at a new font or size it is laid out again, which takes a moment.

## Rebuild

`diptyx-fonts.yaml` is the build config for the upstream tool:

```
cd lib/EpdFont/scripts
pip install -r requirements.txt
python3 build-sd-fonts.py --config ../../../diptyx/fonts/diptyx-fonts.yaml --output-dir out
```

The Noto Serif built into the firmware also has a 10 pt size; Noto Sans does not.

## Licences

All four families are licensed under the SIL Open Font License 1.1 (texts in `licenses/`). The `.cpfont` files are
converted from those fonts under that licence; the OFL allows bundling and redistribution with the licence text.
ChareInk is a derivative of Charis SIL that was renamed in line with the OFL's reserved-name rule, and is distributed by
the CrossInk project (https://github.com/uxjulia/crossink-fonts).
