#pragma once

#include <BitmapHelpers.h>
#include <BoardConfig.h>
#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// 8x8 Bayer matrix (0..63) for the 1-bit ordered dither below.
inline const uint8_t bayer8x8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

// Black-and-white panel: tone-curve the gray, stretch 40..224 to 0..255 (the range the home-screen cover's Atkinson
// dither resolves, it clips shadows to black and highlights to white), then threshold against the Bayer matrix. Returns
// 0 (black) or 3 (white), the only two levels the BW pass can show; the 4-level dither above would push every mid-gray
// to black.
inline uint8_t applyBayerDither1Bit(uint8_t gray, int x, int y) {
  const int tone = bwToneCurve(gray);
  const int stretched = tone <= 40 ? 0 : (tone >= 224 ? 255 : ((tone - 40) * 355) >> 8);
  return stretched >= bayer8x8[y & 7][x & 7] * 4 + 2 ? 3 : 0;
}

// The dither for the active panel: 1-bit on the Diptyx, 4 gray levels elsewhere.
inline uint8_t applyPanelDither(uint8_t gray, int x, int y) {
  return BoardConfig::isDiptyx() ? applyBayerDither1Bit(gray, x, y) : applyBayerDither4Level(gray, x, y);
}
