#include "UnitLinkService.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "SilentRestart.h"

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
  session.setIdentity(currentBookId, layoutHashFor(renderer), millis());
}

void UnitLinkService::refreshLayout(const GfxRenderer& renderer) {
  if (running()) session.setIdentity(currentBookId, layoutHashFor(renderer), millis());
}

void UnitLinkService::tick() {
  if (running() && !ending) session.update(millis());
}

void UnitLinkService::endAndRestart() {
  if (ending) return;
  ending = true;
  session.stop();
  link.end();
  silentRestartToSettings();
}
