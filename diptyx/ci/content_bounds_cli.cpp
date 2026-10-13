// Host harness for lib/Epub/Epub/converters/ContentBounds.cpp (see test_content_bounds.py).
// stdin: "w h\n" then w*h gray bytes. stdout: "x0 y0 x1 y1\n", or "none\n".
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ContentBounds.h"

int main() {
  int w = 0;
  int h = 0;
  if (scanf("%d %d", &w, &h) != 2) return 2;
  getchar();
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  if (fread(gray.data(), 1, gray.size(), stdin) != gray.size()) return 2;
  int x0, y0, x1, y1;
  if (!ContentBounds::find(gray.data(), w, h, x0, y0, x1, y1)) {
    printf("none\n");
    return 0;
  }
  printf("%d %d %d %d\n", x0, y0, x1, y1);
  return 0;
}
