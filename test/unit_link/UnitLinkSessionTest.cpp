#include <gtest/gtest.h>

#include <deque>

#include "UnitLinkSession.h"

using namespace unitlink;

namespace {

// One direction of the cable: whole frames can be lost or have a byte flipped, deterministically.
struct Pipe {
  std::deque<uint8_t> bytes;
  uint32_t lossPercent = 0;
  uint32_t corruptPercent = 0;
  uint32_t rng = 12345;

  uint32_t next() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 16;
  }
  void put(const uint8_t* d, size_t n) {
    if (next() % 100 < lossPercent) return;
    const bool corrupt = next() % 100 < corruptPercent;
    for (size_t i = 0; i < n; i++) bytes.push_back(corrupt && i == n / 2 ? static_cast<uint8_t>(d[i] ^ 0x5A) : d[i]);
  }
};

class End : public Port {
 public:
  End(Pipe& out, Pipe& in) : outPipe(out), inPipe(in) {}
  size_t write(const uint8_t* d, size_t n) override {
    outPipe.put(d, n);
    return n;
  }
  int read() override {
    if (inPipe.bytes.empty()) return -1;
    const int c = inPipe.bytes.front();
    inPipe.bytes.pop_front();
    return c;
  }

 private:
  Pipe& outPipe;
  Pipe& inPipe;
};

struct Rig {
  Pipe l2r, r2l;
  End leftEnd{l2r, r2l};
  End rightEnd{r2l, l2r};
  Session left{leftEnd};
  Session right{rightEnd};
  uint32_t now = 1000;

  Config cfg(Role role, uint32_t book = 7, uint32_t layout = 9) {
    Config c;
    c.role = role;
    c.bookId = book;
    c.layoutHash = layout;
    return c;
  }
  void startBoth() {
    left.start(cfg(Role::Left), now);
    right.start(cfg(Role::Right), now);
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      now += 10;
      left.update(now);
      right.update(now);
    }
  }
};

}  // namespace

TEST(UnitLinkSessionTest, PairsWithinAFewHundredMilliseconds) {
  Rig r;
  r.startBoth();
  EXPECT_EQ(r.left.status(), Status::Pairing);
  r.run(200);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, PairsWhenOneUnitStartsLater) {
  Rig r;
  r.left.start(r.cfg(Role::Left), r.now);
  r.run(3000);
  EXPECT_EQ(r.left.status(), Status::Pairing);
  r.right.start(r.cfg(Role::Right), r.now);
  r.run(1000);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, DifferentBookDoesNotPair) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 1), r.now);
  r.right.start(r.cfg(Role::Right, 2), r.now);
  r.run(1000);
  EXPECT_EQ(r.left.status(), Status::Incompatible);
  EXPECT_EQ(r.right.status(), Status::Incompatible);
  EXPECT_EQ(r.left.incompatibleReasons(), INCOMPATIBLE_BOOK);
}

TEST(UnitLinkSessionTest, DifferentLayoutAndSameSideDoNotPair) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 7, 1), r.now);
  r.right.start(r.cfg(Role::Right, 7, 2), r.now);
  r.run(1000);
  EXPECT_EQ(r.right.incompatibleReasons(), INCOMPATIBLE_LAYOUT);

  Rig s;
  s.left.start(s.cfg(Role::Left), s.now);
  s.right.start(s.cfg(Role::Left), s.now);
  s.run(1000);
  EXPECT_EQ(s.left.incompatibleReasons(), INCOMPATIBLE_ROLE);
}

TEST(UnitLinkSessionTest, PositionIsDeliveredAckedAndTakenOnce) {
  Rig r;
  r.startBoth();
  r.run(200);
  r.left.publish(3, 41, r.now);
  r.run(100);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 3);
  EXPECT_EQ(s.page, 41);
  EXPECT_FALSE(r.right.takeRemote(s));
  EXPECT_TRUE(r.left.publishedIsAcked());
}

TEST(UnitLinkSessionTest, NewerPositionReplacesOlder) {
  Rig r;
  r.startBoth();
  r.run(200);
  r.left.publish(1, 10, r.now);
  r.left.publish(1, 12, r.now);
  r.run(100);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.page, 12);
  EXPECT_FALSE(r.right.takeRemote(s));
}

