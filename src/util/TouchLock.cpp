#include "TouchLock.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointState.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

extern GfxRenderer renderer;

namespace {
constexpr unsigned long HOLD_MS = 2000;
constexpr unsigned long POPUP_MS = 1500;

unsigned long bothHeldSince = 0;
bool gestureFired = false;
bool holdActive = false;        // the gesture has hidden the page buttons from the rest of the firmware
bool releasePassSeen = false;  // both buttons are up and that pass has run
unsigned long popupClearAt = 0;
}  // namespace

namespace touchlock {

bool locked() { return gpio.touchLocked(); }

void restore() {
  if (BoardConfig::hasHomeKey()) gpio.setTouchLocked(APP_STATE.touchLocked != 0);
}

void set(const bool on) {
  if (!BoardConfig::hasHomeKey() || gpio.touchLocked() == on) return;
  gpio.setTouchLocked(on);
  APP_STATE.touchLocked = on ? 1 : 0;
  APP_STATE.saveToFile();
  LOG_INF("TOUCH", "Touch %s", on ? "locked" : "unlocked");
  {
    RenderLock lock;  // the render task shares the framebuffer
    GUI.drawPopup(renderer, on ? tr(STR_TOUCH_LOCKED) : tr(STR_TOUCH_UNLOCKED));
  }
  popupClearAt = millis() + POPUP_MS;
}

void tick() {
  if (!BoardConfig::hasHomeKey()) return;
  const unsigned long now = millis();
  if (popupClearAt != 0 && now >= popupClearAt) {
    popupClearAt = 0;
    activityManager.requestUpdate();  // draws the screen again over the popup
  }
  const bool leftDown = gpio.rawPressed(HalGPIO::BTN_LEFT) || gpio.rawPressed(HalGPIO::BTN_UP);
  const bool rightDown = gpio.rawPressed(HalGPIO::BTN_RIGHT) || gpio.rawPressed(HalGPIO::BTN_DOWN);
  if (leftDown && rightDown) {
    releasePassSeen = false;
    if (!holdActive) {
      holdActive = true;
      gpio.setNavGestureHold(true);
    }
    if (bothHeldSince == 0) {
      bothHeldSince = now;
      gestureFired = false;
    } else if (!gestureFired && now - bothHeldSince >= HOLD_MS) {
      gestureFired = true;
      set(!gpio.touchLocked());
    }
    return;
  }
  bothHeldSince = 0;
  // The buttons stay hidden until both are up, and for one more pass: the pass in which they went up still carries
  // their release edges, which must not turn a page.
  if (holdActive && !leftDown && !rightDown) {
    if (releasePassSeen) {
      gpio.setNavGestureHold(false);
      holdActive = false;
      releasePassSeen = false;
    } else {
      releasePassSeen = true;
    }
  }
}

}  // namespace touchlock
