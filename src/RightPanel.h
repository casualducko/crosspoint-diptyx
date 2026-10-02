#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

// Diptyx second (right) panel, shown while the left panel runs the normal UI. No-ops on every other board.
//
// Each call saves the left panel's frame, draws into the shared framebuffer, refreshes the RIGHT panel, and puts the
// left frame back, so the left activity is unaffected.
namespace RightPanel {

// Home screen: show a card for the current book (cover when it can be decoded, otherwise title and author). Skips the
// refresh when the panel already shows this book's card.
void showCoverCardIfChanged(GfxRenderer& renderer, HalDisplay& display);

// The right panel now shows something else (a reader page): make the home screen redraw its card next time.
void markDirty();

// Sleep: show the stock idle image (/idle_screen_right.jpg on the SD card) if present, otherwise the cover card.
// Always refreshes.
void showSleepScreen(GfxRenderer& renderer, HalDisplay& display);

// Run `draw` into the shared framebuffer and show the result on the right panel with `mode`, leaving the left panel's
// frame (and the framebuffer contents) as they were. Returns false if there was no memory to save the left frame.
// The card and idle image are always drawn in normal polarity; reader pages (keepInversion) follow the left panel's
// night-mode state, because toggling it would mismatch the pair and force a full flash on every turn.
template <typename DrawFn>
bool present(GfxRenderer& r, HalDisplay& d, DrawFn&& draw, HalDisplay::RefreshMode mode, bool keepInversion = false) {
  if (!r.storeBwBuffer()) {
    LOG_ERR("RP", "No memory to save the left frame; skipping right panel");
    return false;
  }
  const auto orientation = r.getOrientation();
  r.setOrientation(GfxRenderer::Orientation::Portrait);
  const bool flipPolarity = d.isInverted() && !keepInversion;
  if (flipPolarity) d.setInverted(false);

  draw();

  d.selectPanel(HalDisplay::Panel::Right);
  r.displayBuffer(mode);
  d.selectPanel(HalDisplay::Panel::Left);

  if (flipPolarity) d.setInverted(true);
  r.setOrientation(orientation);
  r.restoreBwBuffer(/*resyncPanelBaseline=*/false);
  return true;
}

}  // namespace RightPanel
