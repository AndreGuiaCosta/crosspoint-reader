#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
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

bool startOnSlot(PageFlipUdpTransport& transport, const char* slot, const char* slots = "2") {
  ::setenv("CROSSPOINT_PAGEFLIP_PORT", TEST_BASE_PORT, 1);
  // Every instance in one test has to agree on the count: broadcast() sends to every slot but its
  // own, so a two-slot device in a three-device test would simply never speak to the third.
  ::setenv("CROSSPOINT_PAGEFLIP_SLOTS", slots, 1);
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

// The heal offset is fixed by role, not by the direction the sender travelled: right is always
// left + 1 however the pair got there. Reading the sender's direction instead lands a backward heal
// two pages out -- and it presents as the right device showing the page BEFORE the left one, which
// looks like a boundary bug in advanceOnePage rather than a protocol one.
TEST_F(PageFlipSessionTest, BackwardHealStillOffsetsForwardOnTheRightDevice) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  pair.right.end();
  ASSERT_TRUE(pair.left.announceLocalTurn(false, 4, 6, false));
  ASSERT_TRUE(pair.left.announceLocalTurn(false, 4, 4, false));

  ASSERT_TRUE(startOnSlot(pair.rightTransport, "1"));
  ASSERT_TRUE(pair.left.announceLocalTurn(false, 4, 2, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  ASSERT_EQ(decision.action, PageFlipAction::Heal);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_TRUE(decision.forward) << "the right device is one page AHEAD of the left, always";
}

// ...and the mirror: the left device heals backward off the right device's position, even when the
// turn that got them there was a forward one.
TEST_F(PageFlipSessionTest, ForwardHealOffsetsBackwardOnTheLeftDevice) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  pair.left.end();
  ASSERT_TRUE(pair.right.announceLocalTurn(true, 1, 1, false));
  ASSERT_TRUE(pair.right.announceLocalTurn(true, 1, 3, false));

  ASSERT_TRUE(startOnSlot(pair.leftTransport, "0"));
  ASSERT_TRUE(pair.right.announceLocalTurn(true, 1, 5, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  ASSERT_EQ(decision.action, PageFlipAction::Heal);
  EXPECT_FALSE(decision.forward) << "the left device is one page BEHIND the right, always";
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

// Presence is what licenses advancing by two. A link that came up is not a peer that is there, and
// a lone device advancing by two would turn two pages on every press.
TEST_F(PageFlipSessionTest, GreetingAnnouncesPresenceAndAdoptsTheCounter) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  pair.left.adoptTurnSeq(847);
  ASSERT_TRUE(pair.left.announceHello(3, 2, 500));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_TRUE(decision.peerWantsReply);
  EXPECT_EQ(decision.spineIndex, 3);
  EXPECT_EQ(decision.peerVisibleTextOffset, 500u);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_EQ(pair.right.getTurnSeq(), 847u) << "the greeting carries the pair's counter";
}

// The reply to a greeting must not itself be answered, or two devices greet each other forever.
// The decline is the case with no join behind it to stop the exchange on its own.
TEST_F(PageFlipSessionTest, DeclineAsksForNothingBack) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.right.announcePresence(1, 1, 90));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_FALSE(decision.peerWantsReply);
  EXPECT_EQ(decision.peerVisibleTextOffset, 90u);
}

TEST_F(PageFlipSessionTest, GreetingForAnotherBookIsNotOurPeer) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(0x1234u, COMPAT);

  ASSERT_TRUE(pair.left.announceHello(0, 0, 0));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
}

// The greeting is the earliest a layout difference can be caught, and the cheapest: catching it
// only on the first turn means the pair registers as present, advances by two, and desyncs before
// anyone is told.
TEST_F(PageFlipSessionTest, GreetingWithADifferentLayoutIsAMismatch) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, COMPAT ^ 0xFFFFFFFFu);  // same book, a layout this device cannot match

  ASSERT_TRUE(pair.left.announceHello(3, 2, 500));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Mismatch);
  // The reply flag has to survive the mismatch: the answer carries this device's own hash, which is
  // how the peer's user gets told too. One device reporting and the other silently doing nothing is
  // worse than either device alone.
  EXPECT_TRUE(decision.peerWantsReply);
}

