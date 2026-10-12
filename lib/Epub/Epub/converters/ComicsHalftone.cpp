#include "ComicsHalftone.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <memory>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace {

// Temporary buffers are large (a byte or two per pixel): PSRAM first, internal RAM as the fallback, plain malloc on a
// host (the unit test). Everything is released with free(), which also takes heap_caps_malloc() memory.
void* allocTemp(const size_t bytes) {
#ifdef ESP_PLATFORM
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
  return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

struct FreeDeleter {
  void operator()(void* p) const { free(p); }
};
template <typename T>
using TempBuf = std::unique_ptr<T[], FreeDeleter>;

template <typename T>
TempBuf<T> makeTemp(const size_t count) {
  return TempBuf<T>(static_cast<T*>(allocTemp(count * sizeof(T))));
}

// Index of the nearest pixel inside [0, n): the edges are replicated.
inline int clampIndex(const int i, const int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }

// The firmware's 1-bit tone mapping after the tone curve: below 40 black, above 224 white, a stretch in between.
inline int stretchTone(const int tone) {
  if (tone <= 40) return 0;
  if (tone >= 224) return 255;
  return ((tone - 40) * 355) >> 8;
}

constexpr int EDGE_CONTRAST = 110;  // max - min of the 5x5 neighbourhood above which a pixel counts as an edge
constexpr int MEAN_RADIUS = 6;      // the local threshold looks at a 13x13 window
constexpr int MEAN_AREA = (2 * MEAN_RADIUS + 1) * (2 * MEAN_RADIUS + 1);
constexpr int THRESHOLD_CAP = 150;  // paper stays white: the local threshold never rises above a mid gray

}  // namespace

namespace ComicsHalftone {

bool process(const uint8_t* gray, const int width, const int height, const uint8_t* toneLut, uint8_t* out) {
  if (!gray || !toneLut || !out || width <= 0 || height <= 0) return false;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  auto sharp = makeTemp<uint8_t>(pixels);
  auto edge = makeTemp<uint8_t>(pixels);
  auto spare = makeTemp<uint8_t>(pixels);
  auto sums = makeTemp<uint16_t>(pixels);
  auto colSum = makeTemp<int>(static_cast<size_t>(width));
  auto errCur = makeTemp<int>(static_cast<size_t>(width) + 2);
  auto errNext = makeTemp<int>(static_cast<size_t>(width) + 2);
  if (!sharp || !edge || !spare || !sums || !colSum || !errCur || !errNext) return false;

  const auto at = [width](const int y, const int x) { return static_cast<size_t>(y) * static_cast<size_t>(width) + x; };

  // 1. Sharpen: S = clamp(2 * G - blur(G)), blur = [1 4 6 4 1] x [1 4 6 4 1] / 256, rounded.
  static constexpr int kernel[5] = {1, 4, 6, 4, 1};
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int sum = 0;
      for (int k = 0; k < 5; k++) sum += kernel[k] * gray[at(y, clampIndex(x + k - 2, width))];
      sums[at(y, x)] = static_cast<uint16_t>(sum);
    }
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int sum = 0;
      for (int k = 0; k < 5; k++) sum += kernel[k] * sums[at(clampIndex(y + k - 2, height), x)];
      const int blur = (sum + 128) >> 8;
      sharp[at(y, x)] = static_cast<uint8_t>(std::clamp(2 * gray[at(y, x)] - blur, 0, 255));
    }
  }

  // 2. Edge mask: a pixel whose 5x5 neighbourhood of the (unsharpened) page spans more than EDGE_CONTRAST, then grown
  //    by one pixel in every direction.
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int hi = 0;
      int lo = 255;
      for (int k = -2; k <= 2; k++) {
        const int v = gray[at(y, clampIndex(x + k, width))];
        hi = std::max(hi, v);
        lo = std::min(lo, v);
      }
      sums[at(y, x)] = static_cast<uint16_t>((hi << 8) | lo);
    }
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int hi = 0;
      int lo = 255;
      for (int k = -2; k <= 2; k++) {
        const int v = sums[at(clampIndex(y + k, height), x)];
        hi = std::max(hi, v >> 8);
        lo = std::min(lo, v & 0xFF);
      }
      edge[at(y, x)] = (hi - lo) > EDGE_CONTRAST ? 1 : 0;
    }
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t any = 0;
      for (int k = -1; k <= 1; k++) any |= edge[at(y, clampIndex(x + k, width))];
      spare[at(y, x)] = any;
    }
  }
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t any = 0;
      for (int k = -1; k <= 1; k++) any |= spare[at(clampIndex(y + k, height), x)];
      edge[at(y, x)] = any;
    }
  }

  // 3. Row sums of the sharpened page over the 13-pixel window, for the local mean (the column sums are rolled below).
  for (int y = 0; y < height; y++) {
    int window = 0;
    for (int k = -MEAN_RADIUS; k <= MEAN_RADIUS; k++) window += sharp[at(y, clampIndex(k, width))];
    for (int x = 0; x < width; x++) {
      sums[at(y, x)] = static_cast<uint16_t>(window);
      window += sharp[at(y, clampIndex(x + MEAN_RADIUS + 1, width))] - sharp[at(y, clampIndex(x - MEAN_RADIUS, width))];
    }
  }
  for (int x = 0; x < width; x++) {
    int column = 0;
    for (int k = -MEAN_RADIUS; k <= MEAN_RADIUS; k++) column += sums[at(clampIndex(k, height), x)];
    colSum[x] = column;
  }
  for (int i = 0; i < width + 2; i++) {
    errCur[i] = 0;
    errNext[i] = 0;
  }

  // 4. Decide every pixel: threshold near edges, Floyd-Steinberg (serpentine) elsewhere. The error diffusion runs over
  //    the whole page, edge pixels included, so the two regions blend without a seam.
  for (int y = 0; y < height; y++) {
    const int step = (y & 1) ? -1 : 1;
    for (int i = (y & 1) ? width - 1 : 0; i >= 0 && i < width; i += step) {
      const int x = i;
      const int value = stretchTone(toneLut[sharp[at(y, x)]]) + errCur[x + 1];
      const int black = value >= 128 ? 255 : 0;
      const int err = value - black;
      const int q7 = (err * 7) >> 4;
      const int q3 = (err * 3) >> 4;
      const int q5 = (err * 5) >> 4;
      errCur[x + 1 + step] += q7;
      errNext[x + 1 - step] += q3;
      errNext[x + 1] += q5;
      errNext[x + 1 + step] += err - q7 - q3 - q5;

      if (edge[at(y, x)]) {
        const int mean = (colSum[x] + MEAN_AREA / 2) / MEAN_AREA;
        const int threshold = std::min((mean * 9) / 10, THRESHOLD_CAP);
        out[at(y, x)] = sharp[at(y, x)] >= threshold ? 3 : 0;
      } else {
        out[at(y, x)] = black ? 3 : 0;
      }
    }
    for (int i = 0; i < width + 2; i++) {
      errCur[i] = errNext[i];
      errNext[i] = 0;
    }
    // Roll the window of column sums down one row: add row y + 7, drop row y - 6 (rows beyond the page repeat its edge).
    for (int x = 0; x < width; x++) {
      colSum[x] += sums[at(clampIndex(y + MEAN_RADIUS + 1, height), x)] - sums[at(clampIndex(y - MEAN_RADIUS, height), x)];
    }
  }
  return true;
}

uint8_t* allocFrame(const size_t bytes) { return static_cast<uint8_t*>(allocTemp(bytes)); }

void freeFrame(uint8_t* frame) { free(frame); }

}  // namespace ComicsHalftone
