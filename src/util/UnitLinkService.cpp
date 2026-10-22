#include "UnitLinkService.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SdCardFontSystem.h"
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
  mix(SETTINGS.screenMargin);
  return unitlink::fnv1a(CROSSPOINT_VERSION, strlen(CROSSPOINT_VERSION), h);
}

namespace {
bool parseMac(const std::string& hex, uint8_t* mac) {
  if (hex.size() != 12) return false;
  for (int i = 0; i < 6; i++) {
    char* end = nullptr;
    const std::string byte = hex.substr(i * 2, 2);
    const long v = strtol(byte.c_str(), &end, 16);
    if (end == byte.c_str() || *end != '\0') return false;
    mac[i] = static_cast<uint8_t>(v);
  }
  return true;
}
}  // namespace

bool UnitLinkService::start(unitlink::Role role, GfxRenderer& renderer, bool remember) {
  if (!link.begin(role)) return false;
  gfx = &renderer;
  if (remember && !APP_STATE.linkPeer.empty()) {
    // A pairing started from the Link screen is a new pairing, not the saved one.
    APP_STATE.linkPeer.clear();
    link.unlockPeer();
  } else if (!remember) {
    uint8_t mac[6];
    if (parseMac(APP_STATE.linkPeer, mac)) link.lockPeer(mac);
  }
  if (remember) {
    APP_STATE.linkSide = role == unitlink::Role::Left ? 1 : 2;
    APP_STATE.saveToFile();
  }
  unitlink::Config config;
  config.role = role;
  config.bookId = 0;
  config.layoutHash = layoutHashFor(renderer);
  config.display = currentDisplay();
  announcedLayoutHash = config.layoutHash;
  session.start(config, millis());
  return true;
}

void UnitLinkService::startFromSavedSide(GfxRenderer& renderer) {
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
  if (epubPath.empty() && currentBookId != 0) session.sendClose(currentBookId, millis());  // Home: the peer leaves too
  currentBookId = bookIdFor(epubPath);
  currentBookPath = epubPath;
  textSettingsApplied = false;     // a book that opens now is laid out with the settings already in place
  marksDirty = !epubPath.empty();  // a newly paired right unit gets the left unit's bookmarks
  session.setBookPath(epubPath);
  syncLayout(renderer);
}

void UnitLinkService::refreshLayout(const GfxRenderer& renderer) {
  if (running()) syncLayout(renderer);
}

// The one-byte settings that decide where a page breaks, then the SD font name.
namespace {
void settingsToBlob(uint8_t* out) {
  const uint8_t fields[] = {SETTINGS.fontFamily,
                            SETTINGS.fontPointSize,
                            SETTINGS.fontWeight,
                            SETTINGS.lineSpacing,
                            SETTINGS.paragraphAlignment,
                            SETTINGS.extraParagraphSpacing,
                            SETTINGS.paragraphIndentSpaces,
                            SETTINGS.wordSpacing,
                            SETTINGS.characterSpacing,
                            SETTINGS.hyphenationEnabled,
                            SETTINGS.embeddedStyle,
                            SETTINGS.imageRendering,
                            SETTINGS.focusReadingEnabled,
                            SETTINGS.screenMargin};
  static_assert(sizeof(fields) + sizeof(SETTINGS.sdFontFamilyName) == unitlink::LAYOUT_BLOB_LEN);
  memcpy(out, fields, sizeof(fields));
  memcpy(out + sizeof(fields), SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName));
}

bool blobToSettings(const uint8_t* in) {
  if (in[3] >= CrossPointSettings::LINE_COMPRESSION_COUNT || in[4] >= CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) {
    return false;
  }
  SETTINGS.fontFamily = in[0];
  SETTINGS.fontPointSize = in[1];
  SETTINGS.fontWeight = in[2];
  SETTINGS.lineSpacing = in[3];
  SETTINGS.paragraphAlignment = in[4];
  SETTINGS.extraParagraphSpacing = in[5];
  SETTINGS.paragraphIndentSpaces = in[6];
  SETTINGS.wordSpacing = in[7];
  SETTINGS.characterSpacing = in[8];
  SETTINGS.hyphenationEnabled = in[9];
  SETTINGS.embeddedStyle = in[10];
  SETTINGS.imageRendering = in[11];
  SETTINGS.focusReadingEnabled = in[12];
  SETTINGS.screenMargin = in[13];
  memcpy(SETTINGS.sdFontFamilyName, in + 14, sizeof(SETTINGS.sdFontFamilyName));
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  return true;
}
}  // namespace

// A layout hash that moved without the other unit's settings having been adopted is a change made on this unit: the
// other unit adopts it.
void UnitLinkService::syncLayout(const GfxRenderer& renderer) {
  const uint32_t hash = layoutHashFor(renderer);
  if (hash != announcedLayoutHash) {
    uint8_t blob[unitlink::LAYOUT_BLOB_LEN];
    settingsToBlob(blob);
    session.publishLayout(blob, millis());
    announcedLayoutHash = hash;
  }
  session.setIdentity(currentBookId, hash, millis());
}

void UnitLinkService::applyRemoteLayout(const uint8_t* blob) {
  if (!blobToSettings(blob)) {
    LOG_ERR("LINK", "Text settings from the other unit are out of range");
    return;
  }
  SETTINGS.saveToFile();
  if (gfx) {
    sdFontSystem.ensureLoaded(*gfx);  // the font id in the layout hash needs the SD font loaded
    announcedLayoutHash = layoutHashFor(*gfx);
    session.setIdentity(currentBookId, announcedLayoutHash, millis());
  }
  textSettingsApplied = true;
  LOG_INF("LINK", "Text settings adopted from the other unit");
}