// Adoption is deliberately not gated on compatibility. turnSeq is a session fact, not a layout one,
// and skipping it here would leave the pair deadlocked at the moment force-sync made them
// compatible -- the low-counter device's presses would all read as "already applied".
TEST_F(PageFlipSessionTest, MismatchedGreetingStillAdoptsTheCounter) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, COMPAT ^ 0xFFFFFFFFu);
  pair.left.adoptTurnSeq(847);

  ASSERT_TRUE(pair.left.announceHello(0, 0, 0));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  ASSERT_EQ(decision.action, PageFlipAction::Mismatch);
  EXPECT_EQ(pair.right.getTurnSeq(), 847u);
}

// The cold-start hole. A device advertises compatHash 0 -- "not computed yet" -- from the moment
// its link comes up until its first render fixes the viewport, which on a cold cache is seconds.
// A bare equality check reads that as a mismatch, so two identically configured devices would
// report incompatibility every time one rendered faster than the other: warm cache on one, cold on
// the other, i.e. the second time you open a book. Neither simulator harness can catch this,
// because both copy identical SD roots and the two halves hash within a millisecond of each other.
TEST_F(PageFlipSessionTest, AGreetingFromADeviceThatHasNotRenderedIsNotAMismatch) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, 0);  // link up, first render still in flight

  ASSERT_TRUE(pair.left.announceHello(0, 0, 0));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_FALSE(decision.layoutDecided) << "nothing has been established, so nothing may be paired on";
  EXPECT_TRUE(decision.peerWantsReply) << "the handshake still completes; the pair converges next round";
}

// The same hole in the other direction: this device is the one that has not rendered.
TEST_F(PageFlipSessionTest, AGreetingReceivedBeforeOurOwnFirstRenderIsNotAMismatch) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.right.setBook(BOOK, 0);

  ASSERT_TRUE(pair.left.announceHello(0, 0, 0));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_FALSE(decision.layoutDecided);
}

// --- the join negotiation (docs/pageflip.md section 4.2) ---

// One device driven the way the reader drives it: poll, and answer every probe with the verdict
// this device's own pagination implies. The verdict itself is an input here -- deriving it from a
// loaded Section is the reader's job, and this suite is about what the pair concludes from the two
// answers, not how either one was reached.
struct JoinDevice {
  PageFlipSession& session;
  int32_t spineIndex = 0;
  uint32_t offset = 0;
  // Where this device's next page starts, or nothing when it cannot tell -- a section still
  // building has no answer, and the honest reply is Unknown.
  bool nextKnown = true;
  uint32_t nextOffset = 0;

  PageFlipJoinResolution resolution;
  int resolutionCount = 0;
  int packetsSeen = 0;

  bool pump() {
    PageFlipDecision decision;
    if (!session.poll(decision)) return false;
    ++packetsSeen;
    if (!decision.joinProbe) return true;

    PageFlipJoinVerdict verdict = PageFlipJoinVerdict::Unknown;
    if (nextKnown) {
      verdict = (decision.spineIndex == spineIndex && decision.peerVisibleTextOffset == nextOffset)
                    ? PageFlipJoinVerdict::Adjacent
                    : PageFlipJoinVerdict::NotAdjacent;
    }
    PageFlipJoinResolution answer;
    if (session.answerJoin(decision.joinRound, verdict, spineIndex, 0, offset, answer) && answer.resolved) {
      resolution = answer;
      ++resolutionCount;
    }
    return true;
  }
};

// Runs the exchange to quiescence. The bound is part of what is being tested: an exchange that does
// not terminate is two devices trading greetings at packet rate, which on real hardware is a radio
// that never sleeps.
void settleJoin(JoinDevice& a, JoinDevice& b, const int maxRounds = 20) {
  for (int round = 0; round < maxRounds; ++round) {
    bool progressed = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (a.pump()) progressed = true;
      if (b.pump()) progressed = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!progressed) return;
  }
  ADD_FAILURE() << "the join never went quiet";
}

// The bootstrap case, and the one section 4.2's table has no row for: two devices on the same page.
// It is what a book opened for the first time on both looks like, and it is the only join that has
// to CREATE the one-page spread rather than recognise one.
TEST_F(PageFlipSessionTest, IdenticalPositionsAreTheCaseThatMakesTheSpread) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  JoinDevice left{pair.left, 1, 100, true, 250};
  JoinDevice right{pair.right, 1, 100, true, 250};

  ASSERT_TRUE(pair.left.announceHello(1, 0, 100));
  settleJoin(left, right);

  ASSERT_EQ(left.resolutionCount, 1);
  ASSERT_EQ(right.resolutionCount, 1);
  EXPECT_EQ(left.resolution.joinCase, PageFlipJoinCase::Identical);
  EXPECT_EQ(right.resolution.joinCase, PageFlipJoinCase::Identical);
  // Both devices answer NotAdjacent from the same page -- their own next page is somewhere neither
  // of them is. Read off the verdicts alone this is indistinguishable from divergent, which is why
  // the case is a direct equality test instead.
  EXPECT_EQ(right.resolution.peerVisibleTextOffset, 100u);
}

