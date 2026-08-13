#include "PageFlipSession.h"

#include <cstring>

void PageFlipSession::setBook(uint32_t newBookId, uint32_t newCompatHash) {
  // A force-sync in flight is an agreement about one specific layout on both ends. If this device's
  // own layout moves under it -- the user rotated, or changed a setting mid-exchange -- the offer
  // and the answer are both about something that no longer exists, and every later packet would be
  // discarded as stale anyway. Dropping it here is what keeps that from reading as a peer that
  // simply never replied.
  if (newBookId != bookId || newCompatHash != compatHash) {
    cancelSync();
    // And the join goes with it. A classification is a statement about two positions inside one
    // shared pagination; the moment this device's own layout moves, its half of that statement
    // counts pages that no longer exist. Section 4.4's rotation case is exactly this path -- there
    // is no separate recovery mode because re-negotiating IS the recovery.
    beginJoinRound();
  }
  bookId = newBookId;
  compatHash = newCompatHash;
  // turnSeq deliberately survives a book change. It is a session counter shared by the pair, not a
  // per-book one, and resetting it re-creates exactly the deadlock adoption exists to prevent: a
  // peer sitting on a high count reads every low-numbered turn as "already applied".
}

void PageFlipSession::setPeerMac(const uint8_t mac[PageFlipTransport::MAC_BYTES]) {
  if (mac == nullptr) {
    peerMacSet = false;
    std::memset(peerMac, 0, sizeof(peerMac));
    return;
  }
  static constexpr uint8_t NO_MAC[PageFlipTransport::MAC_BYTES] = {};
  if (std::memcmp(mac, NO_MAC, sizeof(peerMac)) == 0) {
    peerMacSet = false;
    std::memset(peerMac, 0, sizeof(peerMac));
    return;
  }
  if (peerMacSet && std::memcmp(peerMac, mac, sizeof(peerMac)) == 0) return;

  std::memcpy(peerMac, mac, sizeof(peerMac));
  peerMacSet = true;
  // Everything concluded so far was concluded with somebody else. A join classifies two positions
  // against each other, and the other one may now be a device this session will not even listen to.
  beginJoinRound();
  cancelSync();
}

bool PageFlipSession::isPairedSender(const uint8_t senderMac[PageFlipTransport::MAC_BYTES]) const {
  if (!peerMacSet) return true;  // never paired: the bookId and compatHash filters are all there is
  if (senderMac == nullptr) return false;
  return std::memcmp(peerMac, senderMac, PageFlipTransport::MAC_BYTES) == 0;
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

void PageFlipSession::beginJoinRound() {
  ++joinRound;
  joinPeerKnown = false;
  joinPeerSpineIndex = 0;
  joinPeerVisibleTextOffset = 0;
  joinPeerVerdict = PageFlipJoinVerdict::Unknown;
  joinPeerWantsReply = false;
  joinResolved = false;
  // A choice made about the old round is not a choice about this one: the positions it named may
  // not exist in the pagination the pair is about to negotiate over.
  resumeProposed = false;
}

bool PageFlipSession::sendHello(int32_t spineIndex, int32_t pageNumber, uint32_t visibleTextOffset, bool wantsReply,
                                bool startsJoin, PageFlipJoinVerdict verdict) {
  PageFlipHello hello;
  hello.compatHash = compatHash;
  hello.bookId = bookId;
  hello.turnSeq = turnSeq;
  hello.spineIndex = spineIndex;
  hello.pageNumber = pageNumber;
  hello.visibleTextOffset = visibleTextOffset;
  hello.role = role;
  hello.wantsReply = wantsReply;
  hello.startsJoin = startsJoin;
  hello.joinVerdict = verdict;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeHello(hello, wire, sizeof(wire), wireLength)) return false;
  return transport.broadcast(wire, wireLength);
}

bool PageFlipSession::announceHello(int32_t spineIndex, int32_t pageNumber, uint32_t visibleTextOffset) {
  // Greeting means joining, and joining means the pair's relative position is unknown again. A
  // device sends this on opening the book and whenever its layout moves, and after a layout change
  // its own saved page number counts pages that no longer exist -- so anything concluded before it
  // has to go.
  beginJoinRound();
  // Nothing has been heard from the peer this round, so there is nothing to have judged.
  return sendHello(spineIndex, pageNumber, visibleTextOffset, true, true, PageFlipJoinVerdict::Unknown);
}

