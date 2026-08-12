#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "PageFlip/PageFlipPacket.h"
#include "PageFlip/PageFlipSession.h"
#include "PageFlip/PageFlipUdpTransport.h"

namespace {

// Away from both the 47190 default and the transport suite's port, so suites can run in parallel.
constexpr const char* TEST_BASE_PORT = "47590";
constexpr uint32_t BOOK = 0xB00C0DE5u;
constexpr uint32_t COMPAT = 0xC0FFEE01u;

bool startOnSlot(PageFlipUdpTransport& transport, const char* slot) {
  ::setenv("CROSSPOINT_PAGEFLIP_PORT", TEST_BASE_PORT, 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOTS", "2", 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOT", slot, 1);
  return transport.begin();
}

bool pollWithRetry(PageFlipSession& session, PageFlipDecision& decision) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (session.poll(decision)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// A pair of sessions talking over real loopback sockets: the protocol is worth testing against the
// transport it will actually run on, not a mock that cannot drop or reorder anything.
struct Pair {
  PageFlipUdpTransport leftTransport;
  PageFlipUdpTransport rightTransport;
  PageFlipSession left{leftTransport, PageFlipRole::Left};
  PageFlipSession right{rightTransport, PageFlipRole::Right};

  bool start() {
    if (!startOnSlot(leftTransport, "0") || !startOnSlot(rightTransport, "1")) return false;
    left.setBook(BOOK, COMPAT);
    right.setBook(BOOK, COMPAT);
    return true;
  }
};

class PageFlipSessionTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ::unsetenv("CROSSPOINT_PAGEFLIP_PORT");
    ::unsetenv("CROSSPOINT_PAGEFLIP_SLOTS");
    ::unsetenv("CROSSPOINT_PAGEFLIP_SLOT");
  }
};

TEST_F(PageFlipSessionTest, ExpectedNextTurnAdvancesTwo) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.left.announceLocalTurn(true, 3, 8, false));
  EXPECT_EQ(pair.left.getTurnSeq(), 1u);

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo);
  EXPECT_TRUE(decision.forward);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_EQ(pair.right.getTurnSeq(), 1u);
}

TEST_F(PageFlipSessionTest, BackwardTurnCarriesItsDirection) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.right.announceLocalTurn(false, 2, 4, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo);
  EXPECT_FALSE(decision.forward);
}

// A duplicate must not advance the pair a second time. This is the packet the counter exists for.
TEST_F(PageFlipSessionTest, DuplicateTurnIsIgnored) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  PageFlipTurn turn;
  turn.compatHash = COMPAT;
  turn.bookId = BOOK;
  turn.turnSeq = 1;
  turn.spineIndex = 4;
  turn.pageNumber = 2;
  turn.role = PageFlipRole::Left;
  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  ASSERT_TRUE(PageFlipPacket::encodeTurn(turn, wire, sizeof(wire), wireLength));

  ASSERT_TRUE(pair.leftTransport.broadcast(wire, wireLength));
  PageFlipDecision first;
  ASSERT_TRUE(pollWithRetry(pair.right, first));
  ASSERT_EQ(first.action, PageFlipAction::AdvanceTwo);

  ASSERT_TRUE(pair.leftTransport.broadcast(wire, wireLength));
  PageFlipDecision second;
  ASSERT_TRUE(pollWithRetry(pair.right, second));
  EXPECT_EQ(second.action, PageFlipAction::Ignore);
  EXPECT_EQ(pair.right.getTurnSeq(), 1u);
}

TEST_F(PageFlipSessionTest, StaleTurnIsIgnoredWithoutRewindingTheCounter) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  pair.right.adoptTurnSeq(5);

  PageFlipTurn stale;
  stale.compatHash = COMPAT;
  stale.bookId = BOOK;
  stale.turnSeq = 2;
  stale.role = PageFlipRole::Left;
  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  ASSERT_TRUE(PageFlipPacket::encodeTurn(stale, wire, sizeof(wire), wireLength));
  ASSERT_TRUE(pair.leftTransport.broadcast(wire, wireLength));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
  EXPECT_EQ(pair.right.getTurnSeq(), 5u);
}

// Turns taken while this device was unreachable must not be replayed one by one -- the pair seeks
// to the sender's absolute position instead, which is what stops a dropped packet becoming
// permanent drift.
TEST_F(PageFlipSessionTest, MissedTurnsHealFromTheAbsolutePosition) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  // The right device is away: its socket is closed, so these turns land nowhere.
  pair.right.end();
  ASSERT_TRUE(pair.left.announceLocalTurn(true, 1, 1, false));
  ASSERT_TRUE(pair.left.announceLocalTurn(true, 1, 3, false));

  ASSERT_TRUE(startOnSlot(pair.rightTransport, "1"));
  ASSERT_TRUE(pair.left.announceLocalTurn(true, 2, 0, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Heal);
  EXPECT_EQ(decision.spineIndex, 2);
  EXPECT_EQ(decision.pageNumber, 0);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_TRUE(decision.forward);
  EXPECT_EQ(pair.right.getTurnSeq(), 3u);
}