TEST(UnitLinkSessionTest, PositionPublishedBeforePairingArrivesAfterPairing) {
  Rig r;
  r.left.start(r.cfg(Role::Left), r.now);
  r.left.publish(2, 5, r.now);
  r.run(1500);
  r.right.start(r.cfg(Role::Right), r.now);
  r.run(1500);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 2);
  EXPECT_EQ(s.page, 5);
  EXPECT_TRUE(r.left.publishedIsAcked());
}

TEST(UnitLinkSessionTest, SurvivesLossyAndNoisyCable) {
  Rig r;
  r.l2r.lossPercent = r.r2l.lossPercent = 40;
  r.l2r.corruptPercent = r.r2l.corruptPercent = 10;
  r.startBoth();
  r.run(8000);
  ASSERT_TRUE(r.left.isLinked());
  ASSERT_TRUE(r.right.isLinked());
  uint16_t lastSeen = 0;
  for (uint16_t page = 1; page <= 20; page++) {
    r.left.publish(1, page, r.now);
    PageState s;
    // Delivered and acked within ten seconds even though almost half of the frames never arrive intact.
    for (int step = 0; step < 100 && !(lastSeen == page && r.left.publishedIsAcked()); step++) {
      r.run(100);
      while (r.right.takeRemote(s)) lastSeen = s.page;
    }
    EXPECT_EQ(lastSeen, page);
    EXPECT_TRUE(r.left.publishedIsAcked());
  }
}

TEST(UnitLinkSessionTest, SilentPeerMarksLinkLostAndItRecovers) {
  Rig r;
  r.startBoth();
  r.run(500);
  ASSERT_TRUE(r.left.isLinked());
  // The right unit stops answering (no update calls) for longer than the timeout.
  for (uint32_t t = 0; t < 4000; t += 10) {
    r.now += 10;
    r.left.update(r.now);
  }
  EXPECT_EQ(r.left.status(), Status::Lost);
  r.r2l.bytes.clear();
  r.run(3000);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, RestartedRightUnitGetsThePositionAgain) {
  Rig r;
  r.startBoth();
  r.run(300);
  r.left.publish(4, 77, r.now);
  r.run(300);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  r.right.start(r.cfg(Role::Right), r.now);  // the right unit rebooted and forgot everything
  r.run(2000);
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 4);
  EXPECT_EQ(s.page, 77);
}

TEST(UnitLinkSessionTest, SleepRequestReachesThePeerOnce) {
  Rig r;
  r.startBoth();
  r.run(300);
  r.left.sendSleep();
  r.run(100);
  EXPECT_TRUE(r.right.takeSleepRequest());
  EXPECT_FALSE(r.right.takeSleepRequest());
  EXPECT_FALSE(r.left.takeSleepRequest());
}

TEST(UnitLinkSessionTest, LineGarbageDoesNotBreakTheLink) {
  Rig r;
  r.startBoth();
  r.run(300);
  for (int i = 0; i < 300; i++) r.l2r.bytes.push_back(static_cast<uint8_t>(r.l2r.next()));
  r.left.publish(1, 2, r.now);
  r.run(500);
  PageState s;
  EXPECT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.page, 2);
}

TEST(UnitLinkSessionTest, SequenceNumberWrapsAround) {
  Rig r;
  r.startBoth();
  r.run(300);
  PageState s;
  uint16_t last = 0;
  for (uint32_t i = 1; i <= 70000; i++) {
    r.left.publish(1, static_cast<uint16_t>(i & 0xFFFF), r.now);
    r.run(20);
    while (r.right.takeRemote(s)) last = s.page;
  }
  EXPECT_EQ(last, static_cast<uint16_t>(70000 & 0xFFFF));
}

TEST(UnitLinkSessionTest, UnitsPairAfterBothOpenTheSameBook) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 0, 9), r.now);
  r.right.start(r.cfg(Role::Right, 0, 9), r.now);
  r.run(500);
  EXPECT_TRUE(r.left.isLinked());
  r.left.setIdentity(5, 9, r.now);
  r.run(500);
  EXPECT_EQ(r.right.status(), Status::Incompatible);
  r.right.setIdentity(5, 9, r.now);
  r.run(1000);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, PositionIsSentAgainAfterTheBookChanged) {
  Rig r;
  r.startBoth();
  r.run(300);
  r.left.setIdentity(5, 9, r.now);
  r.right.setIdentity(5, 9, r.now);
  r.run(1000);
  r.left.publish(3, 40, r.now);
  r.run(300);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 3);
  EXPECT_EQ(s.page, 40);
}

