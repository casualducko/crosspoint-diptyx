#include "UnitLinkService.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "util/BookmarkUtil.h"
#include "util/TouchLock.h"

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
  server.cancel();  // the open file belongs to the book that was open
  if (serveFile.isOpen()) serveFile.close();
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
  if (in[0] >= CrossPointSettings::FONT_FAMILY_COUNT || in[2] >= CrossPointSettings::FONT_WEIGHT_COUNT ||
      in[3] >= CrossPointSettings::LINE_COMPRESSION_COUNT || in[4] >= CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT ||
      in[11] >= CrossPointSettings::IMAGE_RENDERING_COUNT) {
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

// Radio build: the other unit lying against this one (a closed case) shows as a very strong signal, about -10 dBm
// against -40 when the case is open. Closing the case locks touch, opening it unlocks.
void UnitLinkService::watchCase(unsigned long now) {
  constexpr int CLOSED_DBM = -14;
  constexpr int OPEN_DBM = -17;
  constexpr unsigned long SETTLE_OPEN_MS =
      1500;  // closing locks as soon as two packets agree: the screens touch at once
  constexpr unsigned long HOLD_AFTER_CHANGE_MS =
      8000;  // after a change the lock is not released again at once (no flapping); closing is never held
  constexpr uint32_t FRESH_MS = 3000;
  if (!caseKnown) {  // a lock kept from before sleep counts as "case closed": the first reading may open it
    caseKnown = true;
    caseClosed = touchlock::locked();
  }
  const int rssi = link.lastRssi();
  if (!SETTINGS.caseAutoLock || rssi == 0 || link.lastRssiAgeMs() > FRESH_MS) {
    caseSinceMs = 0;
    signalAverage = 0;  // an old reading must not weigh on the first ones after the link comes back
    return;
  }
  if (session.role() != unitlink::Role::Left) return;  // one unit decides; the other follows the lock and the light
  if (link.rssiSamples() != lastRssiSample) {          // one step per packet, not per loop pass
    lastRssiSample = link.rssiSamples();
    signalAverage = signalAverage == 0 ? rssi : (signalAverage + rssi) / 2;
  }
  if (signalAverage == 0) return;  // no packet since the average was cleared: 0 dBm would read as "case closed"
  const bool wantClosed = caseClosed ? signalAverage > OPEN_DBM : signalAverage >= CLOSED_DBM;
  if (!wantClosed && lastCaseChangeMs != 0 && now - lastCaseChangeMs < HOLD_AFTER_CHANGE_MS) {
    caseSinceMs = 0;
    return;
  }
  if (wantClosed == caseClosed) {
    caseSinceMs = 0;
    return;
  }
  if (caseSinceMs == 0) caseSinceMs = now;
  if (!wantClosed && now - caseSinceMs < SETTLE_OPEN_MS) return;
  caseSinceMs = 0;
  lastCaseChangeMs = now;
  caseClosed = wantClosed;
  LOG_INF("LINK", "Case %s (signal %d dBm)", caseClosed ? "closed" : "open", signalAverage);
  touchlock::set(caseClosed);
  // Closed, the units face each other: the light would only shine into the other screen. Back on when the case opens
  // (the other unit's change arrives the same way if it saw the case change later).
  if (Frontlight.present()) {
    if (caseClosed && Frontlight.isOn()) {
      lightOffByCase = true;
      Frontlight.setOn(false);
    } else if (!caseClosed && lightOffByCase) {
      lightOffByCase = false;
      Frontlight.setOn(true);
    } else {
      return;
    }
    SETTINGS.frontlightOn =
        (Frontlight.isOn() || lightOffByCase) ? 1 : 0;  // asleep in a closed case: light back on at wake
    lightSaveDueMs = now + 1000;
    session.setDisplay(currentDisplay(), now);
  }
}

unitlink::DisplayState UnitLinkService::currentDisplay() const {
  unitlink::DisplayState d;
  d.night = SETTINGS.screenInverted != 0;
  d.locked = touchlock::locked();
  if (Frontlight.present()) {
    d.lightOn = Frontlight.isOn();
    d.brightness = std::min<uint8_t>(Frontlight.brightness(), 100);
    d.warmth = Frontlight.hasColorTemperature() ? std::min<uint8_t>(Frontlight.warmth(), 100) : 0;
  }
  return d;
}

void UnitLinkService::applyDisplay(const unitlink::DisplayState& target, uint8_t parts, unsigned long now) {
  // The other unit saw the case close or open before this one did: the light follows the lock, and this unit must
  // remember (or give back) the light it lost, whatever its own signal reading says.
  const bool lightWasOn = Frontlight.present() && Frontlight.isOn();
  const bool lockChanged = (parts & unitlink::DISPLAY_LOCK) != 0 && target.locked != touchlock::locked();
  if (lockChanged && target.locked && (parts & unitlink::DISPLAY_LIGHT) && lightWasOn && !target.lightOn) {
    lightOffByCase = true;
  }
  if (parts & unitlink::DISPLAY_LOCK) touchlock::set(target.locked);
  if ((parts & unitlink::DISPLAY_NIGHT) && (SETTINGS.screenInverted != 0) != target.night) {
    SETTINGS.screenInverted = target.night ? 1 : 0;
    SETTINGS.saveToFile();
    nightApplied = true;
    activityManager.requestUpdate();
    LOG_INF("LINK", "Night mode %s, as on the other unit", target.night ? "on" : "off");
  }
  if ((parts & unitlink::DISPLAY_LIGHT) && Frontlight.present()) {
    Frontlight.setBrightness(target.brightness);
    if (Frontlight.hasColorTemperature()) Frontlight.setWarmth(target.warmth);
    Frontlight.setOn(target.lightOn);
    SETTINGS.frontlightBrightness = target.brightness;
    if (Frontlight.hasColorTemperature()) SETTINGS.frontlightWarmth = target.warmth;
    SETTINGS.frontlightOn = (target.lightOn || lightOffByCase) ? 1 : 0;
    lightSaveDueMs = now + 1000;
  }
  if (lockChanged && !target.locked && lightOffByCase && Frontlight.present()) {
    lightOffByCase = false;
    Frontlight.setOn(true);
    SETTINGS.frontlightOn = 1;
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
  if (const uint8_t key = statusKey(); key != pendingKey) {
    pendingKey = key;
    pendingKeySince = now;
  } else if (now - pendingKeySince >= STATUS_SETTLE_MS) {
    settledKey = key;
  }
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
  uint8_t remoteParts = 0;
  if (session.takeDisplay(remoteDisplay, &remoteParts)) applyDisplay(remoteDisplay, remoteParts, now);
  constexpr unsigned long DISPLAY_SEND_MS = 150;
  if (now - lastDisplaySendMs >= DISPLAY_SEND_MS) {
    const unitlink::DisplayState local = currentDisplay();
    if (local != session.display()) {
      lastDisplaySendMs = now;
      session.setDisplay(local, now);
    }
  }
  watchCase(now);
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

  tickReceive(now);
  tickServe();

  std::string path;
  if (session.takeBookRequest(path) && !receiving) openRequestedBook(path);

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
    if (!startReceive(path)) LOG_ERR("LINK", "The other unit's book is not on this SD card: %s", path.c_str());
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
      if (sending() && reasons == unitlink::INCOMPATIBLE_BOOK) return StrId::STR_LINK_SENDING;
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

// ---- Book transfer ----------------------------------------------------------------------------------------------

namespace {
constexpr unsigned long RECEIVE_RETRY_MS = 60000;  // after a failed transfer of a path

bool endsWithEpub(const std::string& path) {
  if (path.size() < 6) return false;
  const std::string tail = path.substr(path.size() - 5);
  return strcasecmp(tail.c_str(), ".epub") == 0;
}
}  // namespace

// What a paired unit may make this unit write: an absolute .epub path without "..", empty parts or control characters.
bool UnitLinkService::receivablePath(const std::string& path) {
  if (path.empty() || path.size() > 180 || path[0] != '/' || !endsWithEpub(path)) return false;
  for (char c : path) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':') return false;
  }
  if (path.find("//") != std::string::npos || path.find("/../") != std::string::npos) return false;
  const size_t slash = path.find_last_of('/');
  return path.compare(slash + 1, std::string::npos, "..") != 0 && path.size() > slash + 6;  // a name, not just ".epub"
}

class UnitLinkService::FileSink : public unitlink::Sink {
 public:
  explicit FileSink(HalFile& f) : file(f) {}
  bool write(const uint8_t* data, size_t n) override { return file.write(data, n) == n; }

 private:
  HalFile& file;
};

class UnitLinkService::FileSource : public unitlink::Source {
 public:
  FileSource(HalFile& f, const std::string& p) : file(f), path(p) {}
  // Opened when the first request names this book, so an idle link holds no file.
  uint32_t size() override {
    if (!file.isOpen() && !Storage.openFileForRead("LINK", path, file)) return 0;
    return static_cast<uint32_t>(file.size());
  }
  bool read(uint32_t offset, uint8_t* out, size_t n) override {
    return file.seekSet(offset) && file.read(out, n) == static_cast<int>(n);
  }

 private:
  HalFile& file;
  const std::string& path;
};

bool UnitLinkService::startReceive(const std::string& path) {
  if (receiving || !receivablePath(path)) return false;
  // Only the unit that was paired on purpose may send a file, and only when the two units agree on everything else.
  const uint8_t reasons = session.incompatibleReasons();
  if (!link.peerLocked() || session.peerBook() == 0 ||
      (reasons & (unitlink::INCOMPATIBLE_VERSION | unitlink::INCOMPATIBLE_ROLE))) {
    return false;
  }
  if (path == failedReceivePath && millis() - failedReceiveMs < RECEIVE_RETRY_MS) return false;
  const std::string part = path + ".part";
  const size_t slash = path.find_last_of('/');
  if (slash > 0) Storage.ensureDirectoryExists(path.substr(0, slash).c_str());
  if (!Storage.openFileForWrite("LINK", part, receiveFile)) {
    LOG_ERR("LINK", "Cannot create %s", part.c_str());
    return false;
  }
  receiving = true;
  receivePath = path;
  receiveStartMs = millis();
  receiveShownPercent = -1;
  receiver.begin(session, millis());
  LOG_INF("LINK", "Receiving %s from the other unit", path.c_str());
  showReceiveProgress();
  return true;
}

void UnitLinkService::abortReceive(bool failed) {
  if (receiveFile.isOpen()) receiveFile.close();
  Storage.remove((receivePath + ".part").c_str());
  if (failed) {
    failedReceivePath = receivePath;
    failedReceiveMs = millis();
    LOG_ERR("LINK", "Book transfer failed: %s", receivePath.c_str());
    if (gfx) {
      RenderLock lock;
      GUI.drawPopup(*gfx, tr(STR_LINK_RECEIVE_FAILED));
    }
  }
  receiving = false;
  receivePath.clear();
}

void UnitLinkService::finishReceive() {
  receiveFile.close();
  const std::string path = receivePath;
  const std::string part = path + ".part";
  if (Storage.exists(path.c_str()) || !Storage.rename(part.c_str(), path.c_str())) {
    abortReceive(true);
    return;
  }
  receiving = false;
  receivePath.clear();
  // The same name and size give the same book id on both units; anything else means the copy is not the book.
  if (bookIdFor(path) != session.peerBook()) {
    LOG_ERR("LINK", "Received book does not match the other unit's");
    Storage.remove(path.c_str());
    failedReceivePath = path;
    failedReceiveMs = millis();
    return;
  }
  const unsigned long ms = std::max(1ul, millis() - receiveStartMs);
  LOG_INF("LINK", "Received %s: %u bytes in %lu ms (%lu KB/s)", path.c_str(), static_cast<unsigned>(receiver.total()),
          ms, static_cast<unsigned long>(receiver.total()) / ms);
  lastRequestedPath = path;
  lastRequestMs = millis();
  activityManager.goToReader(path);
}

// A popup at the start and then at every tenth: each one is a panel refresh, so no more often than that.
void UnitLinkService::showReceiveProgress() {
  if (!receiving || !gfx) return;
  const int tenth = receivePercent() / 10;
  if (tenth == receiveShownPercent) return;
  receiveShownPercent = tenth;
  const std::string text = std::string(tr(STR_LINK_RECEIVING)) + " " + std::to_string(tenth * 10) + "%";
  RenderLock lock;
  GUI.drawPopup(*gfx, text.c_str());
}

void UnitLinkService::tickReceive(unsigned long now) {
  if (!receiving) return;
  if (session.status() == unitlink::Status::Idle) {
    abortReceive(true);
    return;
  }
  showReceiveProgress();
  FileSink sink(receiveFile);
  switch (receiver.update(session, sink, now)) {
    case unitlink::BookReceiver::State::Done:
      finishReceive();
      break;
    case unitlink::BookReceiver::State::Failed:
      abortReceive(true);
      break;
    default:
      break;
  }
}

// The unit that has the book open serves it. Requests name the book by its id; any other is ignored.
void UnitLinkService::tickServe() {
  if (currentBookId == 0 || currentBookPath.empty() || !link.peerLocked()) {
    server.cancel();
    return;
  }
  FileSource source(serveFile, currentBookPath);
  const unsigned long now = millis();
  if (server.update(session, source, currentBookId)) serveLastMs = now;
  if (!server.busy() && serveFile.isOpen() && now - serveLastMs >= SEND_SHOWN_MS) serveFile.close();
}