// Both devices pressing the same way in one window: each computes the same turnSeq, so each sees
// the other as already applied. The pair advances once, not twice.
TEST_F(PageFlipSessionTest, SimultaneousPressesInTheSameDirectionAdvanceOnce) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.left.announceLocalTurn(true, 5, 1, false));
  ASSERT_TRUE(pair.right.announceLocalTurn(true, 5, 2, false));

  PageFlipDecision atLeft;
  PageFlipDecision atRight;
  ASSERT_TRUE(pollWithRetry(pair.left, atLeft));
  ASSERT_TRUE(pollWithRetry(pair.right, atRight));
  EXPECT_EQ(atLeft.action, PageFlipAction::Ignore);
  EXPECT_EQ(atRight.action, PageFlipAction::Ignore);
  EXPECT_EQ(pair.left.getTurnSeq(), 1u);
  EXPECT_EQ(pair.right.getTurnSeq(), 1u);
}

// Opposite directions in the same window is the residual race. Undefined behaviour here is a
// desync, so the lower MAC wins and the loser heals from the winner's position.
TEST_F(PageFlipSessionTest, SimultaneousOppositePressesResolveByLowerMac) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.left.announceLocalTurn(true, 6, 4, false));    // slot 0: lower MAC, wins
  ASSERT_TRUE(pair.right.announceLocalTurn(false, 6, 1, false));  // slot 1: loses, must heal

  PageFlipDecision atLeft;
  ASSERT_TRUE(pollWithRetry(pair.left, atLeft));
  EXPECT_EQ(atLeft.action, PageFlipAction::Ignore) << "the lower MAC must hold its position";

  PageFlipDecision atRight;
  ASSERT_TRUE(pollWithRetry(pair.right, atRight));
  EXPECT_EQ(atRight.action, PageFlipAction::Heal);
  EXPECT_EQ(atRight.spineIndex, 6);
  EXPECT_EQ(atRight.pageNumber, 4);
  EXPECT_TRUE(atRight.applyRoleOffset);
}

TEST_F(PageFlipSessionTest, PacketForAnotherBookIsPeerContactButNotAnAdvance) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(0x0000AAAAu, COMPAT);

  ASSERT_TRUE(pair.left.announceLocalTurn(true, 1, 1, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision)) << "a decoded packet is still peer contact";
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
  EXPECT_EQ(pair.right.getTurnSeq(), 0u);
}

// Same book, different layout: applying the peer's page number would land on unrelated text.
TEST_F(PageFlipSessionTest, IncompatibleLayoutReportsMismatchAndNeverApplies) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, COMPAT ^ 0xFFFFFFFFu);  // same book, a layout this device cannot match

  ASSERT_TRUE(pair.left.announceLocalTurn(true, 9, 9, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Mismatch);
  EXPECT_EQ(pair.right.getTurnSeq(), 0u);
}

// A cold boot resets the counter. Without adoption the rebooted device's presses are all read as
// "already applied" by an awake peer, and do nothing at all, permanently.
TEST_F(PageFlipSessionTest, AdoptedTurnSeqKeepsARebootedDeviceUsable) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  pair.left.adoptTurnSeq(847);
  EXPECT_EQ(pair.left.getTurnSeq(), 847u);
  // Adoption is max(local, peer): a lower peer count never rewinds this device.
  pair.left.adoptTurnSeq(12);
  EXPECT_EQ(pair.left.getTurnSeq(), 847u);

  pair.right.adoptTurnSeq(847);
  ASSERT_TRUE(pair.right.announceLocalTurn(true, 0, 1, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo) << "a rebooted peer's press must still register";
}

TEST_F(PageFlipSessionTest, SoloTurnsStillAdvanceTheCounter) {
  PageFlipUdpTransport transport;
  ASSERT_TRUE(startOnSlot(transport, "0"));
  PageFlipSession solo(transport, PageFlipRole::Left);
  solo.setBook(BOOK, COMPAT);

  // Nothing is listening, so the broadcast reaches no one -- which is not an error, and the counter
  // must still move so a peer joining later adopts a value that is ahead of it.
  solo.announceLocalTurn(true, 1, 1, false);
  solo.announceLocalTurn(true, 1, 2, false);
  EXPECT_EQ(solo.getTurnSeq(), 2u);
}

}  // namespace