TEST(UnitLinkSessionTest, RightUnitIsToldWhichBookTheLeftUnitHasOpen) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 5), r.now);
  r.right.start(r.cfg(Role::Right, 0), r.now);
  r.left.setBookPath("/Books/Dune.epub");
  r.run(1500);
  EXPECT_EQ(r.right.status(), Status::Incompatible);
  std::string path;
  ASSERT_TRUE(r.right.takeBookRequest(path));
  EXPECT_EQ(path, "/Books/Dune.epub");
  r.right.setIdentity(5, 9, r.now);
  r.run(1500);
  EXPECT_TRUE(r.right.isLinked());
  EXPECT_FALSE(r.right.takeBookRequest(path));  // nothing more is sent once the books match
}

TEST(UnitLinkSessionTest, NoBookIsSentWithoutAPathOrToTheLeftUnit) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 5), r.now);
  r.right.start(r.cfg(Role::Right, 0), r.now);
  r.run(1500);
  std::string path;
  EXPECT_FALSE(r.right.takeBookRequest(path));
  r.right.setBookPath("/Books/Other.epub");
  r.run(1500);
  EXPECT_FALSE(r.left.takeBookRequest(path));  // the right unit does not offer its book to a left unit that has one
}

TEST(UnitLinkSessionTest, LeftUnitIsToldWhichBookTheRightUnitHasOpen) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 0), r.now);
  r.right.start(r.cfg(Role::Right, 5), r.now);
  r.right.setBookPath("/Books/Dune.epub");
  r.run(1500);
  std::string path;
  ASSERT_TRUE(r.left.takeBookRequest(path));
  EXPECT_EQ(path, "/Books/Dune.epub");
  EXPECT_FALSE(r.right.takeBookRequest(path));
}

TEST(UnitLinkSessionTest, TwoDifferentBooksSettleOnTheLeftUnitsBook) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 4), r.now);
  r.right.start(r.cfg(Role::Right, 5), r.now);
  r.left.setBookPath("/Books/A.epub");
  r.right.setBookPath("/Books/B.epub");
  r.run(1500);
  std::string path;
  EXPECT_FALSE(r.left.takeBookRequest(path));
  ASSERT_TRUE(r.right.takeBookRequest(path));
  EXPECT_EQ(path, "/Books/A.epub");
}

TEST(UnitLinkSessionTest, PositionPublishedBeforeTheLinkIsUpIsSentWhenItComesUp) {
  // The right unit already has book 5 open; the left unit opens it, publishes its page, and only then pairs.
  Rig r;
  r.left.start(r.cfg(Role::Left, 0), r.now);
  r.right.start(r.cfg(Role::Right, 5), r.now);
  r.run(1000);
  EXPECT_EQ(r.right.status(), Status::Incompatible);
  r.left.setIdentity(5, 9, r.now);
  r.left.publish(2, 30, r.now);
  r.run(1500);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.page, 30);
}

namespace {
std::string makeMarks(size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) s.push_back(static_cast<char>('a' + (i * 7) % 26));
  return s;
}
}  // namespace

TEST(UnitLinkSessionTest, BookmarkFileArrivesWholeInSeveralChunks) {
  Rig r;
  r.startBoth();
  r.run(1500);
  const std::string file = makeMarks(1000);  // five chunks
  ASSERT_TRUE(r.left.publishMarks(file, r.now));
  r.run(500);
  std::string got;
  ASSERT_TRUE(r.right.takeMarks(got));
  EXPECT_EQ(got, file);
  EXPECT_FALSE(r.right.takeMarks(got));
  EXPECT_TRUE(r.left.marksDelivered());
}

TEST(UnitLinkSessionTest, BookmarkFileIsHandedOverOnlyOnceAndStopsRepeatingAfterTheAck) {
  Rig r;
  r.startBoth();
  r.run(1500);
  ASSERT_TRUE(r.left.publishMarks(makeMarks(300), r.now));
  r.run(300);
  std::string got;
  ASSERT_TRUE(r.right.takeMarks(got));
  r.run(6000);  // no repeats: the left unit has its ack
  EXPECT_FALSE(r.right.takeMarks(got));
}

