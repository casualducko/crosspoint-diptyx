#include "ImageBlock.h"

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "../../../../src/CrossPointSettings.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

bool prefetching = false;  // ImageBlock::setPrefetch()
const std::atomic<bool>* prefetchStop = nullptr;

bool prefetchShouldStop() { return prefetching && prefetchStop && prefetchStop->load(); }

void preservePolarity(GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  if (!prefetching) renderer.preserveImagePolarity(x, y, w, h);
}

// Trim Margins: the part of a page worth showing, kept next to the image (.bbx) so the page is measured once.
struct TrimRect {
  uint16_t x, y, w, h;  // source pixels; w == 0: the page is shown whole
};

std::string getTrimPath(const std::string& imagePath) {
  const size_t dotPos = imagePath.rfind('.');
  return (dotPos != std::string::npos ? imagePath.substr(0, dotPos) : imagePath) + ".bbx";
}

bool readTrim(const std::string& path, TrimRect& out) {
  HalFile file;
  if (!Storage.openFileForRead("IMG", path, file)) return false;
  return file.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

void writeTrim(const std::string& path, const TrimRect& rect) {
  HalFile file;
  if (Storage.openFileForWrite("IMG", path, file)) file.write(&rect, sizeof(rect));
}

std::string getCachePath(const std::string& imagePath, const char* suffix = "") {
  // Replace extension with .pxc (pixel cache). The Diptyx caches its 1-bit result as .pxe<mode>: the extension changes
  // whenever the dither or the downscale changes, and carries the Image Dithering choice, so a cache written by an
  // earlier dither or another mode is never reused.
  std::string ext = ".pxc";
  if (BoardConfig::isDiptyx()) ext = std::string(".pxe") + static_cast<char>('0' + (SETTINGS.imageDither % 10));
  ext += suffix;  // w: double-page picture at two screens' width; t: Trim Margins; z: Zoom (zt: both)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ext;
  }
  return imagePath + ext;
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Suppress repeated failures across the BW/grayscale passes of one page render.
// Clear before the next page render so transient memory/storage failures retry.
constexpr size_t MAX_RENDER_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_RENDER_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisRender(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_RENDER_IMAGE_FAILURES || imageFailedThisRender(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// The tiled grayscale flow re-renders an image page once for the BW
// double-refresh and again for every band of both gray planes, and each pass
// re-read the whole .pxc off SD (~100 ms for a full-page image, ~13 passes).
// Column clipping cannot reduce the SD traffic: the row stride (~100 B) is
// smaller than an SD sector, so every sector is touched regardless of the band
// window. Instead the first pass loads the payload into RAM and later passes
// render from it. Chunked allocation because a single full-image block (up to
// 96 KB) rarely fits the fragmented mid-render heap; each chunk is heap-gated
// and any failure falls back to the streaming path unchanged. The reader
// releases the slot when the page render completes, so nothing stays resident
// across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB: a full-screen 2bpp image
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds the
// full pixel payload for this cache path afterward.
bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) {
    return false;
  }
  size_t remaining = (size_t)bytesPerRow * cachedHeight;
  const size_t chunkCount = (remaining + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return false;
  }
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
    if (ESP.getFreeHeap() < remaining + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) {
      releasePxcSlot();
      return false;
    }
    pxcChunks[i] = makeUniqueNoThrow<uint8_t[]>(want);
    if (!pxcChunks[i] || cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return false;
    }
    remaining -= want;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y, const int clipX0, const int clipX1) {
  const int bytesPerRow = (pxcSlotWidth + 3) / 4;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotHeight; row++) {
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    colStart = std::max(colStart, clipX0 - x);
    colEnd = std::min(colEnd, clipX1 - x);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      pw.writePixel(x + col, pixelValue);
    }
  }
}

