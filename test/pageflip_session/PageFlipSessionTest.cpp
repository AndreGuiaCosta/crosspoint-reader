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
  ASSERT_TRUE(pair.left.announceHello(3, 2, true));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_TRUE(decision.peerWantsReply);
  EXPECT_EQ(decision.spineIndex, 3);
  EXPECT_TRUE(decision.applyRoleOffset);
  EXPECT_EQ(pair.right.getTurnSeq(), 847u) << "the greeting carries the pair's counter";
}

// The answer must not itself be answered, or two devices greet each other forever.
TEST_F(PageFlipSessionTest, AnswerToAGreetingAsksForNothingBack) {
  Pair pair;
  ASSERT_TRUE(pair.start());

  ASSERT_TRUE(pair.right.announceHello(1, 1, false));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.left, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_FALSE(decision.peerWantsReply);
}

TEST_F(PageFlipSessionTest, GreetingForAnotherBookIsNotOurPeer) {
  Pair pair;
  ASSERT_TRUE(pair.start());
  pair.left.setBook(0x1234u, COMPAT);

  ASSERT_TRUE(pair.left.announceHello(0, 0, true));

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

  ASSERT_TRUE(pair.left.announceHello(3, 2, true));

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

  ASSERT_TRUE(pair.left.announceHello(0, 0, true));

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

  ASSERT_TRUE(pair.left.announceHello(0, 0, true));

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

  ASSERT_TRUE(pair.left.announceHello(0, 0, true));

  PageFlipDecision decision;
  ASSERT_TRUE(pollWithRetry(pair.right, decision));
  EXPECT_EQ(decision.action, PageFlipAction::PeerHello);
  EXPECT_FALSE(decision.layoutDecided);
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

}  // namespace
