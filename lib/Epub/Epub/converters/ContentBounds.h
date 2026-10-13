#pragma once

#include <cstdint>

// Diptyx "Trim Margins": finds where a scanned comic page has content, so the reader can show that part larger.
namespace ContentBounds {

// gray: width * height 8-bit pixels (a small copy of the page; any scale). Looks for pixels clearly darker than the
// paper (the 90th brightness percentile), keeps rows and columns that hold a run of them (specks of dust do not
// count) and returns the box around them, widened by a small margin, as x0 <= x < x1 and y0 <= y < y1.
// False when the page has no usable border: a dark page, or no content at all.
bool find(const uint8_t* gray, int width, int height, int& x0, int& y0, int& x1, int& y1);

}  // namespace ContentBounds