// The ordinary reopen: left at P, right at P+1.
TEST_F(PageFlipSessionTest, AnAlreadySpreadPairIsAligned) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  JoinDevice left{pair.left, 1, 100, true, 250};
  JoinDevice right{pair.right, 1, 250, true, 400};

  ASSERT_TRUE(pair.left.announceHello(1, 0, 100));
  settleJoin(left, right);

  ASSERT_EQ(left.resolutionCount, 1);
  ASSERT_EQ(right.resolutionCount, 1);
  EXPECT_EQ(left.resolution.joinCase, PageFlipJoinCase::Aligned);
  EXPECT_EQ(right.resolution.joinCase, PageFlipJoinCase::Aligned)
      << "the device that only heard the verdict must reach the same conclusion as the one that computed it";
}

// The same two positions with the devices the other way round. Only the roles differ, and that is
// the whole difference between "resume" and "the user swapped them".
TEST_F(PageFlipSessionTest, TheSamePositionsWithSwappedRolesAreSwapped) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  JoinDevice left{pair.left, 1, 250, true, 400};
  JoinDevice right{pair.right, 1, 100, true, 250};

  ASSERT_TRUE(pair.left.announceHello(1, 0, 250));
  settleJoin(left, right);

  ASSERT_EQ(left.resolutionCount, 1);
  ASSERT_EQ(right.resolutionCount, 1);
  EXPECT_EQ(left.resolution.joinCase, PageFlipJoinCase::Swapped);
  EXPECT_EQ(right.resolution.joinCase, PageFlipJoinCase::Swapped);
}

TEST_F(PageFlipSessionTest, PositionsNeitherDeviceRecognisesAreDivergent) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  JoinDevice left{pair.left, 1, 100, true, 250};
  JoinDevice right{pair.right, 7, 9000, true, 9100};

  ASSERT_TRUE(pair.left.announceHello(1, 0, 100));
  settleJoin(left, right);

  ASSERT_EQ(left.resolutionCount, 1);
  ASSERT_EQ(right.resolutionCount, 1);
  EXPECT_EQ(left.resolution.joinCase, PageFlipJoinCase::Divergent);
  EXPECT_EQ(right.resolution.joinCase, PageFlipJoinCase::Divergent);
  // The prompt of section 4.3 describes the peer's position, so it has to survive the trip.
  EXPECT_EQ(left.resolution.peerSpineIndex, 7);
  EXPECT_EQ(left.resolution.peerVisibleTextOffset, 9000u);
}

// Unknown is "ask again", never "no". A section still building cannot say whether it is on its last
// page -- pageCount is a watermark until it finalizes -- and treating that as a no would put the
// resume prompt of section 4.3 in front of a user whose pair is perfectly fine.
TEST_F(PageFlipSessionTest, ADeviceThatCannotTestYetIsNotCalledDivergent) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  JoinDevice left{pair.left, 1, 100, true, 250};
  JoinDevice right{pair.right, 1, 700, false, 0};  // section still building: no answer to give

  ASSERT_TRUE(pair.left.announceHello(1, 0, 100));
  settleJoin(left, right);

  EXPECT_EQ(left.resolutionCount, 0);
  EXPECT_EQ(right.resolutionCount, 0);
  // And it must go quiet rather than retry on the wire. The retry that resolves this is the
  // building device's own re-greeting once its section settles, not a packet-rate ping-pong that
  // would keep both radios awake for as long as the build runs.
  EXPECT_LE(left.packetsSeen + right.packetsSeen, 6);
}