TEST(UnitLinkSessionTest, BookmarkFileSurvivesLostFrames) {
  Rig r;
  r.l2r.lossPercent = 30;
  r.r2l.lossPercent = 30;
  r.startBoth();
  r.run(8000);
  ASSERT_TRUE(r.left.isLinked());
  const std::string file = makeMarks(900);
  ASSERT_TRUE(r.left.publishMarks(file, r.now));
  r.run(60000);
  std::string got;
  ASSERT_TRUE(r.right.takeMarks(got));
  EXPECT_EQ(got, file);
  EXPECT_TRUE(r.left.marksDelivered());
}

TEST(UnitLinkSessionTest, NewerBookmarkFileReplacesOneOnItsWay) {
  Rig r;
  r.startBoth();
  r.run(1500);
  ASSERT_TRUE(r.left.publishMarks(makeMarks(800), r.now));
  r.run(50);  // part of it has gone out
  const std::string second = makeMarks(500) + "x";
  ASSERT_TRUE(r.left.publishMarks(second, r.now));
  r.run(1000);
  std::string got;
  ASSERT_TRUE(r.right.takeMarks(got));
  EXPECT_EQ(got, second);
}

TEST(UnitLinkSessionTest, EmptyOrOversizedBookmarkFilesAreRefusedAndTheRightUnitCannotSend) {
  Rig r;
  r.startBoth();
  EXPECT_FALSE(r.left.publishMarks("", r.now));
  EXPECT_FALSE(r.left.publishMarks(std::string(MARKS_MAX_TOTAL + 1, 'x'), r.now));
  r.run(1500);
  // A file from the right unit's side is ignored by the left unit.
  ASSERT_TRUE(r.right.publishMarks("{}", r.now));
  r.run(500);
  std::string got;
  EXPECT_FALSE(r.left.takeMarks(got));
}

TEST(UnitLinkSessionTest, RestartedRightUnitCountsAsFreshAndGetsTheFileAgain) {
  Rig r;
  r.startBoth();
  r.run(1500);
  const uint32_t before = r.left.peerFreshCount();
  ASSERT_TRUE(r.left.publishMarks(makeMarks(250), r.now));
  r.run(500);
  std::string got;
  ASSERT_TRUE(r.right.takeMarks(got));
  r.right.start(r.cfg(Role::Right), r.now);  // the right unit restarts
  r.run(1500);
  EXPECT_TRUE(r.left.peerFreshCount() > before);
  // The service would now publish again; the restarted unit has no memory of the file.
  ASSERT_TRUE(r.left.publishMarks(makeMarks(250), r.now));
  r.run(500);
  EXPECT_TRUE(r.right.takeMarks(got));
}

TEST(UnitLinkSessionTest, RightUnitsKeyPressesReachTheLeftUnitInOrderAndOnlyOnce) {
  Rig r;
  r.startBoth();
  r.run(1500);
  ASSERT_TRUE(r.right.sendKey(KeyAction::NextPage, r.now));
  ASSERT_TRUE(r.right.sendKey(KeyAction::NextPage, r.now));
  ASSERT_TRUE(r.right.sendKey(KeyAction::PrevPage, r.now));
  r.run(3000);
  KeyAction a;
  ASSERT_TRUE(r.left.takeKey(a));
  EXPECT_EQ(a, KeyAction::NextPage);
  ASSERT_TRUE(r.left.takeKey(a));
  EXPECT_EQ(a, KeyAction::NextPage);
  ASSERT_TRUE(r.left.takeKey(a));
  EXPECT_EQ(a, KeyAction::PrevPage);
  EXPECT_FALSE(r.left.takeKey(a));
}

TEST(UnitLinkSessionTest, KeyPressesSurviveLostFramesWithoutTurningTwice) {
  Rig r;
  r.l2r.lossPercent = 25;
  r.r2l.lossPercent = 25;
  r.startBoth();
  r.run(10000);
  ASSERT_TRUE(r.right.isLinked());
  int sent = 0, got = 0;
  for (int i = 0; i < 12; i++) {
    if (r.right.sendKey(KeyAction::NextPage, r.now)) sent++;
    r.run(3000);
    KeyAction a;
    while (r.left.takeKey(a)) got++;
  }
  // A press is never turned twice; one lost with the link (forgotten on purpose) may be missing.
  EXPECT_TRUE(sent > 0);
  EXPECT_TRUE(got <= sent);
  EXPECT_TRUE(got >= sent - 3);
}

