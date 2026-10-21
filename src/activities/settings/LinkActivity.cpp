#include "LinkActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// The same book on both SD cards has the same file name and size; the path may differ.
uint32_t bookIdFromOpenBook() {
  const std::string& path = APP_STATE.openEpubPath;
  if (path.empty()) return 0;
  const size_t slash = path.find_last_of('/');
  const char* name = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  uint32_t id = unitlink::fnv1a(name, strlen(name));
  HalFile file;
  if (Storage.openFileForRead("LINK", path, file)) {
    const uint32_t size = static_cast<uint32_t>(file.size());
    id = unitlink::fnv1a(&size, sizeof(size), id);
  }
  return id;
}

// Everything that changes where a page break falls, taken from the same spec the reader builds its section cache from.
uint32_t layoutHashFromSettings(const GfxRenderer& renderer) {
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
}  // namespace

void LinkActivity::onEnter() {
  Activity::onEnter();
  if (!HalLink::isSupported()) {
    finish();
    return;
  }
  state = State::ChooseSide;
  const StrId options[] = {StrId::STR_LINK_LEFT_PAGE, StrId::STR_LINK_RIGHT_PAGE, StrId::STR_CANCEL};
  sidePopup.show(StrId::STR_LINK_UNIT, options, 3, 0, [this](int idx) {
    if (idx == 0) {
      startLink(unitlink::Role::Left);
    } else if (idx == 1) {
      startLink(unitlink::Role::Right);
    } else {
      finish();
    }
  });
  requestUpdate();
}

void LinkActivity::onExit() {
  // Leaving through finish() before a side was chosen needs nothing; once the link runs, endLink() restarts the unit.
  Activity::onExit();
}

void LinkActivity::startLink(unitlink::Role chosen) {
  role = chosen;
  unitlink::Config config;
  config.role = chosen;
  config.bookId = bookIdFromOpenBook();
  config.layoutHash = layoutHashFromSettings(renderer);
  if (!link.begin(chosen)) {
    LOG_ERR("LINK", "Link could not start");
    finish();
    return;
  }
  state = State::Running;
  session.start(config, millis());
  requestUpdate();
}

void LinkActivity::endLink() {
  if (leaving) return;
  leaving = true;
  link.end();
  // The pads are the USB lines: only a restart brings USB back.
  silentRestartToSettings();
}

void LinkActivity::loop() {
  if (state == State::ChooseSide) {
    if (sidePopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) finish();
    return;
  }

  session.update(millis());

  if (role == unitlink::Role::Left) {
    // Test page: stands in for the reader's page until the reader drives the link.
    bool changed = false;
    if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      testPage++;
      changed = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) && testPage > 0) {
      testPage--;
      changed = true;
    }
    if (changed) {
      testPageKnown = true;
      session.publish(0, testPage, millis());
    }
  } else {
    unitlink::PageState received;
    if (session.takeRemote(received)) {
      testPage = received.page;
      testPageKnown = true;
    }
  }
  if (session.takeSleepRequest()) LOG_INF("LINK", "Peer asked for sleep (not acted on yet)");

  const bool acked = session.publishedIsAcked();
  if (session.status() != shownStatus || testPage != shownPage || acked != shownAcked) requestUpdate();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) endLink();
}

void LinkActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LINK_UNIT));

  if (state == State::ChooseSide) {
    if (sidePopup.processRender(renderer, mappedInput)) return;
    renderer.displayBuffer();
    return;
  }

  shownStatus = session.status();
  shownPage = testPage;
  shownAcked = session.publishedIsAcked();

  const char* statusText = tr(STR_LINK_PAIRING);
  switch (shownStatus) {
    case unitlink::Status::Linked:
      statusText = tr(STR_LINK_LINKED);
      break;
    case unitlink::Status::Lost:
      statusText = tr(STR_LINK_LOST);
      break;
    case unitlink::Status::Incompatible: {
      const uint8_t reasons = session.incompatibleReasons();
      if (reasons & unitlink::INCOMPATIBLE_ROLE) {
        statusText = tr(STR_LINK_SAME_SIDE);
      } else if (reasons & unitlink::INCOMPATIBLE_VERSION) {
        statusText = tr(STR_LINK_DIFFERENT_VERSION);
      } else if (reasons & unitlink::INCOMPATIBLE_BOOK) {
        statusText = tr(STR_LINK_DIFFERENT_BOOK);
      } else {
        statusText = tr(STR_LINK_DIFFERENT_LAYOUT);
      }
      break;
    }
    case unitlink::Status::Idle:
    case unitlink::Status::Pairing:
      break;
  }

  const int mid = pageHeight / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, mid - 70,
                            role == unitlink::Role::Left ? tr(STR_LINK_LEFT_PAGE) : tr(STR_LINK_RIGHT_PAGE), true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, mid - 30, statusText);
  if (shownStatus == unitlink::Status::Linked || shownStatus == unitlink::Status::Lost) {
    char line[48];
    if (role == unitlink::Role::Left || testPageKnown) {
      snprintf(line, sizeof(line), "%u%s", static_cast<unsigned>(testPage), shownAcked ? "" : " ...");
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, line, true, EpdFontFamily::BOLD);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 40,
                              role == unitlink::Role::Left ? tr(STR_LINK_TEST_LEFT) : tr(STR_LINK_TEST_RIGHT));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, mid + 90, tr(STR_LINK_END_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