// The exchange terminates on its own even when both devices greet at once, which is the normal case
// for two readers opened within a second of each other.
TEST_F(PageFlipSessionTest, SimultaneousGreetingsStillConverge) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  JoinDevice left{pair.left, 1, 100, true, 250};
  JoinDevice right{pair.right, 1, 250, true, 400};

  ASSERT_TRUE(pair.left.announceHello(1, 0, 100));
  ASSERT_TRUE(pair.right.announceHello(1, 0, 250));
  settleJoin(left, right);

  EXPECT_EQ(left.resolution.joinCase, PageFlipJoinCase::Aligned);
  EXPECT_EQ(right.resolution.joinCase, PageFlipJoinCase::Aligned);
  EXPECT_EQ(left.resolutionCount, 1) << "classified once, however many packets the exchange took";
  EXPECT_EQ(right.resolutionCount, 1);
}

// An answer computed against a position the peer has since left would classify the pair from a page
// neither device is on. The round is the same staleness guard the force-sync answer takes on
// targetHash.
TEST_F(PageFlipSessionTest, AnAnswerForAnOldRoundIsRejected) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.right.announceHello(1, 0, 250));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  ASSERT_TRUE(decision.joinProbe);
  const uint32_t staleRound = decision.joinRound;

  // The peer moved and greeted again; the answer below is about where it used to be.
  ASSERT_TRUE(pair.right.announceHello(1, 0, 900));
  ASSERT_TRUE(pollWithRetry(pair.left, decision));

  PageFlipJoinResolution resolution;
  EXPECT_FALSE(pair.left.answerJoin(staleRound, PageFlipJoinVerdict::Adjacent, 1, 0, 100, resolution));
  EXPECT_FALSE(resolution.resolved);
}

// Section 4.4: rotation changes the viewport, so it changes the hash, so everything concluded about
// the old pagination is void. Re-negotiating is the recovery -- there is no separate mode for it.
TEST_F(PageFlipSessionTest, ChangingOurLayoutRestartsTheNegotiation) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.right.announceHello(1, 0, 250));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  ASSERT_TRUE(decision.joinProbe);
  const uint32_t roundBefore = decision.joinRound;

  pair.left.setBook(BOOK, COMPAT ^ 0x5A5A5A5Au);

  PageFlipJoinResolution resolution;
  EXPECT_FALSE(pair.left.answerJoin(roundBefore, PageFlipJoinVerdict::Adjacent, 1, 0, 100, resolution))
      << "a verdict about the old pagination must not classify the new one";
}

// The ordering of section 4.2 is mandatory: comparing positions taken from two different paginations
// is meaningless, so the probe may not be raised until the layouts are known to agree.
TEST_F(PageFlipSessionTest, AnIncompatibleOrUndecidedPeerIsNeverProbed) {
  {
    Pair pair;
    ASSERT_TRUE(pair.start());
    pair.left.setBook(BOOK, COMPAT ^ 0xFFFFFFFFu);
    ASSERT_TRUE(pair.left.announceHello(1, 0, 100));

    PageFlipDecision decision;
    ASSERT_TRUE(pollWithRetry(pair.right, decision));
    ASSERT_EQ(decision.action, PageFlipAction::Mismatch);
    EXPECT_FALSE(decision.joinProbe);
  }
  {
    Pair pair;
    ASSERT_TRUE(pair.start());
    pair.left.setBook(BOOK, 0);  // link up, first render still in flight
    ASSERT_TRUE(pair.left.announceHello(1, 0, 100));

    PageFlipDecision decision;
    ASSERT_TRUE(pollWithRetry(pair.right, decision));
    ASSERT_EQ(decision.action, PageFlipAction::PeerHello);
    EXPECT_FALSE(decision.joinProbe) << "the sentinel is not a layout, so there is nothing to have agreed on";
  }
}

// --- the divergent join's answer (docs/pageflip.md section 4.3) ---

// Confirming picks a device, not an option: the chooser stays where it is and the other seeks to
// it. The role offset is fixed by role, so the left device lands one page BEFORE a right device's
// choice however the pair got there.
TEST_F(PageFlipSessionTest, ConfirmingAResumeMovesTheOtherDevice) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.right.proposeResume(4, 1200));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::JoinResume);
  EXPECT_EQ(decision.spineIndex, 4);
  EXPECT_EQ(decision.peerVisibleTextOffset, 1200u);
  EXPECT_EQ(decision.peerRole, PageFlipRole::Right);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_FALSE(decision.forward) << "left sits one page before the right device's choice";
}

TEST_F(PageFlipSessionTest, TheRightDeviceLandsAfterALeftChoice) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.left.proposeResume(4, 1200));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::JoinResume);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_TRUE(decision.forward);
}

