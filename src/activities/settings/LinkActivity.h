#pragma once

#include <HalLink.h>
#include <UnitLinkSession.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Pairs this unit with a second X4 Pro over the pogo data pads. The unit picks a side (left page or right page), shows
// the pairing status and a test page number that the left unit sends to the right one. The link cannot be ended live
// (the pads are the USB lines), so leaving restarts the unit.
class LinkActivity final : public Activity {
 public:
  explicit LinkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Link", renderer, mappedInput), session(link) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == State::Running; }
  bool preventAutoSleep() override { return state == State::Running; }

 private:
  enum class State : uint8_t { ChooseSide, Running };

  void startLink(unitlink::Role chosen);
  void endLink();

  State state = State::ChooseSide;
  unitlink::Role role = unitlink::Role::Left;
  OptionPopup sidePopup;
  HalLink link;
  unitlink::Session session;

  uint16_t testPage = 0;       // left: the number being sent; right: the last number received
  bool testPageKnown = false;  // right unit: something has been received
  unitlink::Status shownStatus = unitlink::Status::Idle;
  uint16_t shownPage = 0;
  bool shownAcked = true;
  bool leaving = false;
};
