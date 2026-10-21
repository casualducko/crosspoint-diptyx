#include "UnitLinkService.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

UnitLinkService& UnitLinkService::get() {
  static UnitLinkService instance;
  return instance;
}

// The same book on both SD cards has the same file name and size; the path may differ.
uint32_t UnitLinkService::bookIdFor(const std::string& epubPath) {
  if (epubPath.empty()) return 0;
  const size_t slash = epubPath.find_last_of('/');
  const char* name = epubPath.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  uint32_t id = unitlink::fnv1a(name, strlen(name));
  HalFile file;
  if (Storage.openFileForRead("LINK", epubPath, file)) {
    const uint32_t size = static_cast<uint32_t>(file.size());
    id = unitlink::fnv1a(&size, sizeof(size), id);
  }
  return id;
}

// Everything that changes where a page break falls, taken from the same spec the reader builds its section cache from.
uint32_t UnitLinkService::layoutHashFor(const GfxRenderer& renderer) {
  const ReaderRenderSpec spec = SETTINGS.readerRenderSpec(static_cast<uint16_t>(renderer.getScreenWidth()),
                                                          static_cast<uint16_t>(renderer.getScreenHeight()));
  uint32_t h = unitlink::FNV_OFFSET;
  const auto mix = [&h](const auto& value) { h = unitlink::fnv1a(&value, sizeof(value), h); };
  mix(spec.fontId);
  mix(spec.lineCompression);
  mix(spec.extraParagraphSpacing);
  mix(spec.paragraphIndentSpaces);
  mix(spec.characterSpacing);
  mix(spec.wordSpacingPercent);
  mix(spec.paragraphAlignment);
  mix(spec.viewportWidth);
  mix(spec.viewportHeight);
  mix(spec.hyphenationEnabled);
  mix(spec.embeddedStyle);
  mix(spec.imageRendering);
  mix(spec.focusReadingEnabled);
  mix(spec.boldBody);
  return unitlink::fnv1a(CROSSPOINT_VERSION, strlen(CROSSPOINT_VERSION), h);
}

bool UnitLinkService::start(unitlink::Role role, const GfxRenderer& renderer) {
  if (!link.begin(role)) return false;
  unitlink::Config config;
  config.role = role;
  config.bookId = 0;
  config.layoutHash = layoutHashFor(renderer);
  session.start(config, millis());
  return true;
}

void UnitLinkService::setBook(const std::string& epubPath, const GfxRenderer& renderer) {
  if (!running()) return;
  currentBookId = bookIdFor(epubPath);
  session.setBookPath(epubPath);
  session.setIdentity(currentBookId, layoutHashFor(renderer), millis());
}

void UnitLinkService::refreshLayout(const GfxRenderer& renderer) {
  if (running()) session.setIdentity(currentBookId, layoutHashFor(renderer), millis());
}

void UnitLinkService::tick() {
  if (!running() || ending) return;
  session.update(millis());
  std::string path;
  if (session.takeBookRequest(path)) openRequestedBook(path);
}

// The left unit has a book open that this unit does not: open the same file (the books are copied to both SD cards).
// The request repeats until this unit's Hello shows the book, so one try per path every few seconds is enough, and a
// missing file is not retried in a loop.
void UnitLinkService::openRequestedBook(const std::string& path) {
  constexpr unsigned long RETRY_MS = 8000;
  if (path == lastRequestedPath && millis() - lastRequestMs < RETRY_MS) return;
  lastRequestedPath = path;
  lastRequestMs = millis();
  if (bookIdFor(path) == currentBookId) return;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("LINK", "Left unit's book is not on this SD card: %s", path.c_str());
    return;
  }
  LOG_INF("LINK", "Opening the left unit's book: %s", path.c_str());
  activityManager.goToReader(path);
}

void UnitLinkService::endAndRestart() {
  if (ending) return;
  ending = true;
  session.stop();
  link.end();
  silentRestartToSettings();
}

StrId UnitLinkService::statusMessage() const {
  switch (session.status()) {
    case unitlink::Status::Linked:
      return StrId::STR_LINK_LINKED;
    case unitlink::Status::Lost:
      return StrId::STR_LINK_LOST;
    case unitlink::Status::Incompatible: {
      const uint8_t reasons = session.incompatibleReasons();
      if (reasons & unitlink::INCOMPATIBLE_ROLE) return StrId::STR_LINK_SAME_SIDE;
      if (reasons & unitlink::INCOMPATIBLE_VERSION) return StrId::STR_LINK_DIFFERENT_VERSION;
      if (reasons & unitlink::INCOMPATIBLE_BOOK) return StrId::STR_LINK_DIFFERENT_BOOK;
      return StrId::STR_LINK_DIFFERENT_LAYOUT;
    }
    case unitlink::Status::Idle:
    case unitlink::Status::Pairing:
      break;
  }
  return StrId::STR_LINK_PAIRING;
}