// Draws the cached image with its top left at (x, y); only the columns with clipX0 <= screen x < clipX1 are written.
bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, const int clipX0 = INT16_MIN, const int clipX1 = INT16_MAX,
                     const int rowStart = 0, const int rowEnd = INT16_MAX) {
  if (prefetching) {  // nothing to draw: only say whether a valid cache is there
    HalFile file;
    uint16_t w, h;
    return Storage.openFileForRead("IMG", cachePath, file) &&
           readValidCacheHeader(file, expectedWidth, expectedHeight, w, h);
  }
  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  const bool windowed = rowStart > 0 || rowEnd < INT16_MAX;  // a window of rows streams from the file
  if (!windowed && pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    renderRowsFromPxcSlot(renderer, x, y, clipX0, clipX1);
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  if (!windowed && pxcSlotHash == 0 && loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, x, y, clipX0, clipX1);
    LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
    return true;
  }

  // Streaming fallback (slot didn't fit). A failed slot load may have consumed
  // part of the payload; rewind to just past the header (or to the window's first row).
  const int firstRow = std::max(0, rowStart);
  const int lastRow = std::min<int>(cachedHeight, rowEnd);
  if (firstRow >= lastRow) return true;
  cacheFile.seek(4 + static_cast<size_t>(firstRow) * bytesPerRow);

  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = firstRow; row < lastRow; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (lastRow - row < rowsPerRead) ? (lastRow - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    colStart = std::max(colStart, clipX0 - x);
    colEnd = std::min(colEnd, clipX1 - x);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisRender(imagePath) && !hasValidCache(); }

void ImageBlock::clearRenderFailures() { failedImageCount = 0; }

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  if (prefetching) return;
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

bool ImageBlock::ensureExtracted() const {
  if (srcPath.empty() || !extractFn || Storage.exists(imagePath.c_str())) return true;
  LOG_DBG("IMG", "Lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
  if (!extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) {
    LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
    return false;
  }
  return true;
}

namespace {
int splitHalf = -1;
int splitViewX = 0, splitViewY = 0, splitViewW = 0, splitViewH = 0;
}  // namespace

void ImageBlock::setPrefetch(const bool on) { prefetching = on; }

void ImageBlock::setPrefetchStop(const std::atomic<bool>* stop) { prefetchStop = stop; }

void ImageBlock::setSplitView(const int half, const int viewX, const int viewY, const int viewW, const int viewH) {
  splitHalf = half;
  splitViewX = viewX;
  splitViewY = viewY;
  splitViewW = viewW;
  splitViewH = viewH;
}

// The picture is scaled to fit two content areas side by side and centered; the screen shows the half that is next to
// the hinge: the left screen the picture's left half, flush with the content area's right edge, the right screen the
// other half, flush with the left edge.
void ImageBlock::renderSplit(GfxRenderer& renderer) {
  const int wideH = std::min<int>(splitViewH, (2 * splitViewW * height) / width);
  const int wideW = std::min<int>(2 * splitViewW, (wideH * width) / height);
  if (wideW < 2 || wideH < 1) return;
  const int half = wideW / 2;
  const int y = splitViewY + (splitViewH - wideH) / 2;
  const int x = splitHalf == 0 ? splitViewX + splitViewW - half : splitViewX - half;
  const int clipX0 = splitHalf == 0 ? x : splitViewX;
  const int clipX1 = splitHalf == 0 ? splitViewX + splitViewW : splitViewX + (wideW - half);

  if (!renderer.glyphIntersectsStrip(clipX0, y, clipX1 - 1, y + wideH - 1)) return;
  if (imageFailedThisRender(imagePath)) return;

  const std::string cachePath = getCachePath(imagePath, "w");
  if (!renderFromCache(renderer, cachePath, x, y, wideW, wideH, clipX0, clipX1)) {
    ensureExtracted();
    if (prefetchShouldStop()) return;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
    if (!decoder) {
      LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      return;
    }
    RenderConfig config;
    config.x = 0;  // the picture is decoded at a virtual origin: it may start off the screen
    config.y = 0;
    config.maxWidth = wideW;
    config.maxHeight = wideH;
    config.useGrayscale = true;
    config.useDithering = true;
    config.ditherMode = SETTINGS.imageDither;
    config.useExactDimensions = true;
    config.cachePath = cachePath;
    config.cacheOnly = true;
    LOG_DBG("IMG", "Decoding the double-page picture %dx%d: %s", wideW, wideH, imagePath.c_str());
    if (!decoder->decodeToFramebuffer(imagePath, renderer, config) ||
        !renderFromCache(renderer, cachePath, x, y, wideW, wideH, clipX0, clipX1)) {
      LOG_ERR("IMG", "Failed to draw the double-page picture: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      return;
    }
  }
  preservePolarity(renderer, clipX0, y, clipX1 - clipX0, wideH);
}

// Trim Margins: the part of the page worth showing, read from the .bbx file or measured once. False when the setting is
// off, the format cannot be measured, or there is little border to cut: the page is shown whole.
bool ImageBlock::contentRect(int& rx, int& ry, int& rw, int& rh) const {
  rx = ry = rw = rh = 0;
  if (!BoardConfig::isDiptyx() || !SETTINGS.trimMargins) return false;
  const std::string trimPath = getTrimPath(imagePath);
  TrimRect rect;
  if (!readTrim(trimPath, rect)) {
    ensureExtracted();
    if (prefetchShouldStop()) return false;
    rect = {0, 0, 0, 0};
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
    ImageDimensions dims;
    int x0, y0, x1, y1;
    if (decoder && decoder->getDimensions(imagePath, dims) && decoder->findContentBounds(imagePath, x0, y0, x1, y1)) {
      // Worth it only when it cuts at least a tenth of the page and leaves a real picture.
      const int64_t full = static_cast<int64_t>(dims.width) * dims.height;
      const int64_t kept = static_cast<int64_t>(x1 - x0) * (y1 - y0);
      if (kept * 10 <= full * 9 && (x1 - x0) * 5 >= dims.width * 2 && (y1 - y0) * 5 >= dims.height * 2) {
        rect = {static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1 - x0),
                static_cast<uint16_t>(y1 - y0)};
      }
    }
    writeTrim(trimPath, rect);
  }
  if (rect.w == 0 || rect.h == 0) return false;
  rx = rect.x;
  ry = rect.y;
  rw = rect.w;
  rh = rect.h;
  return true;
}

// Trim Margins: shows the part of the page that has content, as large as the page's box allows. False when the page is
// to be shown whole.
bool ImageBlock::renderTrimmed(GfxRenderer& renderer, const int x, const int y) {
  TrimRect rect;
  int rx, ry, rw, rh;
  if (!contentRect(rx, ry, rw, rh)) return false;
  rect = {static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), static_cast<uint16_t>(rw), static_cast<uint16_t>(rh)};

  // Fit the content into this page's box, centered.
  int outW = width;
  int outH = (static_cast<int>(rect.h) * width) / rect.w;
  if (outH > height) {
    outH = height;
    outW = (static_cast<int>(rect.w) * height) / rect.h;
  }
  if (outW < 1 || outH < 1) return false;
  const int outX = x + (width - outW) / 2;
  const int outY = y + (height - outH) / 2;

  if (!renderer.glyphIntersectsStrip(outX, outY, outX + outW - 1, outY + outH - 1)) return true;
  if (imageFailedThisRender(imagePath)) return false;

  const std::string cachePath = getCachePath(imagePath, "t");
  if (renderFromCache(renderer, cachePath, outX, outY, outW, outH)) {
    preservePolarity(renderer, outX, outY, outW, outH);
    return true;
  }
  ensureExtracted();
  if (prefetchShouldStop()) return true;  // nothing to fall back to: the real render does the rest
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) return false;
  RenderConfig config;
  config.x = outX;
  config.y = outY;
  config.maxWidth = outW;
  config.maxHeight = outH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.ditherMode = SETTINGS.imageDither;
  config.useExactDimensions = true;
  config.cachePath = cachePath;
  config.cacheOnly = prefetching;
  config.cropX = rect.x;
  config.cropY = rect.y;
  config.cropW = rect.w;
  config.cropH = rect.h;
  LOG_DBG("IMG", "Decoding the trimmed page %dx%d from %u,%u %ux%u: %s", outW, outH, rect.x, rect.y, rect.w, rect.h,
          imagePath.c_str());
  if (!decoder->decodeToFramebuffer(imagePath, renderer, config)) {
    LOG_ERR("IMG", "Failed to decode the trimmed page: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    return false;
  }
  preservePolarity(renderer, outX, outY, outW, outH);
  return true;
}

namespace {
int zoomHalf = -1;
int zoomCol = 0;
int zoomRow = 0;
int zoomViewX = 0, zoomViewY = 0, zoomViewW = 0, zoomViewH = 0;
int zoomPan = 0;
}  // namespace

void ImageBlock::setZoomView(const int half, const int col, const int row, const int viewX, const int viewY,
                             const int viewW, const int viewH) {
  zoomHalf = half;
  zoomCol = col;
  zoomRow = row;
  zoomViewX = viewX;
  zoomViewY = viewY;
  zoomViewW = viewW;
  zoomViewH = viewH;
}

// The picture is fitted into a canvas of two content areas wide and two tall (a double-page picture: four wide, so that
// each page of it gets the size a single page gets); when that leaves it no taller than one area (plus an eighth) it is
// shrunk to one row, otherwise it needs two (top and bottom). Narrower than two areas, it needs one column.
ImageBlock::ZoomLayout ImageBlock::computeZoom(const int viewW, const int viewH) const {
  ZoomLayout z{};
  int rx, ry, rw, rh;
  const bool cropped = contentRect(rx, ry, rw, rh);
  if (cropped) {
    z.cropX = rx;
    z.cropY = ry;
    z.cropW = rw;
    z.cropH = rh;
  }
  const int srcW = cropped ? rw : std::max<int>(width, 1);  // the block's size gives the aspect when the page is whole
  const int srcH = cropped ? rh : std::max<int>(height, 1);
  const int canvasW = (srcW * 5 >= srcH * 6 ? 4 : 2) * viewW;
  // The views overlap by a sixteenth of a screen, so a line of text at a seam is whole in one of them.
  const int canvasH = 2 * viewH - viewH / 16;
  int zw = canvasW;
  int zh = (canvasW * srcH) / srcW;
  if (zh > canvasH) {
    zh = canvasH;
    zw = (canvasH * srcW) / srcH;
  }
  z.rows = 2;
  if (zh <= viewH + viewH / 8) {
    if (zh > viewH) {
      zw = (zw * viewH) / zh;
      zh = viewH;
    }
    z.rows = 1;
  }
  z.cols = zw > 2 * viewW ? 2 : 1;
  z.zoomW = std::max(zw, 2);
  z.zoomH = std::max(zh, 1);
  return z;
}

void ImageBlock::setZoomPan(const int pan) { zoomPan = pan; }

// The window of the two screens may move within the picture; a picture narrower than the two screens may be moved by
// half a screen either way.
void ImageBlock::zoomPanRange(const int viewW, const int viewH, const int col, int& lo, int& hi) const {
  const ZoomLayout z = computeZoom(viewW, viewH);
  if (z.cols == 1) {
    lo = -viewW / 2;
    hi = viewW / 2;
    return;
  }
  const int maxWindow = z.zoomW - 2 * viewW;
  const int base = std::min(col, z.cols - 1) == 0 ? 0 : maxWindow;
  lo = -base;
  hi = maxWindow - base;
}

void ImageBlock::zoomViews(const int viewW, const int viewH, int& cols, int& rows) const {
  const ZoomLayout z = computeZoom(viewW, viewH);
  cols = z.cols;
  rows = z.rows;
}

void ImageBlock::renderZoomed(GfxRenderer& renderer) {
  const ZoomLayout z = computeZoom(zoomViewW, zoomViewH);
  if (prefetchShouldStop()) return;
  // Where the two screens' window starts on the picture: with one column the picture is centered on the hinge, with two
  // the right column is the right-hand end (the columns overlap when the picture is narrower than four areas).
  const int col = std::min(zoomCol, z.cols - 1);
  const int windowX = z.cols == 1 ? z.zoomW / 2 - zoomViewW : (col == 0 ? 0 : z.zoomW - 2 * zoomViewW);
  const int x = zoomViewX - windowX - zoomPan - (zoomHalf == 0 ? 0 : zoomViewW);
  const int clipX0 = std::max(zoomViewX, x);
  const int clipX1 = std::min(zoomViewX + zoomViewW, x + z.zoomW);
  int winStart = 0;
  int winEnd = z.zoomH;
  int y = zoomViewY + (zoomViewH - z.zoomH) / 2;
  if (z.rows == 2) {  // the second view is the bottom of the picture; the two overlap when it is short
    winStart = zoomRow == 0 ? 0 : z.zoomH - zoomViewH;
    winEnd = winStart + zoomViewH;
    y = zoomViewY - winStart;
  }
  if (clipX0 >= clipX1) return;
  if (!renderer.glyphIntersectsStrip(clipX0, y + winStart, clipX1 - 1, y + winEnd - 1)) return;
  if (imageFailedThisRender(imagePath)) return;

  const std::string cachePath = getCachePath(imagePath, z.cropW > 0 ? "zt" : "z");
  if (!renderFromCache(renderer, cachePath, x, y, z.zoomW, z.zoomH, clipX0, clipX1, winStart, winEnd)) {
    ensureExtracted();
    if (prefetchShouldStop()) return;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
    if (!decoder) {
      LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      return;
    }
    RenderConfig config;
    config.x = 0;  // decoded at a virtual origin: the picture is larger than a screen
    config.y = 0;
    config.maxWidth = z.zoomW;
    config.maxHeight = z.zoomH;
    config.useGrayscale = true;
    config.useDithering = true;
    config.ditherMode = SETTINGS.imageDither;
    config.useExactDimensions = true;
    config.cachePath = cachePath;
    config.cacheOnly = true;
    config.cropX = z.cropX;
    config.cropY = z.cropY;
    config.cropW = z.cropW;
    config.cropH = z.cropH;
    LOG_DBG("IMG", "Decoding the zoomed page %dx%d (%d view%s): %s", z.zoomW, z.zoomH, z.rows, z.rows == 1 ? "" : "s",
            imagePath.c_str());
    if (!decoder->decodeToFramebuffer(imagePath, renderer, config) ||
        !renderFromCache(renderer, cachePath, x, y, z.zoomW, z.zoomH, clipX0, clipX1, winStart, winEnd)) {
      LOG_ERR("IMG", "Failed to draw the zoomed page: %s", imagePath.c_str());
      rememberImageFailure(imagePath);
      return;
    }
  }
  preservePolarity(renderer, clipX0, y + winStart, clipX1 - clipX0, winEnd - winStart);
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  if (zoomHalf >= 0) {
    renderZoomed(renderer);
    return;
  }
  if (splitHalf >= 0 && isWide()) {
    renderSplit(renderer);
    return;
  }
  if (BoardConfig::isDiptyx() && SETTINGS.trimMargins && renderTrimmed(renderer, x, y)) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisRender(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    preservePolarity(renderer, x, y, width, height);
    return;  // Successfully rendered from cache
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  ensureExtracted();
  if (prefetchShouldStop()) return;

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.ditherMode = SETTINGS.imageDither;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode
  config.cacheOnly = prefetching;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  preservePolarity(renderer, x, y, width, height);
  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
}