TEST(UnitLinkSessionTest, KeyPressesAreRefusedWithoutALinkAndForgottenWhenItDrops) {
  Rig r;
  r.startBoth();
  EXPECT_FALSE(r.right.sendKey(KeyAction::NextPage, r.now));  // not linked yet
  r.run(1500);
  EXPECT_FALSE(r.left.sendKey(KeyAction::NextPage, r.now));  // only the right unit sends them
  ASSERT_TRUE(r.right.sendKey(KeyAction::NextPage, r.now));
  r.l2r.lossPercent = 100;  // the left unit's answers never arrive
  r.r2l.lossPercent = 100;
  r.run(6000);
  KeyAction a;
  while (r.left.takeKey(a)) {
  }
  r.l2r.lossPercent = 0;
  r.r2l.lossPercent = 0;
  r.run(3000);
  EXPECT_FALSE(r.left.takeKey(a));  // the press from before the drop is not replayed
}

TEST(UnitLinkSessionTest, RestartedRightUnitsFirstPressIsNotMistakenForARepeat) {
  Rig r;
  r.startBoth();
  r.run(1500);
  ASSERT_TRUE(r.right.sendKey(KeyAction::NextPage, r.now));
  r.run(1000);
  KeyAction a;
  ASSERT_TRUE(r.left.takeKey(a));
  r.right.start(r.cfg(Role::Right), r.now);
  r.run(1500);
  ASSERT_TRUE(r.right.sendKey(KeyAction::PrevPage, r.now));  // seq 1 again, as before the restart
  r.run(1000);
  ASSERT_TRUE(r.left.takeKey(a));
  EXPECT_EQ(a, KeyAction::PrevPage);
}

namespace {
void fillBlob(uint8_t* b, uint8_t v) {
  for (size_t i = 0; i < LAYOUT_BLOB_LEN; i++) b[i] = static_cast<uint8_t>(v + i);
}
}  // namespace

TEST(UnitLinkSessionTest, TextSettingsGoEitherWayAndAreConfirmed) {
  Rig r;
  r.startBoth();
  r.run(1500);
  uint8_t a[LAYOUT_BLOB_LEN], got[LAYOUT_BLOB_LEN];
  fillBlob(a, 3);
  ASSERT_TRUE(r.left.publishLayout(a, r.now));
  r.run(300);
  ASSERT_TRUE(r.right.takeLayout(got));
  EXPECT_EQ(memcmp(a, got, sizeof(a)), 0);
  EXPECT_FALSE(r.left.layoutUnconfirmed());
  uint8_t b[LAYOUT_BLOB_LEN];
  fillBlob(b, 90);
  ASSERT_TRUE(r.right.publishLayout(b, r.now));
  r.run(300);
  ASSERT_TRUE(r.left.takeLayout(got));
  EXPECT_EQ(memcmp(b, got, sizeof(b)), 0);
  EXPECT_FALSE(r.right.layoutUnconfirmed());
}

TEST(UnitLinkSessionTest, TextSettingsAreHandedOverOnceAndComeBackWhenTheyChangeBack) {
  Rig r;
  r.startBoth();
  r.run(1500);
  uint8_t a[LAYOUT_BLOB_LEN], b[LAYOUT_BLOB_LEN], got[LAYOUT_BLOB_LEN];
  fillBlob(a, 3);
  fillBlob(b, 50);
  ASSERT_TRUE(r.left.publishLayout(a, r.now));
  r.run(300);
  ASSERT_TRUE(r.right.takeLayout(got));
  r.run(2000);
  EXPECT_FALSE(r.right.takeLayout(got));         // repeats are confirmed, not handed over again
  ASSERT_TRUE(r.right.publishLayout(b, r.now));  // the right unit changes its settings ...
  r.run(300);
  ASSERT_TRUE(r.left.takeLayout(got));
  ASSERT_TRUE(r.left.publishLayout(a, r.now));  // ... and the left unit sets the first ones again
  r.run(300);
  EXPECT_TRUE(r.right.takeLayout(got));
}

