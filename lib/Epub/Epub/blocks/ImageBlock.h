#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearRenderFailures();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);

  // Diptyx two-page spread: a double-page picture (wider than tall) is drawn across both screens, its left half on the
  // left one and its right half on the right one. The reader sets the half (0 left, 1 right) and the content area of
  // one screen before it renders such a page, and clears it (half -1) afterwards.
  // Idle preparation of a page that is not on the screen yet: while set, render() draws nothing and decodes a missing
  // picture into its pixel cache only, so the real render finds it there.
  static void setPrefetch(bool on);
  static void setSplitView(int half, int viewX, int viewY, int viewW, int viewH);
  bool isWide() const { return width * 5 >= height * 6; }
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty once known-extracted
  int16_t width;
  int16_t height;

  void renderSplit(GfxRenderer& renderer);
  bool renderTrimmed(GfxRenderer& renderer, int x, int y);
  bool ensureExtracted();

  static void* extractCtx;
  static ExtractFn extractFn;
};