// Both users confirming in the same window. Without the tiebreak each device seeks to the other and
// the pair trades positions instead of settling on one.
TEST_F(PageFlipSessionTest, SimultaneousResumeChoicesResolveByLowerMac) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.left.proposeResume(1, 100));
  ASSERT_TRUE(pair.right.proposeResume(9, 900));

  // Slot 0's MAC is the lower one, so the left device's choice stands and it ignores the right's.
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);

  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::JoinResume);
  EXPECT_EQ(decision.spineIndex, 1) << "the loser adopts the winner's choice";
}

// A choice made under a layout the pair has since left names a page in a pagination this device is
// not using, and the role offset below is a step through one.
TEST_F(PageFlipSessionTest, AResumeFromAnotherLayoutIsIgnored) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, COMPAT ^ 0xFFFFFFFFu);

  ASSERT_TRUE(pair.left.proposeResume(4, 1200));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
}

TEST_F(PageFlipSessionTest, AResumeBeforeTheFirstRenderIsRefused) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, 0);

  EXPECT_FALSE(pair.left.proposeResume(4, 1200)) << "the sentinel is not a pagination to name a page in";
}

// A turn arriving in that same window must not be applied -- stepping a position whose layout is
// unknown is the desync the hash exists to prevent -- but it must not be reported either.
TEST_F(PageFlipSessionTest, ATurnFromADeviceThatHasNotRenderedIsIgnoredNotReported) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, 0);

  ASSERT_TRUE(pair.left.announceLocalTurn(true, 4, 2, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
  EXPECT_FALSE(decision.layoutDecided);
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

// --- settings force-sync (docs/pageflip.md section 5.1) ---

// The two devices genuinely disagree, which is the only situation a force-sync exists for. The
// source keeps COMPAT; the device being pushed to sits on something else until it applies.
constexpr uint32_t OTHER_COMPAT = 0x5EED1234u;

PageFlipRenderSettings sampleSettings() {
  PageFlipRenderSettings settings;
  settings.fontId = 99;
  settings.viewportWidth = 760;
  settings.viewportHeight = 430;
  settings.fontFamily = 1;
  settings.fontPointSize = 16;
  settings.lineSpacing = 2;
  settings.screenMargin = 20;
  std::snprintf(settings.sdFontFamilyName, sizeof(settings.sdFontFamilyName), "Bookerly");
  return settings;
}

// Drives the whole exchange in one place, because the interesting cases are all variations on it.
struct DivergentPair : Pair {
  bool startDivergent() {
    if (!start()) return false;
    right.setBook(BOOK, OTHER_COMPAT);
    return true;
  }
};

TEST_F(PageFlipSessionTest, OfferIsPreflightedBeforeAnythingIsCommitted) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());

  const PageFlipRenderSettings offered = sampleSettings();
  ASSERT_TRUE(pair.left.offerSettings(offered));
  EXPECT_TRUE(pair.left.hasOutstandingOffer());

  PageFlipDecision atPeer;
  ASSERT_TRUE(pollWithRetry(pair.right, atPeer));
  ASSERT_EQ(atPeer.action, PageFlipAction::SettingsOffer);
  ASSERT_NE(atPeer.settings, nullptr);
  EXPECT_STREQ(atPeer.settings->sdFontFamilyName, "Bookerly");
  EXPECT_EQ(atPeer.settings->screenMargin, offered.screenMargin);
  EXPECT_EQ(atPeer.peerRole, PageFlipRole::Left);

  // The preflight says "applying this lands me on COMPAT", which is the source's own layout.
  ASSERT_TRUE(pair.right.answerOffer(PageFlipSyncResult::Ok, COMPAT));

  PageFlipDecision atSource;
  ASSERT_TRUE(pollWithRetry(pair.left, atSource));
  ASSERT_EQ(atSource.action, PageFlipAction::SettingsAnswer);
  EXPECT_EQ(atSource.syncResult, PageFlipSyncResult::Ok);
  EXPECT_EQ(atSource.peerRole, PageFlipRole::Right);

  ASSERT_TRUE(pair.left.commitOffer());
  EXPECT_FALSE(pair.left.hasOutstandingOffer());

  PageFlipDecision applied;
  ASSERT_TRUE(pollWithRetry(pair.right, applied));
  ASSERT_EQ(applied.action, PageFlipAction::SettingsApply);
  ASSERT_NE(applied.settings, nullptr);
  // What is written is what was preflighted, not what a later packet claimed.
  EXPECT_STREQ(applied.settings->sdFontFamilyName, "Bookerly");
  EXPECT_EQ(applied.settings->fontPointSize, offered.fontPointSize);
}

