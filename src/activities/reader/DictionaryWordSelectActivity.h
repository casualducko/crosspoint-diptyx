#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
// In a two-page spread it gets both pages in reading order: stepping past the
// last (first) word of a page continues on the next (previous) page, and the
// highlight moves to the panel that page is on (the right panel's page is
// highlighted there, through RightPanel::overlay(); the left screen then shows
// its own page without a highlight).
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> first, std::unique_ptr<Page> second, bool spreadRtl,
                                        int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        rtl(spreadRtl),
        marginLeft(marginLeft),
        marginTop(marginTop) {
    pages[0] = std::move(first);
    pages[1] = std::move(second);
    cur = rtl && pages[1] ? 1 : 0;  // start on the page of the left panel
    page = pages[cur].get();
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  bool hopToPage(int target, bool selectLast);
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawHighlight(const WordBox& word) const;
  void drawPage(Page& pageToDraw);
  void renderRightFocus();
  void drawHints() const;
  // Spread pages are in reading order; the first is on the right panel right to left, else the second is.
  bool onRight(const int pageIndex) const { return rtl ? pageIndex == 0 : pageIndex == 1; }

  std::unique_ptr<Page> pages[2];  // reading order; the second is null outside a spread
  int cur = 0;
  Page* page = nullptr;  // pages[cur]
  const bool rtl;        // right-to-left spread
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;
  unsigned long lastHorizontalMoveTime = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // Highlight on the right panel's page: whether one is drawn there and for which word, and whether the left screen
  // currently shows its own page without a highlight (so a move on the right panel needs no left repaint).
  bool rightHighlighted = false;
  int rightShownIdx = -1;
  bool leftClean = false;
};
