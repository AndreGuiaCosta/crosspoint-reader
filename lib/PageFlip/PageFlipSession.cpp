#include "PageFlipSession.h"

#include <cstring>

void PageFlipSession::setBook(uint32_t newBookId, uint32_t newCompatHash) {
  // A force-sync in flight is an agreement about one specific layout on both ends. If this device's
  // own layout moves under it -- the user rotated, or changed a setting mid-exchange -- the offer
  // and the answer are both about something that no longer exists, and every later packet would be
  // discarded as stale anyway. Dropping it here is what keeps that from reading as a peer that
  // simply never replied.
  if (newBookId != bookId || newCompatHash != compatHash) cancelSync();
  bookId = newBookId;
  compatHash = newCompatHash;
  // turnSeq deliberately survives a book change. It is a session counter shared by the pair, not a
  // per-book one, and resetting it re-creates exactly the deadlock adoption exists to prevent: a
  // peer sitting on a high count reads every low-numbered turn as "already applied".
}

bool PageFlipSession::begin() { return transport.begin(); }

void PageFlipSession::end() { transport.end(); }

void PageFlipSession::adoptTurnSeq(uint32_t peerTurnSeq) {
  if (peerTurnSeq > turnSeq) turnSeq = peerTurnSeq;
}

bool PageFlipSession::winsTiebreakAgainst(const uint8_t peerMac[PageFlipTransport::MAC_BYTES]) const {
  uint8_t localMac[PageFlipTransport::MAC_BYTES] = {};
  if (!transport.localMac(localMac)) return false;  // no identity: let the peer lead
  return std::memcmp(localMac, peerMac, PageFlipTransport::MAC_BYTES) < 0;
}

bool PageFlipSession::announceLocalTurn(bool forward, int32_t spineIndex, int32_t pageNumber, bool atBookEnd) {
  // The counter advances even when nothing is listening. A peer that joins later adopts the higher
  // value (section 3), so a solo run does not leave the pair permanently out of step.
  ++turnSeq;
  lastLocalForward = forward;
  hasLocalTurn = true;

  PageFlipTurn turn;
  turn.compatHash = compatHash;
  turn.bookId = bookId;
  turn.turnSeq = turnSeq;
  turn.spineIndex = spineIndex;
  turn.pageNumber = pageNumber;
  turn.role = role;
  turn.forward = forward;
  turn.atBookEnd = atBookEnd;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeTurn(turn, wire, sizeof(wire), wireLength)) return false;
  return transport.broadcast(wire, wireLength);
}

bool PageFlipSession::announceHello(int32_t spineIndex, int32_t pageNumber, bool wantsReply) {
  PageFlipHello hello;
  hello.compatHash = compatHash;
  hello.bookId = bookId;
  hello.turnSeq = turnSeq;
  hello.spineIndex = spineIndex;
  hello.pageNumber = pageNumber;
  hello.role = role;
  hello.wantsReply = wantsReply;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeHello(hello, wire, sizeof(wire), wireLength)) return false;
  return transport.broadcast(wire, wireLength);
}

bool PageFlipSession::offerSettings(const PageFlipRenderSettings& settings) {
  // An offer names the layout the peer has to reach, so a device that has not rendered has nothing
  // to offer: the sentinel is not a layout, and a peer converging on it would mean nothing.
  if (compatHash == 0) return false;

  PageFlipSyncOffer offer;
  offer.compatHash = compatHash;
  offer.bookId = bookId;
  offer.settings = settings;
  offer.role = role;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeSyncOffer(offer, wire, sizeof(wire), wireLength)) return false;
  if (!transport.broadcast(wire, wireLength)) return false;

  offerOutstanding = true;
  offeredHash = compatHash;
  return true;
}

bool PageFlipSession::answerOffer(PageFlipSyncResult result, uint32_t resultHash) {
  if (!offerPending) return false;

  PageFlipSyncAnswer answer;
  answer.targetHash = pendingOfferHash;
  answer.bookId = bookId;
  answer.resultHash = resultHash;
  answer.result = result;
  answer.role = role;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeSyncAnswer(answer, wire, sizeof(wire), wireLength)) return false;
  // A refusal ends the exchange here: no apply can follow one, so holding the offer would only leave
  // a stale commit to honour later.
  if (result != PageFlipSyncResult::Ok) {
    offerPending = false;
    pendingOfferHash = 0;
  }
  return transport.broadcast(wire, wireLength);
}

bool PageFlipSession::commitOffer() {
  if (!offerOutstanding) return false;

  PageFlipSyncApply apply;
  apply.targetHash = offeredHash;
  apply.bookId = bookId;
  apply.role = role;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeSyncApply(apply, wire, sizeof(wire), wireLength)) return false;

  offerOutstanding = false;
  offeredHash = 0;
  return transport.broadcast(wire, wireLength);
}

