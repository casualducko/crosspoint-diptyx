#pragma once

#include <HalLink.h>
#include <I18n.h>
#include <UnitLinkSession.h>

#include <string>

class GfxRenderer;

// The link to a second unit, kept alive across screens: the Settings screen starts it, the reader tells it which book
// is open and drives it. The pads are the USB lines, so the link ends only by restarting the unit.
class UnitLinkService {
 public:
  static UnitLinkService& get();

  bool running() const { return link.active(); }
  unitlink::Role role() const { return session.role(); }
  unitlink::Session& linkSession() { return session; }

  // Starts with no book: both units pair as soon as they see each other, and setBook() narrows that to the open book.
  bool start(unitlink::Role role, const GfxRenderer& renderer);
  // The reader opened `epubPath`, or closed its book (empty path). The text layout is taken from the current settings.
  void setBook(const std::string& epubPath, const GfxRenderer& renderer);
  // The text settings may have changed: announce the layout again if its hash moved.
  void refreshLayout(const GfxRenderer& renderer);
  // What to tell the user about the link right now (STR_LINK_LINKED when all is well).
  StrId statusMessage() const;
  // Changes whenever the status or the reason for a mismatch changes.
  uint8_t statusKey() const { return static_cast<uint8_t>(session.status()) | (session.incompatibleReasons() << 3); }
  // Call once per main loop pass.
  void tick();
  // Ends the link and restarts the unit so USB comes back.
  void endAndRestart();

  // Both units must derive the same ids from the same files and settings.
  static uint32_t bookIdFor(const std::string& epubPath);
  static uint32_t layoutHashFor(const GfxRenderer& renderer);

 private:
  UnitLinkService() : session(link) {}

  HalLink link;
  void openRequestedBook(const std::string& path);

  uint32_t currentBookId = 0;
  std::string lastRequestedPath;
  unsigned long lastRequestMs = 0;
  unitlink::Session session;
  bool ending = false;
};
