#include "ComicsHalftone.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif

// The firmware builds at -Os; this file is a few hot loops over a whole page (over a second at -Os), so it is built for
// speed instead.
#pragma GCC optimize("O3")

namespace {

// Microseconds spent in each stage of the last process() call (zero on a host), for the log.
uint32_t gStageMicros[ComicsHalftone::STAGE_COUNT] = {};

#ifdef ESP_PLATFORM
#define STAGE_MARK(i)                                            \
  do {                                                           \
    const int64_t markNow = esp_timer_get_time();                \
    gStageMicros[i] = static_cast<uint32_t>(markNow - markLast); \
    markLast = markNow;                                          \
  } while (0)
#else
#define STAGE_MARK(i) \
  do {                \
  } while (0)
#endif

// Large temporaries (a byte or two per pixel) go to PSRAM, internal RAM is the fallback, and plain malloc serves a host
// (the unit test). Small ones (the error-diffusion rows, the column sums) are touched in the innermost loops, where
// they would fight the streaming page buffers for the PSRAM cache, so they come from internal RAM first. Everything is
// released with free(), which also takes heap_caps_malloc() memory.
void* allocTemp(const size_t bytes) {
#ifdef ESP_PLATFORM
  constexpr size_t SMALL_BYTES = 16 * 1024;
  if (bytes <= SMALL_BYTES) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) return p;
  }
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

// Visits every x in [0, w): edge(x) within `margin` of either side (where a window would leave the row), interior(x)
// elsewhere, so the interior loop carries no clamping.
template <typename Interior, typename Edge>
inline void forRow(const int w, const int margin, Interior&& interior, Edge&& edge) {
  const int lo = std::min(margin, w);
  const int hi = std::max(w - margin, lo);
  for (int x = 0; x < lo; x++) edge(x);
  for (int x = lo; x < hi; x++) interior(x);
  for (int x = hi; x < w; x++) edge(x);
}

}  // namespace

