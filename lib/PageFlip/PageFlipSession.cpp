#include "PageFlipSession.h"

#include <cstring>

void PageFlipSession::setBook(uint32_t newBookId, uint32_t newCompatHash) {
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