// The abort case, and the reason the exchange has three steps: a device that cannot resolve the
// font says so, and nothing is written anywhere.
TEST_F(PageFlipSessionTest, RefusedOfferLeavesNothingToCommit) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());

  ASSERT_TRUE(pair.left.offerSettings(sampleSettings()));
  PageFlipDecision atPeer;
  ASSERT_TRUE(pollWithRetry(pair.right, atPeer));
  ASSERT_EQ(atPeer.action, PageFlipAction::SettingsOffer);

  ASSERT_TRUE(pair.right.answerOffer(PageFlipSyncResult::MissingFont, 0));

  PageFlipDecision atSource;
  ASSERT_TRUE(pollWithRetry(pair.left, atSource));
  ASSERT_EQ(atSource.action, PageFlipAction::SettingsAnswer);
  EXPECT_EQ(atSource.syncResult, PageFlipSyncResult::MissingFont);
  // The refusal ended it: there is nothing left to commit, on either side.
  EXPECT_FALSE(pair.left.hasOutstandingOffer());
  EXPECT_FALSE(pair.left.commitOffer());
}

// findFamily() succeeding is not the same as the two devices ending up laid out alike -- a
// different file for the same family name resolves a different fontId. The hash is what decides, so
// an Ok that does not converge must not be treated as one.
TEST_F(PageFlipSessionTest, AnswerThatDoesNotConvergeIsNotOk) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());

  ASSERT_TRUE(pair.left.offerSettings(sampleSettings()));
  PageFlipDecision atPeer;
  ASSERT_TRUE(pollWithRetry(pair.right, atPeer));
  ASSERT_EQ(atPeer.action, PageFlipAction::SettingsOffer);

  ASSERT_TRUE(pair.right.answerOffer(PageFlipSyncResult::Ok, OTHER_COMPAT));

  PageFlipDecision atSource;
  ASSERT_TRUE(pollWithRetry(pair.left, atSource));
  ASSERT_EQ(atSource.action, PageFlipAction::SettingsAnswer);
  EXPECT_EQ(atSource.syncResult, PageFlipSyncResult::Unknown);
  EXPECT_FALSE(pair.left.hasOutstandingOffer());
}

// A commit is only ever honoured for the offer this device preflighted itself. Anything else would
// write settings nobody checked.
TEST_F(PageFlipSessionTest, ApplyForAnOfferNeverPreflightedIsIgnored) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());

  PageFlipSyncApply forged;
  forged.targetHash = COMPAT;
  forged.bookId = BOOK;
  forged.role = PageFlipRole::Left;
  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  ASSERT_TRUE(PageFlipPacket::encodeSyncApply(forged, wire, sizeof(wire), wireLength));
  ASSERT_TRUE(pair.leftTransport.broadcast(wire, wireLength));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
  EXPECT_EQ(decision.settings, nullptr);
}

// An offer names the layout the peer must reach, so a device that has not rendered has nothing to
// offer -- and the sentinel it would advertise is not a layout.
TEST_F(PageFlipSessionTest, OfferBeforeTheFirstRenderIsRefused) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(BOOK, 0);

  EXPECT_FALSE(pair.left.offerSettings(sampleSettings()));
  EXPECT_FALSE(pair.left.hasOutstandingOffer());
}

// Both users confirming at once. Exactly one push must survive, or the two devices overwrite each
// other and neither ends up where the user asked.
TEST_F(PageFlipSessionTest, SimultaneousOffersResolveByLowerMac) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());

  ASSERT_TRUE(pair.left.offerSettings(sampleSettings()));
  ASSERT_TRUE(pair.right.offerSettings(sampleSettings()));

  // Slot 0's MAC is the lower one, so the left device keeps its offer and ignores the other.
  PageFlipDecision atLeft;
  ASSERT_TRUE(pollWithRetry(pair.left, atLeft));
  EXPECT_EQ(atLeft.action, PageFlipAction::Ignore);
  EXPECT_TRUE(pair.left.hasOutstandingOffer());

  // The loser drops its own offer and preflights the winner's.
  PageFlipDecision atRight;
  ASSERT_TRUE(pollWithRetry(pair.right, atRight));
  EXPECT_EQ(atRight.action, PageFlipAction::SettingsOffer);
  EXPECT_FALSE(pair.right.hasOutstandingOffer());
}

