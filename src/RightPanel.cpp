#include "RightPanel.h"

#include <Bitmap.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "fontIds.h"

namespace {

constexpr char IDLE_JPG[] = "/idle_screen_right.jpg";       // stock Diptyx idle image convention
// Cover box height on the 480x648 card: the cover fills the panel inside a margin, with no text.
constexpr int kCoverMargin = 28;
constexpr int kCoverBoxHeight = 648 - 2 * kCoverMargin;
constexpr char IDLE_BMP[] = "/.crosspoint/idle_right.bmp";  // 1-bit conversion cache
constexpr char IDLE_KEY[] = "/.crosspoint/idle_right.key";  // size of the JPEG the cache was made from

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
// The card last drawn, so a night-mode change can redraw it without reopening the book (RAM only: after a wake the
// key is cleared anyway).
BookCard shownCard;
bool shownCardValid = false;

// The current book: the open one, else the most recent. Returns false when there is none.
bool currentBook(BookCard& card) {
  const auto& books = RECENT_BOOKS.getBooks();
  card.path = APP_STATE.openEpubPath;
  // A deleted open book must not leave a text-only card while Home shows another book.
  if (!card.path.empty() && !Storage.exists(card.path.c_str())) card.path.clear();
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
    // The book objects are large; keep them off the task stack.
    auto xtc = makeUniqueNoThrow<Xtc>(card.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("RP", "OOM: XTC cover");
      return;
    }
    if (xtc->load() && xtc->generateCoverBmp()) card.coverBmpPath = xtc->getCoverBmpPath();
  } else if (FsHelpers::hasReflowableBookExtension(card.path)) {
    auto epub = makeUniqueNoThrow<Epub>(card.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("RP", "OOM: EPUB cover");
      return;
    }
    if (epub->load(true, true)) {
      if (card.title.empty()) card.title = epub->getTitle();
      if (card.author.empty()) card.author = epub->getAuthor();
      // The panel has no grayscale, so a 1-bit dithered cover sized to the card beats thresholding the 2-bit one.
      if (epub->generateThumbBmp(kCoverBoxHeight)) {
        card.coverBmpPath = epub->getThumbBmpPath(kCoverBoxHeight);
      } else if (epub->generateCoverBmp(false, false)) {
        card.coverBmpPath = epub->getCoverBmpPath(false, false);
      }
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
        constexpr int margin = kCoverMargin;
        const int boxW = w - 2 * margin;
        const int boxH = h - 2 * margin;
        // drawBitmap only scales down, so center using the scaled size.
        float scale =
            std::min(static_cast<float>(boxW) / bitmap.getWidth(), static_cast<float>(boxH) / bitmap.getHeight());
        if (scale > 1.0f) scale = 1.0f;
        const int dw = static_cast<int>(bitmap.getWidth() * scale);
        const int dh = static_cast<int>(bitmap.getHeight() * scale);
        drewCover = r.drawBitmap(bitmap, margin + (boxW - dw) / 2, margin + (boxH - dh) / 2, boxW, boxH);
      }
    }
  }

  const int textWidth = w - 56;
  if (!drewCover) {
    int y = h / 2 - 90;
    drawCentered(r, UI_12_FONT_ID, card.title.empty() ? "CrossPoint" : card.title, y, 4, textWidth);
    y += 16;
    drawCentered(r, UI_10_FONT_ID, card.author, y, 2, textWidth);
  }
}

// A quiet sleep layout: the title in a serif face with the author below a short rule, on plain white, a little above
// the vertical middle.
void drawQuietTitle(GfxRenderer& r, const BookCard& card) {
  const int w = r.getScreenWidth();
  const int h = r.getScreenHeight();
  r.clearScreen();
  const int textWidth = w - 140;
  int y = h * 38 / 100;
  drawCentered(r, LITERATAMONO_14_FONT_ID, card.title, y, 4, textWidth);
  if (!card.author.empty()) {
    y += 14;
    r.drawLine(w / 2 - 22, y, w / 2 + 22, y, true);
    y += 22;
    drawCentered(r, UI_10_FONT_ID, card.author, y, 2, textWidth);
  }
}

// Makes sure the 1-bit conversion of the stock idle image is cached. False if there is no usable image. This decodes a
// JPEG, so it runs before present() parks a copy of the left frame (which would take heap away from the decoder).
bool ensureIdleCache(const GfxRenderer& r) {
  if (!Storage.exists(IDLE_JPG)) return false;
  // The conversion is cached, but must follow the JPEG: replacing idle_screen_right.jpg (a different size) has to
  // regenerate it. The size of the source file is the cache key.
  std::string jpgKey;
  {
    HalFile jpg;
    if (!Storage.openFileForRead("RP", IDLE_JPG, jpg)) return false;
    jpgKey = std::to_string(jpg.fileSize()) + ":" + std::to_string(jpg.modificationTime()) + ":" +
             std::to_string(r.getScreenWidth()) + "x" + std::to_string(r.getScreenHeight());
  }
  std::string cachedKey;
  const bool haveKey = Storage.exists(IDLE_KEY) && Storage.readFileToString("RP", IDLE_KEY, 96, cachedKey);
  // A JPEG that failed to convert is remembered (":bad") so it is not retried on every sleep until the file changes.
  if (haveKey && cachedKey == jpgKey + ":bad") return false;
  const bool cacheValid = haveKey && Storage.exists(IDLE_BMP) && cachedKey == jpgKey;
  if (!cacheValid) {
    Storage.remove(IDLE_KEY);
    HalFile jpg;
    HalFile bmp;
    if (!Storage.openFileForRead("RP", IDLE_JPG, jpg) || !Storage.openFileForWrite("RP", IDLE_BMP, bmp)) return false;
    const bool ok =
        JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpg, bmp, r.getScreenWidth(), r.getScreenHeight());
    bmp.close();
    if (!ok) {
      Storage.remove(IDLE_BMP);
      Storage.writeFile(IDLE_KEY, String((jpgKey + ":bad").c_str()));
      LOG_ERR("RP", "Could not convert %s", IDLE_JPG);
      return false;
    }
    Storage.writeFile(IDLE_KEY, String(jpgKey.c_str()));  // written last: a half-written cache is never trusted
  }
  return true;
}