TEST(UnitLinkSessionTest, TextSettingsSurviveLostFramesAndGiveUpWithoutAPeer) {
  Rig r;
  r.l2r.lossPercent = 40;
  r.r2l.lossPercent = 40;
  r.startBoth();
  r.run(10000);
  uint8_t a[LAYOUT_BLOB_LEN], got[LAYOUT_BLOB_LEN];
  fillBlob(a, 7);
  ASSERT_TRUE(r.left.publishLayout(a, r.now));
  r.run(8000);
  EXPECT_TRUE(r.right.takeLayout(got));
  EXPECT_EQ(memcmp(a, got, sizeof(a)), 0);

  Rig lonely;
  lonely.left.start(lonely.cfg(Role::Left), lonely.now);
  ASSERT_TRUE(lonely.left.publishLayout(a, lonely.now));
  for (uint32_t t = 0; t < 12000; t += 10) {
    lonely.now += 10;
    lonely.left.update(lonely.now);
  }
  EXPECT_FALSE(lonely.left.layoutUnconfirmed());
}

TEST(UnitLinkSessionTest, WhenBothChangeAtOnceTheLeftUnitsSettingsWin) {
  Rig r;
  r.startBoth();
  r.run(1500);
  uint8_t a[LAYOUT_BLOB_LEN], b[LAYOUT_BLOB_LEN], got[LAYOUT_BLOB_LEN];
  fillBlob(a, 3);
  fillBlob(b, 90);
  ASSERT_TRUE(r.left.publishLayout(a, r.now));
  ASSERT_TRUE(r.right.publishLayout(b, r.now));
  r.run(1500);
  EXPECT_FALSE(r.left.takeLayout(got));  // the left unit keeps its own
  ASSERT_TRUE(r.right.takeLayout(got));
  EXPECT_EQ(memcmp(a, got, sizeof(a)), 0);
  EXPECT_FALSE(r.left.layoutUnconfirmed());
  EXPECT_FALSE(r.right.layoutUnconfirmed());  // the right unit stopped sending its own
}

namespace {
DisplayState nightOnly(bool night) {
  DisplayState d;
  d.night = night;
  return d;
}
}  // namespace

TEST(UnitLinkSessionTest, DisplayStateChangedOnEitherUnitReachesTheOther) {
  Rig r;
  r.startBoth();
  r.run(1500);
  DisplayState got;
  EXPECT_FALSE(r.left.takeDisplay(got));
  EXPECT_FALSE(r.right.takeDisplay(got));
  DisplayState bright = nightOnly(true);
  bright.lightOn = true;
  bright.brightness = 65;
  bright.warmth = 30;
  r.left.setDisplay(bright, r.now);
  r.run(1500);
  ASSERT_TRUE(r.right.takeDisplay(got));
  EXPECT_TRUE(got == bright);
  r.right.setDisplay(got, r.now);  // what the service does after adopting it
  r.run(3000);
  EXPECT_FALSE(r.left.takeDisplay(got));  // no echo
  EXPECT_FALSE(r.right.takeDisplay(got));
  DisplayState dimmer = bright;
  dimmer.brightness = 20;
  r.right.setDisplay(dimmer, r.now);
  r.run(1500);
  ASSERT_TRUE(r.left.takeDisplay(got));
  EXPECT_EQ(got.brightness, 20);
}

TEST(UnitLinkSessionTest, DisplayStateAtFirstContactFollowsTheLeftUnit) {
  Rig r;
  Config l = r.cfg(Role::Left);
  Config rt = r.cfg(Role::Right);
  l.display = nightOnly(true);
  r.left.start(l, r.now);
  r.right.start(rt, r.now);
  r.run(2000);
  DisplayState got;
  ASSERT_TRUE(r.right.takeDisplay(got));
  EXPECT_TRUE(got.night);
  EXPECT_FALSE(r.left.takeDisplay(got));  // the left unit keeps its own

  Rig q;
  Config l2 = q.cfg(Role::Left);
  Config r2 = q.cfg(Role::Right);
  r2.display = nightOnly(true);
  q.left.start(l2, q.now);
  q.right.start(r2, q.now);
  q.run(2000);
  EXPECT_FALSE(q.left.takeDisplay(got));  // the right unit's value does not override the left unit's
  ASSERT_TRUE(q.right.takeDisplay(got));  // it follows the left unit instead
  EXPECT_FALSE(got.night);
}