namespace ComicsHalftone {

bool process(const uint8_t* gray, const int width, const int height, const uint8_t* toneLut, uint8_t* out) {
  if (!gray || !toneLut || !out || width <= 0 || height <= 0) return false;
  const int w = width;
  const int h = height;
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  auto sharp = makeTemp<uint8_t>(pixels);
  auto edge = makeTemp<uint8_t>(pixels);
  auto spare = makeTemp<uint8_t>(pixels);
  auto sums = makeTemp<uint16_t>(pixels);
  auto colSum = makeTemp<int>(static_cast<size_t>(w));
  auto errCur = makeTemp<int>(static_cast<size_t>(w) + 2);
  auto errNext = makeTemp<int>(static_cast<size_t>(w) + 2);
  if (!sharp || !edge || !spare || !sums || !colSum || !errCur || !errNext) return false;

#ifdef ESP_PLATFORM
  int64_t markLast = esp_timer_get_time();
#endif
  const auto rowOf = [w](auto* base, const int y) { return base + static_cast<size_t>(y) * static_cast<size_t>(w); };
  const auto rowAt = [&](auto* base, const int y) { return rowOf(base, clampIndex(y, h)); };

  // 1. Sharpen: S = clamp(2 * G - blur(G)), blur = [1 4 6 4 1] x [1 4 6 4 1] / 256, rounded.
  for (int y = 0; y < h; y++) {
    const uint8_t* g = rowOf(gray, y);
    uint16_t* t = rowOf(sums.get(), y);
    forRow(
        w, 2,
        [&](const int x) {
          t[x] = static_cast<uint16_t>(g[x - 2] + 4 * g[x - 1] + 6 * g[x] + 4 * g[x + 1] + g[x + 2]);
        },
        [&](const int x) {
          t[x] = static_cast<uint16_t>(g[clampIndex(x - 2, w)] + 4 * g[clampIndex(x - 1, w)] + 6 * g[x] +
                                       4 * g[clampIndex(x + 1, w)] + g[clampIndex(x + 2, w)]);
        });
  }
  for (int y = 0; y < h; y++) {
    const uint16_t* r0 = rowAt(sums.get(), y - 2);
    const uint16_t* r1 = rowAt(sums.get(), y - 1);
    const uint16_t* r2 = rowOf(sums.get(), y);
    const uint16_t* r3 = rowAt(sums.get(), y + 1);
    const uint16_t* r4 = rowAt(sums.get(), y + 2);
    const uint8_t* g = rowOf(gray, y);
    uint8_t* s = rowOf(sharp.get(), y);
    for (int x = 0; x < w; x++) {
      const int blur = (r0[x] + 4 * r1[x] + 6 * r2[x] + 4 * r3[x] + r4[x] + 128) >> 8;
      s[x] = static_cast<uint8_t>(std::clamp(2 * g[x] - blur, 0, 255));
    }
  }

  STAGE_MARK(0);

  // 2. Edge mask: a pixel whose 5x5 neighbourhood of the (unsharpened) page spans more than EDGE_CONTRAST, then grown
  //    by one pixel in every direction.
  for (int y = 0; y < h; y++) {
    const uint8_t* g = rowOf(gray, y);
    uint16_t* t = rowOf(sums.get(), y);
    forRow(
        w, 2,
        [&](const int x) {
          const int hi = std::max({g[x - 2], g[x - 1], g[x], g[x + 1], g[x + 2]});
          const int lo = std::min({g[x - 2], g[x - 1], g[x], g[x + 1], g[x + 2]});
          t[x] = static_cast<uint16_t>((hi << 8) | lo);
        },
        [&](const int x) {
          int hi = 0;
          int lo = 255;
          for (int k = -2; k <= 2; k++) {
            const int v = g[clampIndex(x + k, w)];
            hi = std::max(hi, v);
            lo = std::min(lo, v);
          }
          t[x] = static_cast<uint16_t>((hi << 8) | lo);
        });
  }
  for (int y = 0; y < h; y++) {
    const uint16_t* r[5] = {rowAt(sums.get(), y - 2), rowAt(sums.get(), y - 1), rowOf(sums.get(), y),
                            rowAt(sums.get(), y + 1), rowAt(sums.get(), y + 2)};
    uint8_t* e = rowOf(edge.get(), y);
    for (int x = 0; x < w; x++) {
      int hi = 0;
      int lo = 255;
      for (int k = 0; k < 5; k++) {
        hi = std::max(hi, r[k][x] >> 8);
        lo = std::min(lo, r[k][x] & 0xFF);
      }
      e[x] = (hi - lo) > EDGE_CONTRAST ? 1 : 0;
    }
  }
  for (int y = 0; y < h; y++) {
    const uint8_t* e = rowOf(edge.get(), y);
    uint8_t* d = rowOf(spare.get(), y);
    forRow(
        w, 1, [&](const int x) { d[x] = e[x - 1] | e[x] | e[x + 1]; },
        [&](const int x) { d[x] = e[clampIndex(x - 1, w)] | e[x] | e[clampIndex(x + 1, w)]; });
  }
  for (int y = 0; y < h; y++) {
    const uint8_t* up = rowAt(spare.get(), y - 1);
    const uint8_t* mid = rowOf(spare.get(), y);
    const uint8_t* down = rowAt(spare.get(), y + 1);
    uint8_t* e = rowOf(edge.get(), y);
    for (int x = 0; x < w; x++) e[x] = up[x] | mid[x] | down[x];
  }

  STAGE_MARK(1);

  // 3. Row sums of the sharpened page over the 13-pixel window, for the local mean (the column sums are rolled below).
  for (int y = 0; y < h; y++) {
    const uint8_t* s = rowOf(sharp.get(), y);
    uint16_t* t = rowOf(sums.get(), y);
    int window = 0;
    for (int k = -MEAN_RADIUS; k <= MEAN_RADIUS; k++) window += s[clampIndex(k, w)];
    for (int x = 0; x < w; x++) {
      t[x] = static_cast<uint16_t>(window);
      window += s[std::min(x + MEAN_RADIUS + 1, w - 1)] - s[std::max(x - MEAN_RADIUS, 0)];
    }
  }
  for (int x = 0; x < w; x++) colSum[x] = 0;
  for (int k = -MEAN_RADIUS; k <= MEAN_RADIUS; k++) {
    const uint16_t* t = rowAt(sums.get(), k);
    for (int x = 0; x < w; x++) colSum[x] += t[x];
  }
  for (int i = 0; i < w + 2; i++) {
    errCur[i] = 0;
    errNext[i] = 0;
  }

  STAGE_MARK(2);

  // The tone curve and stretch folded into one table: stretchTone(toneLut[v]).
  int toned[256];
  for (int v = 0; v < 256; v++) toned[v] = stretchTone(toneLut[v]);

  // 4. Decide every pixel: threshold near edges, Floyd-Steinberg (serpentine) elsewhere. The error diffusion runs over
  //    the whole page, edge pixels included, so the two regions blend without a seam.
  for (int y = 0; y < h; y++) {
    const uint8_t* s = rowOf(sharp.get(), y);
    const uint8_t* e = rowOf(edge.get(), y);
    uint8_t* o = rowOf(out, y);
    const int step = (y & 1) ? -1 : 1;
    int* cur = errCur.get() + 1;
    int* next = errNext.get() + 1;
    for (int i = (y & 1) ? w - 1 : 0; i >= 0 && i < w; i += step) {
      const int x = i;
      const int value = toned[s[x]] + cur[x];
      const int black = value >= 128 ? 255 : 0;
      const int err = value - black;
      const int q7 = (err * 7) >> 4;
      const int q3 = (err * 3) >> 4;
      const int q5 = (err * 5) >> 4;
      cur[x + step] += q7;
      next[x - step] += q3;
      next[x] += q5;
      next[x + step] += err - q7 - q3 - q5;

      if (e[x]) {
        const int mean = (colSum[x] + MEAN_AREA / 2) / MEAN_AREA;
        const int threshold = std::min((mean * 9) / 10, THRESHOLD_CAP);
        o[x] = s[x] >= threshold ? 3 : 0;
      } else {
        o[x] = black ? 3 : 0;
      }
    }
    for (int i = 0; i < w + 2; i++) {
      errCur[i] = errNext[i];
      errNext[i] = 0;
    }
    // Roll the window of column sums down one row: add row y + 7, drop row y - 6 (rows beyond the page repeat its
    // edge).
    const uint16_t* add = rowAt(sums.get(), y + MEAN_RADIUS + 1);
    const uint16_t* drop = rowAt(sums.get(), y - MEAN_RADIUS);
    for (int x = 0; x < w; x++) colSum[x] += add[x] - drop[x];
  }
  STAGE_MARK(3);
  return true;
}

bool processBanded(uint8_t* gray, const int width, const int height, const uint8_t* toneLut,
                   const size_t maxBandPixels) {
  constexpr int HALO = 8;  // sharpen 2 + 13x13 mean 6
  if (!gray || !toneLut || width <= 0 || height <= 0) return false;
  const size_t w = static_cast<size_t>(width);
  const int bandRows = std::max<int>(HALO * 2, static_cast<int>(maxBandPixels / w) - 2 * HALO);
  const size_t maxRows = static_cast<size_t>(bandRows) + 2 * HALO;
  auto saved = makeTemp<uint8_t>(HALO * w);  // the original rows above the next band
  auto in = makeTemp<uint8_t>(maxRows * w);
  auto out = makeTemp<uint8_t>(maxRows * w);
  if (!saved || !in || !out) return false;

  for (int r0 = 0; r0 < height; r0 += bandRows) {
    const int r1 = std::min(height, r0 + bandRows);
    const int top = r0 > 0 ? HALO : 0;
    const int bottom = std::min(HALO, height - r1);
    if (top) memcpy(in.get(), saved.get(), static_cast<size_t>(top) * w);
    memcpy(in.get() + static_cast<size_t>(top) * w, gray + static_cast<size_t>(r0) * w,
           static_cast<size_t>(r1 - r0 + bottom) * w);
    if (!process(in.get(), width, top + (r1 - r0) + bottom, toneLut, out.get())) return false;
    if (r1 < height) memcpy(saved.get(), gray + static_cast<size_t>(r1 - HALO) * w, HALO * w);
    memcpy(gray + static_cast<size_t>(r0) * w, out.get() + static_cast<size_t>(top) * w,
           static_cast<size_t>(r1 - r0) * w);
  }
  return true;
}

const uint32_t* lastStageMicros() { return gStageMicros; }

uint8_t* allocFrame(const size_t bytes) { return static_cast<uint8_t*>(allocTemp(bytes)); }

void freeFrame(uint8_t* frame) { free(frame); }

}  // namespace ComicsHalftone
