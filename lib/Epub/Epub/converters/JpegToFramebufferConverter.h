#pragma once

#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  bool findContentBounds(const std::string& imagePath, int& x0, int& y0, int& x1, int& y1) const override;

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "JPEG"; }
};
