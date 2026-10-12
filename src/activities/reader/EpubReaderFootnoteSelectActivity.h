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
// footnotes, drawn on this panel like a page turn.
class EpubReaderFootnoteSelectActivity final : public Activity {
 public:
  explicit EpubReaderFootnoteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                            std::unique_ptr<Page> first, std::unique_ptr<Page> second,
                                            bool startOnSecond, int marginLeft, int marginTop)
      : Activity("EpubReaderFootnoteSelect", renderer, mappedInput), marginLeft(marginLeft), marginTop(marginTop) {
    pages[0] = std::move(first);
    pages[1] = std::move(second);
    cur = startOnSecond && pages[1] ? 1 : 0;
    page = pages[cur].get();
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
  void drawHints() const;

  std::unique_ptr<Page> pages[2];  // reading order; the second is null outside a spread
  int cur = 0;
  Page* page = nullptr;  // pages[cur]
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

  unsigned long lastHorizontalMoveTime = 0;
};
