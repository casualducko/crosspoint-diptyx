#pragma once

#include <UnitLinkSession.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Pairs this unit with a second X4 Pro over the pogo data pads. The unit picks a side (left page or right page), shows
// the pairing status and a test page number that the left unit sends to the right one. The link itself lives in
// UnitLinkService: Confirm leaves this screen and keeps it running, Back ends it, which restarts the unit (the pads are
// the USB lines).
class LinkActivity final : public Activity {
 public:
  explicit LinkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Link", renderer, mappedInput) {}

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
  unitlink::Session* session = nullptr;  // the service's session, set once the link runs

  uint16_t testPage = 0;       // left: the number being sent; right: the last number received
  bool testPageKnown = false;  // right unit: something has been received
  unitlink::Status shownStatus = unitlink::Status::Idle;
  uint16_t shownPage = 0;
  bool shownAcked = true;
  unsigned long lastCountersDrawMs = 0;
};