void PageFlipSession::cancelSync() {
  offerOutstanding = false;
  offeredHash = 0;
  offerPending = false;
  pendingOfferHash = 0;
}

bool PageFlipSession::poll(PageFlipDecision& decision) {
  uint8_t buffer[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t length = 0;
  uint8_t senderMac[PageFlipTransport::MAC_BYTES] = {};
  if (!transport.poll(buffer, sizeof(buffer), length, senderMac)) return false;

  // Anything that is not a decodable PageFlip turn is not peer contact, and returning false here is
  // what keeps a stray broadcaster on the channel from holding the device awake (section 4).
  PageFlipMessage message = PageFlipMessage::Turn;
  if (!PageFlipPacket::peekMessage(buffer, length, message)) return false;

  decision = PageFlipDecision{};

  if (message == PageFlipMessage::Hello) {
    PageFlipHello hello;
    if (!PageFlipPacket::decodeHello(buffer, length, hello)) return false;
    if (hello.bookId != bookId) return true;  // a peer on another book is present but irrelevant

    // Adopting the peer's counter is what the greeting is for. A cold boot resets turnSeq, and
    // without adoption the rebooted device's presses would all look "already applied" to an awake
    // peer -- its presses would do nothing at all, permanently, while the reverse direction
    // appeared to work.
    adoptTurnSeq(hello.turnSeq);
    decision.peerWantsReply = hello.wantsReply;
    decision.spineIndex = hello.spineIndex;
    decision.pageNumber = hello.pageNumber;
    decision.applyRoleOffset = hello.role != role;
    // The greeting is the earliest a layout difference can be seen and the cheapest place to see
    // it: no position has been applied yet, so nothing has to be undone. Note the counter above is
    // adopted either way -- turnSeq is a session fact, not a layout one, and skipping it would
    // leave the pair deadlocked the moment force-sync (section 5.1) made them compatible.
    //
    // Unless one side has not rendered yet, in which case there is nothing to compare and the
    // honest verdict is neither "compatible" nor "incompatible". The caller still answers the
    // greeting; both devices re-greet once their own first render fixes the viewport, so the pair
    // converges a round later without anyone being told a layout story that was never true.
    decision.layoutDecided = canCompareLayout(hello.compatHash);
    decision.action = (!decision.layoutDecided || hello.compatHash == compatHash) ? PageFlipAction::PeerHello
                                                                                 : PageFlipAction::Mismatch;
    return true;
  }

  // Settings force-sync (section 5.1). None of the three runs the layout comparison the turn and
  // hello paths do: these packets exist precisely because the layouts differ, and reporting that
  // difference again here would tell the user the pair is broken in the middle of repairing it.
  if (message == PageFlipMessage::SyncOffer) {
    PageFlipSyncOffer offer;
    if (!PageFlipPacket::decodeSyncOffer(buffer, length, offer)) return false;
    if (offer.bookId != bookId) return true;
    // An offer carrying the sentinel names no layout to converge on -- a device that has not
    // rendered cannot be a source. Its own offerSettings() refuses too; this is the wire-side half.
    if (offer.compatHash == 0) return true;

    // Both users confirming in the same window: the same lower-MAC tiebreak a conflicting turn
    // takes. The winner keeps its own offer and ignores this one, the loser drops its offer and
    // preflights this one, so the pair converges on one push rather than overwriting each other.
    if (offerOutstanding && winsTiebreakAgainst(senderMac)) return true;
    offerOutstanding = false;
    offeredHash = 0;

    pendingOffer = offer.settings;
    pendingOfferHash = offer.compatHash;
    offerPending = true;

    decision.action = PageFlipAction::SettingsOffer;
    decision.settings = &pendingOffer;
    decision.peerRole = offer.role;
    decision.applyRoleOffset = offer.role != role;
    return true;
  }

  if (message == PageFlipMessage::SyncAnswer) {
    PageFlipSyncAnswer answer;
    if (!PageFlipPacket::decodeSyncAnswer(buffer, length, answer)) return false;
    if (answer.bookId != bookId) return true;
    // Stale: either nothing is outstanding, or this device's settings moved on after the offer went
    // out, so the answer judges a layout it no longer has.
    if (!offerOutstanding || answer.targetHash != offeredHash) return true;

    // The verdict is only worth as much as the hash behind it. A peer that answers Ok while landing
    // on a different layout has checked the things it knows to check and still diverged -- which is
    // the failure this comparison exists to catch, so it outranks the peer's own opinion.
    const bool converges = answer.result == PageFlipSyncResult::Ok && answer.resultHash == compatHash;
    decision.action = PageFlipAction::SettingsAnswer;
    decision.peerRole = answer.role;
    decision.syncResult = converges                                 ? PageFlipSyncResult::Ok
                          : answer.result == PageFlipSyncResult::Ok ? PageFlipSyncResult::Unknown
                                                                    : answer.result;
    // Nothing left to commit on a refusal, and nothing has been written on either device.
    if (!converges) cancelSync();
    return true;
  }

  if (message == PageFlipMessage::SyncApply) {
    PageFlipSyncApply apply;
    if (!PageFlipPacket::decodeSyncApply(buffer, length, apply)) return false;
    if (apply.bookId != bookId) return true;
    // Only the offer this device actually preflighted may be committed. Honouring any other commit
    // would write settings nobody checked, which is the whole failure the three steps prevent.
    if (!offerPending || apply.targetHash != pendingOfferHash) return true;

    offerPending = false;
    pendingOfferHash = 0;
    decision.action = PageFlipAction::SettingsApply;
    // The buffer outlives the flag: the caller is being handed settings it preflighted itself.
    decision.settings = &pendingOffer;
    decision.peerRole = apply.role;
    decision.applyRoleOffset = apply.role != role;
    return true;
  }

  PageFlipTurn incoming;
  if (!PageFlipPacket::decodeTurn(buffer, length, incoming)) return false;

  // A peer reading a different book is alive but has nothing to say about this one.
  if (incoming.bookId != bookId) return true;

  // Same sentinel rule as the greeting above. A turn arriving while either side is still
  // undecided is not applied -- stepping a position whose layout is unknown is exactly the desync
  // this guards -- but it is not reported either. The next turn after both have rendered runs
  // ahead of the counter, so it heals absolutely rather than replaying, which is self-correcting.
  if (!canCompareLayout(incoming.compatHash)) {
    decision.layoutDecided = false;
    return true;  // Ignore
  }

  if (incoming.compatHash != compatHash) {
    decision.action = PageFlipAction::Mismatch;
    return true;
  }

  decision.spineIndex = incoming.spineIndex;
  decision.pageNumber = incoming.pageNumber;
  // Roles that differ are the normal pair, one page apart. Two devices configured with the same
  // role is a setup error the join negotiation catches (section 4); here it simply means no offset.
  decision.applyRoleOffset = incoming.role != role;
  // Which way to step. For an advance it is the direction the turn travelled. For a heal it is
  // NOT: the role offset is fixed by role, not by travel -- right is always left + 1 -- so the
  // right device steps forward off the sender's position however the sender got there. Reading the
  // sender's direction here would land a backward heal two pages out, and it would look like a
  // boundary bug in advanceOnePage rather than a protocol one.
  decision.forward = incoming.forward;
  const bool roleOffsetForward = role == PageFlipRole::Right;

  // Both devices pressing in the same window independently compute the same turnSeq. Same
  // direction is harmless -- each ignores the other and the pair advances once, which is the whole
  // point of the counter. Opposite directions is the residual race, and it desyncs unless someone
  // yields, so the lower MAC wins and the loser heals from the winner's absolute position.
  if (hasLocalTurn && incoming.turnSeq == turnSeq && incoming.forward != lastLocalForward) {
    // Winning leaves hasLocalTurn set on purpose: our turn stands, so a retransmission of the
    // peer's losing packet must reach this same branch and be rejected the same way.
    if (winsTiebreakAgainst(senderMac)) return true;  // Ignore: the peer heals to us
    decision.action = PageFlipAction::Heal;
    decision.forward = roleOffsetForward;
    hasLocalTurn = false;
    return true;
  }

  // Already applied, or our own turn observed coming back. The absolute position it carries is a
  // consistency check we do not use yet -- worth wiring in once the reader can compare cheaply.
  // (turnSeq is monotonic and 32 bits: at one turn per second it wraps after ~136 years.)
  if (incoming.turnSeq <= turnSeq) return true;

  // Exactly the next turn: step locally, running the same boundary algorithm against this device's
  // own section rather than trusting page arithmetic the peer cannot do for us. Anything further
  // ahead means turns were missed, and replaying them would be guesswork -- seek absolutely
  // instead, which is what makes a dropped packet self-correcting rather than permanent drift.
  const bool isNextTurn = incoming.turnSeq == turnSeq + 1;
  turnSeq = incoming.turnSeq;
  hasLocalTurn = false;
  decision.action = isNextTurn ? PageFlipAction::AdvanceTwo : PageFlipAction::Heal;
  if (!isNextTurn) decision.forward = roleOffsetForward;
  return true;
}