unitlink::DisplayState UnitLinkService::currentDisplay() const {
  unitlink::DisplayState d;
  d.night = SETTINGS.screenInverted != 0;
  if (Frontlight.present()) {
    d.lightOn = Frontlight.isOn();
    d.brightness = std::min<uint8_t>(Frontlight.brightness(), 100);
    d.warmth = Frontlight.hasColorTemperature() ? std::min<uint8_t>(Frontlight.warmth(), 100) : 0;
  }
  return d;
}

void UnitLinkService::applyDisplay(const unitlink::DisplayState& target, unsigned long now) {
  if ((SETTINGS.screenInverted != 0) != target.night) {
    SETTINGS.screenInverted = target.night ? 1 : 0;
    SETTINGS.saveToFile();
    nightApplied = true;
    activityManager.requestUpdate();
    LOG_INF("LINK", "Night mode %s, as on the other unit", target.night ? "on" : "off");
  }
  if (Frontlight.present()) {
    Frontlight.setBrightness(target.brightness);
    if (Frontlight.hasColorTemperature()) Frontlight.setWarmth(target.warmth);
    Frontlight.setOn(target.lightOn);
    SETTINGS.frontlightBrightness = target.brightness;
    if (Frontlight.hasColorTemperature()) SETTINGS.frontlightWarmth = target.warmth;
    SETTINGS.frontlightOn = target.lightOn ? 1 : 0;
    lightSaveDueMs = now + 1000;
  }
  session.setDisplay(currentDisplay(), now);
}

bool UnitLinkService::takeNightApplied() {
  const bool applied = nightApplied;
  nightApplied = false;
  return applied;
}

bool UnitLinkService::takeTextSettingsApplied() {
  const bool applied = textSettingsApplied;
  textSettingsApplied = false;
  return applied;
}

void UnitLinkService::tick() {
  if (!running() || ending) return;
  const unsigned long now = millis();
  session.update(now);
  // Radio: the first unit that links is the partner; from then on only it is heard and answered.
  if (session.status() == unitlink::Status::Linked && !link.peerLocked()) {
    uint8_t mac[6];
    if (link.lastSender(mac)) {
      link.lockPeer(mac);
      char hex[13];
      snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      APP_STATE.linkPeer = hex;
      APP_STATE.saveToFile();
      LOG_INF("LINK", "Locked to peer %s", hex);
    }
  }
  // Nobody answers: set the receiver up again every few seconds, in case the cable was joined with the lines noisy.
  constexpr unsigned long DEAF_MS = 4000;
  const auto status = session.status();
  if ((status == unitlink::Status::Pairing || status == unitlink::Status::Lost) &&
      (now - session.lastHeardMs() >= DEAF_MS || link.radioDown()) && now - lastUartRestartMs >= DEAF_MS) {
    lastUartRestartMs = now;
    link.restart();
  }
  // Night mode and the frontlight follow the other unit; a change made here goes the other way. While a slider is
  // dragged the state moves continuously, so local changes are sent at most every DISPLAY_SEND_MS.
  unitlink::DisplayState remoteDisplay;
  if (session.takeDisplay(remoteDisplay)) applyDisplay(remoteDisplay, now);
  constexpr unsigned long DISPLAY_SEND_MS = 150;
  if (now - lastDisplaySendMs >= DISPLAY_SEND_MS) {
    const unitlink::DisplayState local = currentDisplay();
    if (local != session.display()) {
      lastDisplaySendMs = now;
      session.setDisplay(local, now);
    }
  }
  if (lightSaveDueMs != 0 && now >= lightSaveDueMs) {  // not on every step of a slider
    lightSaveDueMs = 0;
    SETTINGS.saveToFile();
  }

  if (session.takeEndRequest()) {
    LOG_INF("LINK", "The other unit ended the link");
    endAndRestart(/*tellPeer=*/false);
    return;
  }
  if (session.takeClose() && currentBookId != 0) {
    LOG_INF("LINK", "The other unit left the book");
    activityManager.goHome();
  }

  std::string path;
  if (session.takeBookRequest(path)) openRequestedBook(path);

  uint8_t blob[unitlink::LAYOUT_BLOB_LEN];
  if (session.takeLayout(blob)) applyRemoteLayout(blob);
  // Units that only disagree about the text settings and have not sorted it out themselves: the left unit's win.
  if (session.role() == unitlink::Role::Left && status == unitlink::Status::Incompatible &&
      session.incompatibleReasons() == unitlink::INCOMPATIBLE_LAYOUT && !session.layoutUnconfirmed()) {
    constexpr unsigned long OFFER_AFTER_MS = 3000;
    if (layoutMismatchSinceMs == 0) {
      layoutMismatchSinceMs = now;
    } else if (now - layoutMismatchSinceMs >= OFFER_AFTER_MS) {
      layoutMismatchSinceMs = now;
      settingsToBlob(blob);
      session.publishLayout(blob, now);
    }
  } else {
    layoutMismatchSinceMs = 0;
  }

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

void UnitLinkService::endAndRestart(bool tellPeer) {
  if (ending) return;
  ending = true;
  const auto status = session.status();
  if (tellPeer && (status == unitlink::Status::Linked || status == unitlink::Status::Lost)) {
    // The peer ends its link too. Nothing confirms this, so it goes out three times before this unit restarts.
    for (int i = 0; i < 3; i++) {
      session.sendEnd();
      link.flush();
      delay(30);
    }
  }
  APP_STATE.linkSide = 0;  // USB only comes back if the link does not start again at boot
  APP_STATE.linkPeer.clear();
  link.unlockPeer();
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
