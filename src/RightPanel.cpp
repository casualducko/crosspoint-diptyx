#include "RightPanel.h"

#include <BoardConfig.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Xtc.h>
#include <esp_attr.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <string>

#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "fontIds.h"

namespace {

constexpr char IDLE_JPG[] = "/idle_screen_right.jpg";    // stock Diptyx idle image convention
constexpr char IDLE_BMP[] = "/.crosspoint/idle_right.bmp";  // 1-bit conversion cache

// Identifies what the right panel currently shows. RTC memory survives deep sleep but not a power-off, so the card is
// simply redrawn once after a cold boot.
RTC_DATA_ATTR uint32_t shownKey = 0;

uint32_t hashString(const std::string& s) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : s) h = (h ^ c) * 16777619u;
  return h ? h : 1;
}

struct BookCard {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;  // empty when the cover could not be produced
};

// The current book: the open one, else the most recent. Returns false when there is none.
bool currentBook(BookCard& card) {
  const auto& books = RECENT_BOOKS.getBooks();
  card.path = APP_STATE.openEpubPath;
  if (card.path.empty() && !books.empty()) card.path = books.front().path;
  if (card.path.empty()) return false;
  for (const auto& b : books) {
    if (b.path == card.path) {
      card.title = b.title;
      card.author = b.author;
      break;
    }
  }
  return true;
}

// Full-size cover for the card, the same way the cover sleep screen produces it. Heavy (opens the book), so it only
// runs when the card is actually redrawn. Failures (e.g. a progressive JPEG) just mean a text-only card.
void resolveCover(BookCard& card) {
  if (FsHelpers::hasXtcExtension(card.path)) {
    Xtc xtc(card.path, "/.crosspoint");
    if (xtc.load() && xtc.generateCoverBmp()) card.coverBmpPath = xtc.getCoverBmpPath();
  } else if (FsHelpers::hasReflowableBookExtension(card.path)) {
    Epub epub(card.path, "/.crosspoint");
    if (epub.load(true, true)) {
      if (card.title.empty()) card.title = epub.getTitle();
      if (card.author.empty()) card.author = epub.getAuthor();
      if (epub.generateCoverBmp(false, false)) card.coverBmpPath = epub.getCoverBmpPath(false, false);
    }
  }
}

void drawCentered(const GfxRenderer& r, const int fontId, const std::string& text, int& y, const int maxLines,
                  const int maxWidth) {
  const auto lines = r.wrappedText(fontId, text.c_str(), maxWidth, maxLines);
  const int step = r.getTextHeight(fontId) + 6;
  for (const auto& line : lines) {
    r.drawCenteredText(fontId, y, line.c_str());
    y += step;
  }
}

void drawBookCard(GfxRenderer& r, const BookCard& card) {
  const int w = r.getScreenWidth();
  const int h = r.getScreenHeight();
  r.clearScreen();

  bool drewCover = false;
  if (!card.coverBmpPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("RP", card.coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        constexpr int margin = 28;
        const int boxW = w - 2 * margin;
        const int boxH = h - 190;
        // drawBitmap only scales down, so centre using the scaled size.
        float scale = std::min(static_cast<float>(boxW) / bitmap.getWidth(), static_cast<float>(boxH) / bitmap.getHeight());
        if (scale > 1.0f) scale = 1.0f;
        const int dw = static_cast<int>(bitmap.getWidth() * scale);
        const int dh = static_cast<int>(bitmap.getHeight() * scale);
        drewCover = r.drawBitmap(bitmap, margin + (boxW - dw) / 2, margin + (boxH - dh) / 2, boxW, boxH);
      }
    }
  }

  const int textWidth = w - 56;
  if (drewCover) {
    int y = h - 150;
    drawCentered(r, UI_12_FONT_ID, card.title, y, 2, textWidth);
    drawCentered(r, UI_10_FONT_ID, card.author, y, 1, textWidth);
  } else {
    int y = h / 2 - 90;
    drawCentered(r, UI_12_FONT_ID, card.title.empty() ? "CrossPoint" : card.title, y, 4, textWidth);
    y += 16;
    drawCentered(r, UI_10_FONT_ID, card.author, y, 2, textWidth);
  }
}

// Draws the stock idle image into the framebuffer. False if there is no usable image.
bool drawIdleImage(GfxRenderer& r) {
  if (!Storage.exists(IDLE_JPG)) return false;
  if (!Storage.exists(IDLE_BMP)) {
    HalFile jpg;
    HalFile bmp;
    if (!Storage.openFileForRead("RP", IDLE_JPG, jpg) || !Storage.openFileForWrite("RP", IDLE_BMP, bmp)) return false;
    const bool ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpg, bmp, r.getScreenWidth(), r.getScreenHeight());
    bmp.close();
    if (!ok) {
      Storage.remove(IDLE_BMP);
      LOG_ERR("RP", "Could not convert %s", IDLE_JPG);
      return false;
    }
  }
  HalFile file;
  if (!Storage.openFileForRead("RP", IDLE_BMP, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  r.clearScreen();
  const int x = std::max(0, (r.getScreenWidth() - bitmap.getWidth()) / 2);
  const int y = std::max(0, (r.getScreenHeight() - bitmap.getHeight()) / 2);
  return r.drawBitmap(bitmap, x, y, r.getScreenWidth(), r.getScreenHeight());
}

template <typename DrawFn>
void presentOnRight(GfxRenderer& r, HalDisplay& d, DrawFn&& draw) {
  // Full waveform: the card / idle image sits there until the book changes.
  RightPanel::present(r, d, std::forward<DrawFn>(draw), HalDisplay::HALF_REFRESH);
}

}  // namespace

namespace RightPanel {

void showCoverCardIfChanged(GfxRenderer& renderer, HalDisplay& display) {
  if (!BoardConfig::isDiptyx()) return;
  BookCard card;
  const bool hasBook = currentBook(card);
  const uint32_t key = hasBook ? hashString(card.path) : 1;
  if (key == shownKey) return;

  LOG_DBG("RP", "Cover card for %s", hasBook ? card.path.c_str() : "(no book)");
  if (hasBook) resolveCover(card);
  presentOnRight(renderer, display, [&] { drawBookCard(renderer, card); });
  shownKey = key;
}

void markDirty() { shownKey = 0; }

void showSleepScreen(GfxRenderer& renderer, HalDisplay& display) {
  if (!BoardConfig::isDiptyx()) return;
  bool idleDrawn = false;
  BookCard card;
  presentOnRight(renderer, display, [&] {
    idleDrawn = drawIdleImage(renderer);
    if (!idleDrawn && currentBook(card)) {
      resolveCover(card);
      drawBookCard(renderer, card);
    } else if (!idleDrawn) {
      drawBookCard(renderer, card);
    }
  });
  shownKey = 0;  // force the home card to be redrawn after the next wake
}

}  // namespace RightPanel