bool PageFlipSession::proposeResume(const int32_t spineIndex, const uint32_t visibleTextOffset) {
  // A position is only meaningful inside a pagination, and the sentinel is not one.
  if (compatHash == 0) return false;

  PageFlipJoinResume resume;
  resume.compatHash = compatHash;
  resume.bookId = bookId;
  resume.spineIndex = spineIndex;
  resume.visibleTextOffset = visibleTextOffset;
  resume.role = role;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  if (!PageFlipPacket::encodeJoinResume(resume, wire, sizeof(wire), wireLength)) return false;
  if (!transport.broadcast(wire, wireLength)) return false;

  resumeProposed = true;
  return true;
}

bool PageFlipSession::announcePresence(int32_t spineIndex, int32_t pageNumber, uint32_t visibleTextOffset) {
  // A statement, not a question: no reply asked for, no round started. Both callers need exactly
  // that. Answering an incompatible peer with "and hello to you" would have two mismatched devices
  // trading greetings for as long as they sat next to each other, and a heartbeat that restarted
  // the join would re-classify the pair every couple of seconds.
  return sendHello(spineIndex, pageNumber, visibleTextOffset, false, false, PageFlipJoinVerdict::Unknown);
}

bool PageFlipSession::answerJoin(const uint32_t round, const PageFlipJoinVerdict verdict, const int32_t spineIndex,
                                 const int32_t pageNumber, const uint32_t visibleTextOffset,
                                 PageFlipJoinResolution& resolution) {
  // The peer moved between the probe and this answer, so the verdict judges an offset it is no
  // longer at. Answering anyway would classify the pair from a position neither device holds.
  if (round != joinRound) return false;
  if (!joinPeerKnown) return false;

  resolution = PageFlipJoinResolution{};
  resolution.peerSpineIndex = joinPeerSpineIndex;
  resolution.peerVisibleTextOffset = joinPeerVisibleTextOffset;
  resolution.peerRole = joinPeerRole;

  if (!joinResolved) {
    // Identical first, and by direct equality rather than by the verdicts: two devices on the same
    // page each find their own next page somewhere the other is not, so both answer NotAdjacent and
    // the pair reads as divergent. This test is only sound because the join runs behind the layout
    // agreement of section 5 -- the same offset under two different layouts is not the same page --
    // which is why the probe is never raised for a peer whose hash has not been checked.
    const bool identical = spineIndex == joinPeerSpineIndex && visibleTextOffset == joinPeerVisibleTextOffset;
    if (identical) {
      resolution.resolved = true;
      resolution.joinCase = PageFlipJoinCase::Identical;
    } else if (verdict == PageFlipJoinVerdict::Adjacent) {
      // The peer is one page after this device, so this device is the front half of the spread.
      resolution.resolved = true;
      resolution.joinCase = role == PageFlipRole::Left ? PageFlipJoinCase::Aligned : PageFlipJoinCase::Swapped;
    } else if (joinPeerVerdict == PageFlipJoinVerdict::Adjacent) {
      // Mirror image: this device is one page after the peer, so the peer is the front half.
      resolution.resolved = true;
      resolution.joinCase =
          joinPeerRole == PageFlipRole::Left ? PageFlipJoinCase::Aligned : PageFlipJoinCase::Swapped;
    } else if (verdict == PageFlipJoinVerdict::NotAdjacent && joinPeerVerdict == PageFlipJoinVerdict::NotAdjacent) {
      // Both sides tested and neither found the other. Only now is the pair genuinely unrelated --
      // an Unknown on either side is "ask again", never a no, or a device whose section was still
      // building would put a resume prompt in front of the user for a pair that is perfectly fine.
      resolution.resolved = true;
      resolution.joinCase = PageFlipJoinCase::Divergent;
    }
    joinResolved = resolution.resolved;
  }

  // Reply only to a greeting. Answering an answer is what would have two devices talking forever,
  // and the rule is the same one the presence handshake has always used.
  if (!joinPeerWantsReply) return true;
  joinPeerWantsReply = false;

  // Ask for one more round only while the pair is still undecided AND this device contributed
  // something to decide with. A device that could not test its own position asks for nothing: two
  // of those would trade greetings at packet rate forever, and the retry it actually needs is its
  // own re-greeting once its section has settled.
  const bool wantsReply = !joinResolved && verdict != PageFlipJoinVerdict::Unknown;
  // Never a restart: however many rounds the exchange takes, it is one negotiation.
  sendHello(spineIndex, pageNumber, visibleTextOffset, wantsReply, false, verdict);
  return true;
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

  // A device the user did not pair with is not a peer, and the packet is not contact (section 8.1).
  // Returned as "nothing arrived" rather than as an Ignore decision, deliberately: an Ignore is
  // still peer contact, and contact holds this device awake and feeds the presence timer. A
  // stranger reading the same book would otherwise keep the reader from sleeping.
  if (!isPairedSender(senderMac)) return false;

  // Anything that is not a decodable PageFlip turn is not peer contact, and returning false here is
  // what keeps a stray broadcaster on the channel from holding the device awake (section 4).
  PageFlipMessage message = PageFlipMessage::Turn;
  if (!PageFlipPacket::peekMessage(buffer, length, message)) return false;

  // A pairing screen somewhere in the room, which has nothing to say to a reader -- not even that
  // somebody is there. Rejected explicitly rather than left to fall through to decodeTurn, so the
  // rule survives the next message type being added.
  if (message == PageFlipMessage::PairBeacon) return false;

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
    decision.peerVisibleTextOffset = hello.visibleTextOffset;
    decision.applyRoleOffset = hello.role != role;

    // Only a device announcing itself starts a round: a fresh boot, or a layout change that made
    // everything concluded about the old one meaningless. A reply belongs to the round already
    // running even when it asks for one more, and restarting on that would classify the pair a
    // second time -- Identical resolved twice steps the right device forward twice.
    if (hello.startsJoin) beginJoinRound();
    joinPeerKnown = true;
    joinPeerSpineIndex = hello.spineIndex;
    joinPeerVisibleTextOffset = hello.visibleTextOffset;
    joinPeerRole = hello.role;
    joinPeerVerdict = hello.joinVerdict;
    joinPeerWantsReply = joinPeerWantsReply || hello.wantsReply;
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
    // The join runs only behind a layout the pair has agreed on. That ordering is section 4.2's and
    // it is mandatory: a position is an offset into a pagination, and two devices laid out
    // differently do not share one. An undecided layout is not agreement either -- the sentinel
    // means one device has not rendered, so it does not know what it would be agreeing to.
    decision.joinProbe = decision.action == PageFlipAction::PeerHello && decision.layoutDecided;
    decision.joinRound = joinRound;
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

  if (message == PageFlipMessage::JoinResume) {
    PageFlipJoinResume resume;
    if (!PageFlipPacket::decodeJoinResume(buffer, length, resume)) return false;
    if (resume.bookId != bookId) return true;
    // A choice expressed in a pagination this device is not using cannot be seeked to by page, and
    // the role offset below is a step through one. The pair will re-negotiate once the layouts
    // agree again, so dropping this is a delay, not a loss.
    if (!canCompareLayout(resume.compatHash) || resume.compatHash != compatHash) return true;

    // Both users confirming in the same window. Without a tiebreak each device would seek to the
    // other and the pair would trade positions rather than settle on one -- the same failure a
    // conflicting turn has, resolved the same way.
    if (resumeProposed && winsTiebreakAgainst(senderMac)) return true;
    resumeProposed = false;

    decision.action = PageFlipAction::JoinResume;
    decision.spineIndex = resume.spineIndex;
    decision.peerVisibleTextOffset = resume.visibleTextOffset;
    decision.peerRole = resume.role;
    decision.applyRoleOffset = resume.role != role;
    // The offset is fixed by role and not by who chose: right is always left + 1, so the right
    // device lands one page after the chosen position and the left device one page before it.
    decision.forward = role == PageFlipRole::Right;
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
