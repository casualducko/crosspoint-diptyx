#include "ContentBounds.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ContentBounds {

namespace {

constexpr int MIN_PAPER = 150;  // a page whose bright level is lower than this is dark: no trimming
constexpr int INK_BELOW_PAPER = 45;
constexpr int MIN_INK_LEVEL = 60;

// First and last index whose count reaches `need` with a neighbour that does too; false when there is none.
bool span(const std::vector<uint16_t>& counts, const int need, int& first, int& last) {
  const int n = static_cast<int>(counts.size());
  first = -1;
  last = -1;
  for (int i = 0; i < n; i++) {
    if (counts[i] >= need && ((i + 1 < n && counts[i + 1] >= need) || (i > 0 && counts[i - 1] >= need))) {
      if (first < 0) first = i;
      last = i;
    }
  }
  return first >= 0;
}

}  // namespace

bool find(const uint8_t* gray, const int width, const int height, int& x0, int& y0, int& x1, int& y1) {
  if (!gray || width < 8 || height < 8) return false;

  uint32_t histogram[256] = {};
  const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
  for (size_t i = 0; i < total; i++) histogram[gray[i]]++;
  const uint32_t target = static_cast<uint32_t>(total - total / 10);  // the 90th percentile
  uint32_t seen = 0;
  int paper = 255;
  for (int v = 0; v < 256; v++) {
    seen += histogram[v];
    if (seen >= target) {
      paper = v;
      break;
    }
  }
  if (paper < MIN_PAPER) return false;
  const int inkBelow = std::max(MIN_INK_LEVEL, paper - INK_BELOW_PAPER);

  std::vector<uint16_t> rows(static_cast<size_t>(height), 0);
  std::vector<uint16_t> cols(static_cast<size_t>(width), 0);
  for (int y = 0; y < height; y++) {
    const uint8_t* row = gray + static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int x = 0; x < width; x++) {
      if (row[x] < inkBelow) {
        rows[y]++;
        cols[x]++;
      }
    }
  }

  int firstRow, lastRow, firstCol, lastCol;
  if (!span(rows, std::max(2, width / 200), firstRow, lastRow)) return false;
  if (!span(cols, std::max(2, height / 200), firstCol, lastCol)) return false;

  const int pad = std::max(2, std::min(width, height) / 60);
  x0 = std::max(0, firstCol - pad);
  y0 = std::max(0, firstRow - pad);
  x1 = std::min(width, lastCol + 1 + pad);
  y1 = std::min(height, lastRow + 1 + pad);
  return x1 > x0 && y1 > y0;
}

}  // namespace ContentBounds
