#include "UnitLinkService.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "util/BookmarkUtil.h"

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

bool UnitLinkService::start(unitlink::Role role, const GfxRenderer& renderer, bool remember) {
  if (!link.begin(role)) return false;
  if (remember) {
    APP_STATE.linkSide = role == unitlink::Role::Left ? 1 : 2;
    APP_STATE.saveToFile();
  }
  unitlink::Config config;
  config.role = role;
  config.bookId = 0;
  config.layoutHash = layoutHashFor(renderer);
  session.start(config, millis());
  return true;
}

void UnitLinkService::startFromSavedSide(const GfxRenderer& renderer) {
  if (!HalLink::isSupported() || running() || APP_STATE.linkSide == 0) return;
  start(APP_STATE.linkSide == 1 ? unitlink::Role::Left : unitlink::Role::Right, renderer, false);
}

void UnitLinkService::announceSleep() {
  if (!running() || peerAskedSleep) return;
  const auto status = session.status();
  if (status != unitlink::Status::Linked && status != unitlink::Status::Lost) return;
  session.sendSleep();
  link.flush();
}

bool UnitLinkService::takePeerSleep() {
  if (peerAskedSleep || !running()) return false;
  if (!session.takeSleepRequest()) return false;
  peerAskedSleep = true;
  return true;
}

void UnitLinkService::setBook(const std::string& epubPath, const GfxRenderer& renderer) {
  if (!running()) return;
  currentBookId = bookIdFor(epubPath);
  currentBookPath = epubPath;
  marksDirty = !epubPath.empty();  // a newly paired right unit gets the left unit's bookmarks
  session.setBookPath(epubPath);
  session.setIdentity(currentBookId, layoutHashFor(renderer), millis());
}

void UnitLinkService::refreshLayout(const GfxRenderer& renderer) {
  if (running()) session.setIdentity(currentBookId, layoutHashFor(renderer), millis());
}

void UnitLinkService::tick() {
  if (!running() || ending) return;
  const unsigned long now = millis();
  session.update(now);
  // Nobody answers: set the receiver up again every few seconds, in case the cable was joined with the lines noisy.
  constexpr unsigned long DEAF_MS = 4000;
  const auto status = session.status();
  if ((status == unitlink::Status::Pairing || status == unitlink::Status::Lost) &&
      now - session.lastHeardMs() >= DEAF_MS && now - lastUartRestartMs >= DEAF_MS) {
    lastUartRestartMs = now;
    link.restart();
  }
  std::string path;
  if (session.takeBookRequest(path)) openRequestedBook(path);

  if (session.role() == unitlink::Role::Left) {
    // A right unit that has just started has none of the bookmarks, whatever it was sent before.
    if (session.peerFreshCount() != lastPeerFresh) {
      lastPeerFresh = session.peerFreshCount();
      marksDirty = true;
    }
    if (marksDirty && status == unitlink::Status::Linked && !currentBookPath.empty()) {
      marksDirty = false;
      sendBookmarks();
    }
  } else if (std::string json; session.takeMarks(json)) {
    applyBookmarks(json);
  }
}

void UnitLinkService::bookmarksSaved(const std::string& epubPath) {
  if (running() && session.role() == unitlink::Role::Left && epubPath == currentBookPath) marksDirty = true;
}

bool UnitLinkService::takeBookmarksUpdated() {
  const bool updated = marksUpdated;
  marksUpdated = false;
  return updated;
}

// The right unit gets the file as it is on this card; with no file yet it gets an empty list, so bookmarks removed here
// disappear there too.
void UnitLinkService::sendBookmarks() {
  static constexpr char EMPTY_LIST[] = "{\"bookmarks\":[]}";
  std::string json;
  if (!Storage.readFileToString("LINK", BookmarkUtil::getBookmarkPath(currentBookPath), unitlink::MARKS_MAX_TOTAL,
                                json)) {
    json = EMPTY_LIST;
  }
  if (!session.publishMarks(json, millis()))
    LOG_ERR("LINK", "Bookmark file not sent (%u bytes)", (unsigned)json.size());
}

// Written through a temporary file so a cut cable or a power loss cannot leave half a list behind.
void UnitLinkService::applyBookmarks(const std::string& json) {
  if (currentBookPath.empty()) return;
  const std::string path = BookmarkUtil::getBookmarkPath(currentBookPath);
  std::string current;
  if (Storage.readFileToString("LINK", path, unitlink::MARKS_MAX_TOTAL, current) && current == json) return;
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  const std::string tmp = path + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(json.c_str())) || !Storage.replaceFile(tmp.c_str(), path.c_str())) {
    LOG_ERR("LINK", "Could not store the left unit's bookmarks");
    return;
  }
  LOG_INF("LINK", "Bookmarks copied from the left unit (%u bytes)", (unsigned)json.size());
  marksUpdated = true;
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
  APP_STATE.linkSide = 0;  // USB only comes back if the link does not start again at boot
  APP_STATE.saveToFile();
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
