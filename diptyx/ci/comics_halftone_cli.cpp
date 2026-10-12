// Host harness for lib/Epub/Epub/converters/ComicsHalftone.cpp (see test_comics_halftone.py).
// stdin: "w h\n", then the 256 tone-LUT values on one line, then w*h gray bytes. stdout: w*h output bytes (0 or 3).
#include <cstdio>
#include <cstdint>
#include <vector>

#include "ComicsHalftone.h"

int main() {
  int w = 0;
  int h = 0;
  if (scanf("%d %d", &w, &h) != 2) return 2;
  std::vector<uint8_t> lut(256);
  for (auto& v : lut) {
    int t = 0;
    if (scanf("%d", &t) != 1) return 2;
    v = static_cast<uint8_t>(t);
  }
  getchar();  // the newline after the LUT
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  if (fread(gray.data(), 1, gray.size(), stdin) != gray.size()) return 2;
  std::vector<uint8_t> out(gray.size());
  if (!ComicsHalftone::process(gray.data(), w, h, lut.data(), out.data())) return 3;
  fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}