// Draws the cached idle image into the framebuffer. False if it cannot be read.
bool drawIdleImage(GfxRenderer& r) {
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
bool presentOnRight(GfxRenderer& r, HalDisplay& d, DrawFn&& draw, bool followNightMode = false) {
  // Full waveform: the card / idle image sits there until the book (or the polarity) changes.
  return RightPanel::present(r, d, std::forward<DrawFn>(draw), HalDisplay::HALF_REFRESH, followNightMode);
}

// The frame last shown on the right panel in a polarity-following mode (home card, reader page), so a night-mode toggle
// can show it again without re-rendering. One persistent buffer (panel size, ~39 KB), allocated once on first use,
// preferably in PSRAM, and never freed; if it cannot be allocated the toggle simply takes effect on the next redraw.
uint8_t* storedFrame = nullptr;
size_t storedCapacity = 0;
size_t storedBytes = 0;
bool storedValid = false;
bool storedInverted = false;

// Cache key of a card: the book plus the polarity it was drawn in.
uint32_t cardKey(bool hasBook, const std::string& path, bool inverted) {
  const uint32_t key = (hasBook ? hashString(path) : 1u) ^ (inverted ? 0x9E3779B9u : 0u);
  return key ? key : 1u;
}

}  // namespace

namespace RightPanel {

void showCoverCardIfChanged(GfxRenderer& renderer, HalDisplay& display) {
  if (!BoardConfig::isDiptyx()) return;
  BookCard card;
  const bool hasBook = currentBook(card);
  const uint32_t key = cardKey(hasBook, card.path, display.isInverted());
  if (key == shownKey) return;

  LOG_DBG("RP", "Cover card for %s", hasBook ? card.path.c_str() : "(no book)");
  if (hasBook) resolveCover(card);
  // Only remember the card as shown if it really was (no memory to save the left frame: try again next visit).
  if (presentOnRight(renderer, display, [&] { drawBookCard(renderer, card); }, /*followNightMode=*/true)) {
    shownKey = key;
    shownCard = card;
    shownCardValid = true;
  }
}

bool refreshPolarity(GfxRenderer& renderer, HalDisplay& display) {
  if (!BoardConfig::isDiptyx() || !storedValid) return true;
  const bool inverted = display.isInverted();
  if (inverted == storedInverted) return true;
  const size_t bytes = storedBytes;
  // present() re-remembers the same frame, now in the new polarity.
  if (!presentOnRight(
          renderer, display, [&] { memcpy(display.getFrameBuffer(), storedFrame, bytes); }, /*followNightMode=*/true)) {
    return false;
  }
  if (shownKey != 0 && shownCardValid) {
    shownKey = cardKey(!shownCard.path.empty(), shownCard.path, inverted);  // the card is on the panel in this polarity
  }
  return true;
}

namespace detail {

void rememberFrame(const uint8_t* frame, size_t bytes, bool inverted) {
  if (!frame || bytes == 0) return;
  if (!storedFrame || storedCapacity < bytes) {
    // Persistent by design (see storedFrame): allocate once, PSRAM first.
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    if (!buffer) {
      LOG_ERR("RP", "OOM: %u bytes for the right-panel frame copy", static_cast<unsigned>(bytes));
      storedValid = false;
      return;
    }
    if (storedFrame) heap_caps_free(storedFrame);
    storedFrame = buffer;
    storedCapacity = bytes;
  }
  if (frame != storedFrame) memcpy(storedFrame, frame, bytes);
  storedBytes = bytes;
  storedInverted = inverted;
  storedValid = true;
}

void forgetFrame() { storedValid = false; }

}  // namespace detail

void markDirty() { shownKey = 0; }

void showSleepScreen(GfxRenderer& renderer, HalDisplay& display) {
  if (!BoardConfig::isDiptyx()) return;
  BookCard card;
  // Title & Author follows the left screen's cover rule: only a book that is open right now (sleeping from the home
  // screen has none); otherwise this falls back to the idle image or the card.
  const bool quiet = SETTINGS.rightSleepScreen == CrossPointSettings::RIGHT_SLEEP_TITLE_AUTHOR &&
                     !APP_STATE.openEpubPath.empty() && currentBook(card) && !card.title.empty();
  // Everything that decodes or opens a book happens before present(), while the heap is not yet short by the parked
  // left frame; the draw callback only paints.
  const bool idleReady = !quiet && ensureIdleCache(renderer);
  const bool cardFromBook = !quiet && !idleReady && currentBook(card);
  if (cardFromBook) resolveCover(card);
  presentOnRight(renderer, display, [&] {
    if (quiet) {
      drawQuietTitle(renderer, card);
    } else if (!(idleReady && drawIdleImage(renderer))) {
      drawBookCard(renderer, card);
    }
  });
  shownKey = 0;  // force the home card to be redrawn after the next wake
}

}  // namespace RightPanel
