#include "EpubReaderFootnoteSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;
}  // namespace

void EpubReaderFootnoteSelectActivity::onEnter() {
  Activity::onEnter();
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  buildFootnoteLinks();
  requestUpdate();
}

void EpubReaderFootnoteSelectActivity::buildFootnoteLinks() {
  footnoteLinks.clear();
  footnoteLinks.reserve(page->footnotes.size());

  // Match each footnote entry to its PageLink on this page. Footnote links
  // and footnote entries share the same href, so we can cross-reference them.
  for (const auto& footnote : page->footnotes) {
    const auto link = std::find_if(page->links.begin(), page->links.end(), [&footnote](const PageLink& candidate) {
      return strcmp(candidate.href, footnote.href) == 0;
    });
    if (link != page->links.end()) {
      footnoteLinks.push_back({&*link, &footnote});
    }
  }
}

// Shows pages[target] if it has footnote markers, with the highlight on its first or last one.
bool EpubReaderFootnoteSelectActivity::hopToPage(const int target, const bool selectLast) {
  if (target < 0 || target > 1 || !pages[target]) return false;
  const int previous = cur;
  cur = target;
  page = pages[cur].get();
  buildFootnoteLinks();
  if (footnoteLinks.empty()) {
    cur = previous;
    page = pages[cur].get();
    buildFootnoteLinks();
    return false;
  }
  selected = selectLast ? static_cast<int>(footnoteLinks.size()) - 1 : 0;
  snapshotIdx = -1;  // the framebuffer no longer holds this page
  requestUpdate();
  return true;
}

void EpubReaderFootnoteSelectActivity::performJump() {
  if (footnoteLinks.empty()) return;
  ActivityResult result;
  result.data = FootnoteResult{footnoteLinks[selected].footnote->href};
  setResult(std::move(result));
  finish();
}

void EpubReaderFootnoteSelectActivity::drawHints() const {
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                                       tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderFootnoteSelectActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
       mappedInput.wasReleased(MappedInputManager::Button::Power)) &&
      !footnoteLinks.empty()) {
    performJump();
    return;
  }

  const bool hasPrev = !footnoteLinks.empty() && selected > 0;
  const bool hasNext = !footnoteLinks.empty() && selected + 1 < static_cast<int>(footnoteLinks.size());
  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  const bool goPrev = moveLeft || mappedInput.wasPressed(MappedInputManager::Button::ScreenUp);
  const bool goNext = moveRight || mappedInput.wasPressed(MappedInputManager::Button::ScreenDown);
  if (goPrev && hasPrev) {
    selected--;
    if (moveLeft) lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (goNext && hasNext) {
    selected++;
    if (moveRight) lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (goPrev && hopToPage(cur - 1, /*selectLast=*/true)) {
    if (moveLeft) lastHorizontalMoveTime = now;
  } else if (goNext && hopToPage(cur + 1, /*selectLast=*/false)) {
    if (moveRight) lastHorizontalMoveTime = now;
  }
}

bool EpubReaderFootnoteSelectActivity::drawHighlightWithSnapshot() {
  if (footnoteLinks.empty()) return false;
  const PageLink* link = footnoteLinks[selected].link;

  const int x = link->x + marginLeft;
  const int y = link->y + marginTop;
  int hx = x - 2;
  int hy = y - 2;
  int hw = link->width + 4;
  int hh = link->height + 4;

  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  // Draw a small marker (the footnote number) at the highlight position
  const char* number = footnoteLinks[selected].footnote->number;
  if (number[0]) {
    renderer.drawText(SETTINGS.getReaderFontId(), x, y - 1, number, false);
  }
  return saved;
}

void EpubReaderFootnoteSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer still
  // holds a clean page.
  if (snapshotIdx >= 0 && !footnoteLinks.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Render the page underneath
  if (page) {
    page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
  }

  if (!footnoteLinks.empty()) {
    drawHighlightWithSnapshot();
  }

  drawHints();

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
