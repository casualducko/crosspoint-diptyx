#include "MarkAsRead.h"

#include <HalStorage.h>
#include <Logging.h>

#include <functional>
#include <iterator>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
constexpr char READ_FOLDER[] = "/read";
// Any spine index past the last chapter means "finished": the reader clamps it to the chapter count.
constexpr uint16_t FINISHED_SPINE_INDEX = 0x7FFF;

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

std::string cachePathFor(const std::string& bookPath) {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
}

// progress.bin as the reader loads it: spine index and next page, 16 bit little endian each.
bool writeFinishedProgress(const std::string& bookPath) {
  const std::string cache = cachePathFor(bookPath);
  if (!Storage.exists(cache.c_str()) && !Storage.mkdir(cache.c_str())) {
    LOG_ERR("MAR", "cannot create %s", cache.c_str());
    return false;
  }
  HalFile file;
  if (!Storage.openFileForWrite("MAR", cache + "/progress.bin", file)) return false;
  const uint8_t data[4] = {static_cast<uint8_t>(FINISHED_SPINE_INDEX & 0xFF),
                           static_cast<uint8_t>(FINISHED_SPINE_INDEX >> 8), 0, 0};
  return file.write(data, sizeof(data)) == sizeof(data);
}

std::string readFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) return dstPath;

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Same steps as the reader's move of a finished book: the book, its protected-book sidecars (rolled back together if
// one fails), its cache folder, and the Recents and open-book paths.
bool moveToReadFolder(const std::string& srcPath) {
  const std::string dstPath = readFolderDestination(srcPath);
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("MAR", "cannot move %s to the Read folder", srcPath.c_str());
    return false;
  }

  static constexpr const char* SIDECARS[] = {".key", ".rights"};
  for (size_t i = 0; i < std::size(SIDECARS); i++) {
    const std::string from = srcPath + SIDECARS[i];
    if (!Storage.exists(from.c_str())) continue;
    if (Storage.rename(from.c_str(), (dstPath + SIDECARS[i]).c_str())) continue;
    LOG_ERR("MAR", "cannot move sidecar %s", from.c_str());
    for (size_t j = 0; j < i; j++) {
      Storage.rename((dstPath + SIDECARS[j]).c_str(), (srcPath + SIDECARS[j]).c_str());
    }
    if (!Storage.rename(dstPath.c_str(), srcPath.c_str())) LOG_ERR("MAR", "cannot restore %s", srcPath.c_str());
    return false;
  }

  const std::string oldCache = cachePathFor(srcPath);
  const std::string newCache = cachePathFor(dstPath);
  if (Storage.exists(oldCache.c_str()) && !Storage.rename(oldCache.c_str(), newCache.c_str())) {
    LOG_ERR("MAR", "cannot rename cache %s (non-fatal)", oldCache.c_str());
  }
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCache, newCache);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
  return true;
}
}  // namespace

MarkAsReadResult markBookAsRead(const std::string& path) {
  MarkAsReadResult result;
  if (!writeFinishedProgress(path)) return result;
  result.ok = true;

  if (SETTINGS.removeReadBooksFromRecents && RECENT_BOOKS.removeByPath(path)) RECENT_BOOKS.saveToFile();
  if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(path)) result.moved = moveToReadFolder(path);
  return result;
}
