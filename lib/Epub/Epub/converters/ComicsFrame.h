#pragma once

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ComicsHalftone.h"
#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "ImageToFramebufferDecoder.h"
#include "PixelCache.h"

// The decoder side of the Comics dither (ComicsHalftone.h): while decoding, the converters keep the downscaled gray in
// a frame buffer instead of dithering each pixel; finish() then halftones the whole page and writes it the way the
// per-pixel path would have: into the framebuffer and the pixel cache.
namespace ComicsFrame {

// About seven bytes per pixel (input, output, temporaries) must fit the PSRAM: pages above 1.2 MP go through in bands
// of about 0.45 MP. The usual comic page, 0.25 MP, and the zoomed single page, 1.0 MP, are done whole.
constexpr size_t BANDED_ABOVE_PIXELS = 1200000;
constexpr size_t BAND_PIXELS = 450000;

// Comics is a Diptyx 1-bit mode for opaque images that are dithered at all.
inline bool wanted(const RenderConfig& config) {
  return BoardConfig::isDiptyx() && config.useDithering && !config.preserveAlpha &&
         config.ditherMode == PANEL_DITHER_COMICS;
}

// A frame for a page of width x height output pixels, white to begin with so a pixel the decoder never reaches stays
// white. Null when it cannot be allocated (the caller then dithers per pixel as usual).
inline ComicsHalftone::FramePtr makeFrame(const int width, const int height) {
  if (width <= 0 || height <= 0) return nullptr;
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height);
  ComicsHalftone::FramePtr frame(ComicsHalftone::allocFrame(bytes));
  if (frame)
    memset(frame.get(), 255, bytes);
  else
    LOG_ERR("CMX", "OOM: %u byte comics frame, using the ordered dither", static_cast<unsigned>(bytes));
  return frame;
}

// Halftones gray (width x height, the page at its output size) and writes it at (config.x, config.y), then streams it
// into the pixel cache when config.cachePath is set. Out of memory for the halftone falls back to a plain threshold
// (the page is still shown). Rows below the screen are skipped, as in the per-pixel path.
inline void finish(GfxRenderer& renderer, const RenderConfig& config, uint8_t* gray, const int width, const int height,
                   const int screenWidth, const int screenHeight) {
  uint8_t tone[256];
  for (int i = 0; i < 256; i++) tone[i] = bwToneCurve(static_cast<uint8_t>(i));
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height);
  const unsigned long startMs = millis();
  // A page of more than BANDED_ABOVE_PIXELS (a zoomed double-page picture) does not fit the halftone's temporaries in
  // one piece: it is done in bands, in place.
  const bool banded = bytes > BANDED_ABOVE_PIXELS;
  ComicsHalftone::FramePtr out;
  const uint8_t* values = nullptr;
  if (banded) {
    if (ComicsHalftone::processBanded(gray, width, height, tone, BAND_PIXELS)) values = gray;
  } else {
    out.reset(ComicsHalftone::allocFrame(bytes));
    if (out && ComicsHalftone::process(gray, width, height, tone, out.get())) values = out.get();
  }
  if (values) {
    const uint32_t* st = ComicsHalftone::lastStageMicros();
    LOG_DBG("CMX", "halftone %dx%d%s: %lu ms (sharpen %lu, edges %lu, mean %lu, decide %lu ms)", width, height,
            banded ? " in bands" : "", millis() - startMs, static_cast<unsigned long>(st[0] / 1000),
            static_cast<unsigned long>(st[1] / 1000), static_cast<unsigned long>(st[2] / 1000),
            static_cast<unsigned long>(st[3] / 1000));
  } else {
    LOG_ERR("CMX", "Comics halftone unavailable, using a plain threshold");
    for (size_t i = 0; i < bytes; i++) gray[i] = gray[i] >= 140 ? 3 : 0;
    values = gray;
  }

  PixelCache cache;
  bool caching = !config.cachePath.empty();
  if (caching && !cache.begin(config.cachePath, width, height, config.x, config.y, 1)) {
    LOG_ERR("CMX", "Failed to start cache stream, continuing without caching");
    caching = false;
  }
  DirectPixelWriter pw;
  pw.init(renderer);
  if (config.cacheOnly) pw.clipRows = 0;
  DirectCacheWriter cw;
  for (int dstY = 0; dstY < height; dstY++) {
    const int outY = config.y + dstY;
    if (outY >= screenHeight) break;
    pw.beginRow(outY);
    bool cacheRow = caching;
    if (cacheRow) {
      if (!cache.advanceTo(dstY)) {
        cacheRow = false;
        caching = false;
      } else {
        cw.init(cache.buffer, cache.bytesPerRow, cache.bandRows, cache.originX);
        cw.beginRow(outY, config.y + cache.bandStart);
      }
    }
    const uint8_t* row = values + static_cast<size_t>(dstY) * static_cast<size_t>(width);
    for (int dstX = 0; dstX < width; dstX++) {
      const int outX = config.x + dstX;
      if (outX >= screenWidth) break;
      pw.writePixel(outX, row[dstX], false);
      if (cacheRow) cw.writePixel(outX, row[dstX]);
    }
  }
  if (caching) cache.finalize();
  LOG_DBG("CMX", "page finished in %lu ms (halftone and cache write)", millis() - startMs);
}

}  // namespace ComicsFrame
