#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Diptyx "Comics" image dither for the 1-bit panel: legible lettering and line art next to shading. Unlike the
// per-pixel dithers in DitherUtils.h it needs the whole downscaled page (a sharpen, an edge mask and a local mean look
// at neighbouring pixels), so the decoders collect the page first and run this once.
//
//   1. Sharpen the gray page (unsharp mask, amount 1).
//   2. Near strong edges (lettering, line art): a local threshold, so strokes stay solid on an uneven paper tone.
//   3. Elsewhere: Floyd-Steinberg error diffusion of the tone-mapped page, which keeps shading as a halftone.
//
// diptyx/ci/comics_halftone_ref.py is an independent implementation of exactly this; test_comics_halftone.py requires
// the two to agree bit for bit. A change here needs the same change there, and a new pixel-cache file extension
// (ImageBlock.cpp getCachePath()) so caches written by the old version are not reused.
namespace ComicsHalftone {

// gray: width * height 8-bit pixels (the page already scaled to its output size, row-major). toneLut: the 256-entry tone
// curve the other 1-bit dithers use (bwToneCurve). out: width * height, 0 (black) or 3 (white), the two levels of the
// BW pass. Needs about five bytes per pixel of temporary memory (PSRAM when there is some). Returns false, leaving out
// untouched, when that could not be allocated or the size is not positive.
bool process(const uint8_t* gray, int width, int height, const uint8_t* toneLut, uint8_t* out);

// A page-sized buffer for the decoders (PSRAM when there is some; null when it cannot be had). Release with freeFrame().
uint8_t* allocFrame(size_t bytes);
void freeFrame(uint8_t* frame);

struct FrameDeleter {
  void operator()(uint8_t* frame) const { freeFrame(frame); }
};
using FramePtr = std::unique_ptr<uint8_t[], FrameDeleter>;

}  // namespace ComicsHalftone