// Rotating or changing a setting mid-exchange moves this device's own layout, so the offer and any
// answer to it are about something that no longer exists.
TEST_F(PageFlipSessionTest, ChangingOurLayoutDropsAnOfferInFlight) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());

  ASSERT_TRUE(pair.left.offerSettings(sampleSettings()));
  PageFlipDecision atPeer;
  ASSERT_TRUE(pollWithRetry(pair.right, atPeer));
  ASSERT_EQ(atPeer.action, PageFlipAction::SettingsOffer);

  pair.left.setBook(BOOK, 0x0BADF00Du);
  EXPECT_FALSE(pair.left.hasOutstandingOffer());

  // The answer to the abandoned offer is stale, not a verdict to act on.
  ASSERT_TRUE(pair.right.answerOffer(PageFlipSyncResult::Ok, COMPAT));
  PageFlipDecision atSource;
  ASSERT_TRUE(pollWithRetry(pair.left, atSource));
  EXPECT_EQ(atSource.action, PageFlipAction::Ignore);
}

TEST_F(PageFlipSessionTest, OfferForAnotherBookIsNotOurExchange) {
  DivergentPair pair;
  ASSERT_TRUE(pair.startDivergent());
  pair.left.setBook(0xFEEDFACEu, COMPAT);

  ASSERT_TRUE(pair.left.offerSettings(sampleSettings()));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::Ignore);
  EXPECT_EQ(decision.settings, nullptr);
}

// Three devices in one room, all on the same book with the same layout (docs/pageflip.md section
// 8.1). Without a paired MAC the bookId and compatHash filters admit every one of them, and the
// third device's presses move the pair's pages.
struct Room {
  PageFlipUdpTransport leftTransport;
  PageFlipUdpTransport rightTransport;
  PageFlipUdpTransport strangerTransport;
  PageFlipSession left{leftTransport, PageFlipRole::Left};
  PageFlipSession right{rightTransport, PageFlipRole::Right};
  // Configured as a left half deliberately: a stranger is not a badly configured peer, it is
  // somebody else's device, and it will happily be the left half of its own pair.
  PageFlipSession stranger{strangerTransport, PageFlipRole::Left};

  bool start() {
    if (!startOnSlot(leftTransport, "0", "3") || !startOnSlot(rightTransport, "1", "3") ||
        !startOnSlot(strangerTransport, "2", "3")) {
      return false;
    }
    left.setBook(BOOK, COMPAT);
    right.setBook(BOOK, COMPAT);
    stranger.setBook(BOOK, COMPAT);
    return true;
  }

  bool pairLeftToRight() {
    uint8_t mac[PageFlipTransport::MAC_BYTES] = {};
    if (!rightTransport.localMac(mac)) return false;
    left.setPeerMac(mac);
    return true;
  }
};

// Nothing arrives, rather than an Ignore decision: an Ignore is still peer contact, and contact
// holds the reader awake and feeds the presence timer. A stranger must cost this device nothing.
TEST_F(PageFlipSessionTest, AStrangersTurnDoesNotEvenArrive) {
  Room room;
  ASSERT_TRUE(room.start());
  ASSERT_TRUE(room.pairLeftToRight());

  ASSERT_TRUE(room.stranger.announceLocalTurn(true, 5, 9, false));

  PageFlipDecision decision;
  EXPECT_FALSE(pollWithRetry(room.left, decision));
  EXPECT_EQ(room.left.getTurnSeq(), 0u);
}