TEST(UnitLinkSessionTest, DisplayStateSurvivesLostFrames) {
  Rig r;
  r.l2r.lossPercent = 40;
  r.r2l.lossPercent = 40;
  r.startBoth();
  r.run(8000);
  DisplayState got;
  r.right.setDisplay(nightOnly(true), r.now);
  r.run(8000);
  ASSERT_TRUE(r.left.takeDisplay(got));
  EXPECT_TRUE(got.night);
}

TEST(UnitLinkSessionTest, RightUnitsJumpReachesTheLeftUnitOnceAndNewerReplacesOlder) {
  Rig r;
  r.startBoth();
  r.run(1500);
  JumpRequest j;
  j.spine = 4;
  j.page = 17;
  j.kind = JumpKind::Offset;
  j.value = 123456;
  ASSERT_TRUE(r.right.sendGoto(j, r.now));
  r.run(1500);
  JumpRequest got;
  ASSERT_TRUE(r.left.takeGoto(got));
  EXPECT_EQ(got.spine, 4);
  EXPECT_EQ(got.page, 17);
  EXPECT_TRUE(got.kind == JumpKind::Offset);
  EXPECT_EQ(got.value, 123456u);
  r.run(3000);
  EXPECT_FALSE(r.left.takeGoto(got));  // the resends stop and a repeat is not a new request
  j.spine = 5;
  j.kind = JumpKind::Fraction;
  j.value = 500000;
  ASSERT_TRUE(r.right.sendGoto(j, r.now));
  r.run(1500);
  ASSERT_TRUE(r.left.takeGoto(got));
  EXPECT_EQ(got.spine, 5);
  EXPECT_TRUE(got.kind == JumpKind::Fraction);
  EXPECT_FALSE(r.left.sendGoto(j, r.now));  // only the right unit asks
}

TEST(UnitLinkSessionTest, JumpSurvivesLostFrames) {
  Rig r;
  r.l2r.lossPercent = 40;
  r.r2l.lossPercent = 40;
  r.startBoth();
  r.run(8000);
  JumpRequest j;
  j.spine = 2;
  j.page = 9;
  int sent = 0;
  while (!r.right.sendGoto(j, r.now) && sent++ < 100) r.run(100);
  r.run(6000);
  JumpRequest got;
  ASSERT_TRUE(r.left.takeGoto(got));
  EXPECT_EQ(got.page, 9);
  EXPECT_FALSE(r.left.takeGoto(got));
}

TEST(UnitLinkSessionTest, LeavingTheBookTellsThePeerAndItIsNotOfferedBackAtOnce) {
  Rig r;
  r.startBoth();
  r.left.setBookPath("/a.epub");
  r.left.setIdentity(7, 9, r.now);
  r.run(1500);
  r.left.sendClose(7, r.now);
  r.left.setBookPath("");
  r.left.setIdentity(0, 9, r.now);  // what leaving the book does on the left unit
  r.run(500);
  EXPECT_TRUE(r.right.takeClose());
  EXPECT_FALSE(r.right.takeClose());
  r.right.setIdentity(0, 9, r.now);  // the right unit follows
  std::string path;
  r.run(2500);
  EXPECT_FALSE(r.right.takeBookRequest(path));
  EXPECT_FALSE(r.left.takeBookRequest(path));
  EXPECT_TRUE(r.left.status() == Status::Linked);
}

TEST(UnitLinkSessionTest, CloseOfAnotherBookIsIgnored) {
  Rig r;
  r.startBoth();
  r.run(1500);
  r.left.sendClose(1234, r.now);  // not the book the right unit has (7)
  r.run(500);
  EXPECT_FALSE(r.right.takeClose());
}

TEST(UnitLinkSessionTest, EndingTheLinkOnOneUnitIsHeardByTheOtherOnlyWhileLinked) {
  Rig r;
  r.startBoth();
  r.run(1500);
  r.left.sendEnd();
  r.run(200);
  EXPECT_TRUE(r.right.takeEndRequest());
  EXPECT_FALSE(r.right.takeEndRequest());

  Rig q;  // not linked yet: a stray End from outside does nothing
  q.right.start(q.cfg(Role::Right), q.now);
  q.left.sendEnd();  // never started, so it sends into the pipe only
  q.run(50);
  EXPECT_FALSE(q.right.takeEndRequest());
}
