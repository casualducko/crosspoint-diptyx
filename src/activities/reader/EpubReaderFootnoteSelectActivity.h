#pragma once

#include <Epub/Page.h>
#include <Epub/PageLink.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"

// Footnote reference selection overlaid on the current reader page, modeled on
// DictionaryWordSelectActivity: the page renders underneath and each footnote
// reference marker gets a highlight box. Up/Down/Left/Right move between
// markers, Confirm jumps to that footnote, Back returns to the reader.
// In a two-page spread it gets both pages in reading order: stepping past the
// last (first) marker of a page continues on the next (previous) page that has
// footnotes, and the highlight moves to the panel that page is on (the right
// panel's page is highlighted there, through RightPanel::overlay(); the left
// screen then shows its own page without a highlight).
class EpubReaderFootnoteSelectActivity final : public Activity {
 public:
  explicit EpubReaderFootnoteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                            std::unique_ptr<Page> first, std::unique_ptr<Page> second, bool spreadRtl,
                                            int marginLeft, int marginTop)
      : Activity("EpubReaderFootnoteSelect", renderer, mappedInput),
        rtl(spreadRtl),
        marginLeft(marginLeft),
        marginTop(marginTop) {
    pages[0] = std::move(first);
    pages[1] = std::move(second);
    page = pages[0].get();  // onEnter() picks the page to start on
  }

 private:
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void buildFootnoteLinks();
  bool hopToPage(int target, bool selectLast);
  void performJump();
  bool drawHighlightWithSnapshot();
  void drawHighlight() const;
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

  struct FootnoteLink {
    const PageLink* link;
    const FootnoteEntry* footnote;
  };
  std::vector<FootnoteLink> footnoteLinks;
  int selected = 0;

  // Differential highlight repaint: pixels under the current highlight box.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // Highlight on the right panel's page: whether one is drawn there and for which marker, and whether the left screen
  // currently shows its own page without a highlight.
  bool rightHighlighted = false;
  int rightShownIdx = -1;
  bool leftClean = false;

  unsigned long lastHorizontalMoveTime = 0;
};
