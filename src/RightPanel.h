#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>

// Diptyx second (right) panel, shown while the left panel runs the normal UI. No-ops on every other board.
//
// Each call saves the left panel's frame, draws into the shared framebuffer, refreshes the RIGHT panel, and puts the
// left frame back, so the left activity is unaffected.
namespace RightPanel {

// Home screen: show a card for the current book (cover when it can be decoded, otherwise title and author), following
// night mode. Skips the refresh when the panel already shows this book's card in the current polarity.
void showCoverCardIfChanged(GfxRenderer& renderer, HalDisplay& display);

// The left panel's night-mode setting changed: show the right panel's last frame again in the new polarity. No
// re-render is needed (the display inverts at output), so this works whatever activity or menu is on top, for the home
// card and for a reader page alike. No-op when the right panel shows the idle image or nothing known.
// Returns false only when the redraw failed (e.g. out of memory), so the caller tries again on the next render.
bool refreshPolarity(GfxRenderer& renderer, HalDisplay& display);

namespace detail {
// Keep a copy of the frame just drawn for the right panel (polarity-following content), or forget it.
void rememberFrame(const uint8_t* frame, size_t bytes, bool inverted);
void forgetFrame();
}  // namespace detail

// The right panel now shows something else (a reader page): make the home screen redraw its card next time.
void markDirty();

// Sleep: show the stock idle image (/idle_screen_right.jpg on the SD card) if present, otherwise the cover card.
// Always refreshes.
void showSleepScreen(GfxRenderer& renderer, HalDisplay& display);

// Run `draw` into the shared framebuffer and show the result on the right panel with `mode`, leaving the left panel's
// frame (and the framebuffer contents) as they were. Returns false if there was no memory to save the left frame.
// By default the content is drawn in normal polarity (the sleep screen and idle image, like CrossPoint's own sleep
// screens); with keepInversion it follows the left panel's night-mode state (reader pages, and the home card), because
// toggling it would mismatch the pair and force a full flash on every turn.
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

  // Polarity-following content is remembered so a later night-mode toggle can show it again without a re-render.
  if (keepInversion) {
    detail::rememberFrame(d.getFrameBuffer(), static_cast<size_t>(r.getDisplayWidthBytes()) * r.getDisplayHeight(),
                          d.isInverted());
  } else {
    detail::forgetFrame();
  }

  d.selectPanel(HalDisplay::Panel::Right);
  r.displayBuffer(mode);
  d.selectPanel(HalDisplay::Panel::Left);

  if (flipPolarity) d.setInverted(true);
  r.setOrientation(orientation);
  r.restoreBwBuffer(/*resyncPanelBaseline=*/false);
  return true;
}

}  // namespace RightPanel