// The negative control for the test above: unpaired, that same packet is applied.
TEST_F(PageFlipSessionTest, WithoutAPairedDeviceAStrangerIsAPeer) {
  Room room;
  ASSERT_TRUE(room.start());

  ASSERT_TRUE(room.stranger.announceLocalTurn(true, 5, 9, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo);
}

TEST_F(PageFlipSessionTest, ThePairedDeviceIsStillHeard) {
  Room room;
  ASSERT_TRUE(room.start());
  ASSERT_TRUE(room.pairLeftToRight());

  ASSERT_TRUE(room.right.announceLocalTurn(true, 3, 8, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo);
  EXPECT_TRUE(decision.applyRoleOffset);
}

TEST_F(PageFlipSessionTest, ClearingThePairListensToAnyoneAgain) {
  Room room;
  ASSERT_TRUE(room.start());
  ASSERT_TRUE(room.pairLeftToRight());
  EXPECT_TRUE(room.left.hasPeerMac());

  room.left.setPeerMac(nullptr);
  EXPECT_FALSE(room.left.hasPeerMac());

  ASSERT_TRUE(room.stranger.announceLocalTurn(true, 5, 9, false));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo);
}

// An all-zero MAC is the absence of a value, not a device to listen for. A session that took it
// literally would filter out every packet in the world and look like a peer that never showed up.
TEST_F(PageFlipSessionTest, AnAllZeroMacIsNotADevice) {
  Room room;
  ASSERT_TRUE(room.start());
  const uint8_t zeros[PageFlipTransport::MAC_BYTES] = {};
  room.left.setPeerMac(zeros);
  EXPECT_FALSE(room.left.hasPeerMac());

  ASSERT_TRUE(room.right.announceLocalTurn(true, 3, 8, false));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::AdvanceTwo);
}

// A pairing screen in the room says nothing to a reader -- not even that somebody is there. Sent
// from the PAIRED device, so it is the message type being rejected here and not the sender.
TEST_F(PageFlipSessionTest, APairBeaconIsNotPeerContact) {
  Room room;
  ASSERT_TRUE(room.start());
  ASSERT_TRUE(room.pairLeftToRight());

  uint8_t beacon[PageFlipPacket::PAIR_BEACON_BYTES] = {};
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodePairBeacon(PageFlipRole::Right, beacon, sizeof(beacon), length));
  ASSERT_TRUE(room.rightTransport.broadcast(beacon, length));

  PageFlipDecision decision;
  EXPECT_FALSE(pollWithRetry(room.left, decision));
}

// Pairing with somebody else mid-session invalidates the classification, exactly as a role change
// does: a join is a statement about two positions, and one of them now belongs to a device this
// session will not listen to.
TEST_F(PageFlipSessionTest, PairingWithAnotherDeviceRestartsTheJoin) {
  Room room;
  ASSERT_TRUE(room.start());

  ASSERT_TRUE(room.stranger.announceHello(2, 0, 400));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  ASSERT_EQ(decision.action, PageFlipAction::PeerHello);
  ASSERT_TRUE(decision.joinProbe);

  ASSERT_TRUE(room.pairLeftToRight());

  // The probe was raised against the stranger's offset, and the round it belonged to is gone.
  PageFlipJoinResolution resolution;
  EXPECT_FALSE(
      room.left.answerJoin(decision.joinRound, PageFlipJoinVerdict::Adjacent, 2, 0, 400, resolution));
  EXPECT_FALSE(resolution.resolved);
}

// The reader re-reads the setting every pump, so setPeerMac() is called with the SAME value on
// every frame. If that restarted the round, no join would ever survive long enough to be answered
// and the pair would never classify at all -- a dead feature with nothing on screen to say so. The
// unpaired case matters just as much: it is the one every solo device takes, forever.
TEST_F(PageFlipSessionTest, ReapplyingTheSameSettingIsANoOp) {
  Room room;
  ASSERT_TRUE(room.start());
  ASSERT_TRUE(room.pairLeftToRight());

  ASSERT_TRUE(room.right.announceHello(2, 0, 400));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  ASSERT_EQ(decision.action, PageFlipAction::PeerHello);
  ASSERT_TRUE(decision.joinProbe);

  // What a handful of pumps between the probe and its answer would do.
  for (int pump = 0; pump < 5; ++pump) ASSERT_TRUE(room.pairLeftToRight());

  PageFlipJoinResolution resolution;
  EXPECT_TRUE(room.left.answerJoin(decision.joinRound, PageFlipJoinVerdict::Adjacent, 2, 0, 400, resolution));
}

TEST_F(PageFlipSessionTest, ReapplyingNoPairedDeviceIsANoOp) {
  Room room;
  ASSERT_TRUE(room.start());

  ASSERT_TRUE(room.right.announceHello(2, 0, 400));
  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(room.left, decision));
  ASSERT_TRUE(decision.joinProbe);

  for (int pump = 0; pump < 5; ++pump) room.left.setPeerMac(nullptr);

  PageFlipJoinResolution resolution;
  EXPECT_TRUE(room.left.answerJoin(decision.joinRound, PageFlipJoinVerdict::Adjacent, 2, 0, 400, resolution));
}

}  // namespace
