#include "IdleImage.h"

#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <cmath>
#include <string>

#include "CrossPointSettings.h"

namespace idleimage {

bool ensureConverted(const char* tag, const char* jpgPath, const char* bmpPath, const char* keyPath, const uint8_t fit,
                     const int screenWidth, const int screenHeight) {
  if (!Storage.exists(jpgPath)) return false;
  std::string jpgKey;
  {
    HalFile jpg;
    if (!Storage.openFileForRead(tag, jpgPath, jpg)) return false;
    jpgKey = std::to_string(jpg.fileSize()) + ":" + std::to_string(jpg.modificationTime()) + ":" +
             std::to_string(screenWidth) + "x" + std::to_string(screenHeight) + ":" + std::to_string(fit);
  }
  std::string cachedKey;
  const bool haveKey = Storage.exists(keyPath) && Storage.readFileToString(tag, keyPath, 96, cachedKey);
  if (haveKey && cachedKey == jpgKey + ":bad") return false;  // not retried on every sleep until the file changes
  if (haveKey && cachedKey == jpgKey && Storage.exists(bmpPath)) return true;

  Storage.remove(keyPath);
  HalFile jpg;
  HalFile bmp;
  if (!Storage.openFileForRead(tag, jpgPath, jpg) || !Storage.openFileForWrite(tag, bmpPath, bmp)) return false;
  // The decoder needs a lot of heap: a failure with little free is not a verdict on the file, so it is retried next time.
  const bool lowHeap = ESP.getFreeHeap() < 128 * 1024;
  const bool crop = fit == CrossPointSettings::CROP;
  const bool stretch = fit == CrossPointSettings::STRETCH;
  const bool ok = JpegToBmpConverter::jpegFileTo1BitCoverBmpStream(jpg, bmp, crop, stretch);
  bmp.close();
  if (!ok) {
    Storage.remove(bmpPath);
    if (!lowHeap) Storage.writeFile(keyPath, String((jpgKey + ":bad").c_str()));
    LOG_ERR(tag, "Could not convert %s%s", jpgPath, lowHeap ? " (low heap, will retry)" : "");
    return false;
  }
  Storage.writeFile(keyPath, String(jpgKey.c_str()));  // written last: a half-written cache is never trusted
  return true;
}

Placement place(const int bitmapWidth, const int bitmapHeight, const int screenWidth, const int screenHeight,
                const bool crop) {
  Placement p;
  if (bitmapWidth > screenWidth || bitmapHeight > screenHeight) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float screenRatio = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    if (ratio > screenRatio) {
      if (crop) {
        p.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - p.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      p.x = 0;
      const float height = static_cast<float>(screenWidth) / ratio;
      p.y = static_cast<int>(std::round((static_cast<float>(screenHeight) - height) / 2));
    } else {
      if (crop) {
        p.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - p.cropY) * static_cast<float>(bitmapHeight));
      }
      const float width = static_cast<float>(screenHeight) * ratio;
      p.x = static_cast<int>(std::round((static_cast<float>(screenWidth) - width) / 2));
      p.y = 0;
    }
  } else {
    p.x = (screenWidth - bitmapWidth) / 2;
    p.y = (screenHeight - bitmapHeight) / 2;
  }
  return p;
}

}  // namespace idleimage
