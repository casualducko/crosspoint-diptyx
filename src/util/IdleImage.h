#pragma once

#include <cstdint>

namespace idleimage {

// Makes sure bmpPath holds the 1-bit conversion of the JPEG at jpgPath, placed as `fit` says (a CrossPointSettings
// SLEEP_SCREEN_COVER_MODE value: Fit, Crop or Stretch). The cache key (at keyPath) follows the JPEG (size and time), the
// screen size and the placement, so replacing the file or changing the setting converts again. A JPEG that failed to
// convert is remembered until the file changes. False when there is no usable image.
bool ensureConverted(const char* tag, const char* jpgPath, const char* bmpPath, const char* keyPath, uint8_t fit,
                     int screenWidth, int screenHeight);

struct Placement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;  // fraction of the width removed (half from each side) when the bitmap is larger than the screen
  float cropY = 0.0f;
};

// Where to draw a converted bitmap of bitmapWidth x bitmapHeight on a screen: centered when it fits, scaled down to fit
// when it is larger, and cropped to fill when `crop` is set (a crop-converted bitmap covers the screen on one axis).
Placement place(int bitmapWidth, int bitmapHeight, int screenWidth, int screenHeight, bool crop);

}  // namespace idleimage
