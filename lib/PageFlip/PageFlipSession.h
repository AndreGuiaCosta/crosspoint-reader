#pragma once

#include <cstdint>

#include "PageFlipPacket.h"
#include "PageFlipTransport.h"

// The pair protocol (docs/pageflip.md section 3): what a device does with a turn its peer just
// broadcast. Deliberately free of the reader -- it decides, the activity applies -- so the rules
// that are easy to get subtly wrong (dedup, drift healing, simultaneous presses) are exercised by
// host tests against two real sessions rather than only through the UI.
//
// The model is "each device advances itself by two". Neither side is in charge, so a press on the
// right device does not invert the pages and no press needs an extra hop.

// What the reader should do with a received packet.
enum class PageFlipAction : uint8_t {
  Ignore,      // stale, duplicate, a different book, or our own turn coming back
  AdvanceTwo,  // the expected next turn: step the local position twice, same direction
  Heal,        // turns were missed or the pair conflicted: seek absolutely instead of replaying
  Mismatch,    // same book, incompatible layout -- report it (section 5), never apply it
  PeerHello,   // a peer announced itself: the pair is real, and turns may advance by two
  // The three steps of the settings force-sync (section 5.1), from the receiving side of each.
  SettingsOffer,   // the peer wants to push its render settings: preflight them and answerOffer()
  SettingsAnswer,  // our offer came back judged: commitOffer() on Ok, otherwise tell the user why
  SettingsApply,   // the offer we preflighted was committed: write it and rebuild the layout
  JoinResume,      // the user picked the peer's position for the pair: seek there, plus role offset
};

// Where the two devices stand relative to each other, once the join negotiation has run
// (docs/pageflip.md section 4.2).
enum class PageFlipJoinCase : uint8_t {
  // Both devices are on the same page. Section 4.2's table has no row for this, and it is the most
  // common join there is: a book opened for the first time on both, or two devices resumed from the
  // same synced progress. It is also the only case that has to CREATE the spread rather than
  // recognise one -- the right device steps forward, and the pair is a spread from then on.
  //
  // Note it cannot be read off the two verdicts: both devices answer NotAdjacent from the same
  // offset, which is indistinguishable from divergent. It is a direct equality test instead.
  Identical,
  // Left is at P and right at P+1: the ordinary reopen. Resume silently.
  Aligned,
  // Right is at P and left at P+1. The user physically swapped the devices; re-normalise to role.
  Swapped,
  // Two unrelated positions -- the devices were read separately. Prompt on both (section 4.3).
  Divergent,
};

// The outcome of answerJoin(), which is where every classification happens. Both devices reach it:
// the one that hears the peer's verdict last and the one that computes its own last both end up
// here, which is what keeps the join from working in one direction and being silently dead in the
// other -- the same shape of half-broken pair the turnSeq adoption rule exists to prevent.
struct PageFlipJoinResolution {
  bool resolved = false;
  PageFlipJoinCase joinCase = PageFlipJoinCase::Divergent;
  // Where the peer says it is. Every case except Aligned moves this device relative to it, and the
  // anchor is the offset -- a page number would be counted in the peer's layout, not this one's.
  int32_t peerSpineIndex = 0;
  uint32_t peerVisibleTextOffset = 0;
  PageFlipRole peerRole = PageFlipRole::Left;
};

struct PageFlipDecision {
  PageFlipAction action = PageFlipAction::Ignore;

  // AdvanceTwo: the direction the turn travelled, to step twice.
  // Heal: which way the role offset below points -- fixed by role, not by the sender's direction,
  // because right is always left + 1 however the pair got there.
  bool forward = true;

  // Heal only: the sender's own resulting position. Seek here, then take one further step in
  // `forward` when `applyRoleOffset` is set -- the pair is one page apart, and that page is only
  // knowable by stepping, not by arithmetic (a section boundary may sit between the two).
  int32_t spineIndex = 0;
  int32_t pageNumber = 0;
  bool applyRoleOffset = false;

  // PeerHello only: the greeting expects an answer. An answer does not, which is what keeps two
  // devices from greeting each other forever.
  bool peerWantsReply = false;

  // PeerHello only: the join negotiation (section 4.2) has a question for the reader. Set only once
  // the layouts are known to agree, because that ordering is mandatory -- classifying first would
  // compare positions taken from two different layouts, which means nothing.
  bool joinProbe = false;
  // Where the peer says it is. `spineIndex` above carries its spine; this is the anchor to compare
  // against, and the only part of a peer's position that survives a difference in layout.
  uint32_t peerVisibleTextOffset = 0;
  // Which negotiation this question belongs to. Handed back to answerJoin() so an answer computed
  // against a position the peer has since moved off is rejected rather than classifying the pair
  // from a stale offset -- the same staleness rule the force-sync answer takes on targetHash.
  uint32_t joinRound = 0;

  // False while either device is still advertising the "not computed yet" compat sentinel, which
  // it does until its own first render has fixed the viewport. Comparing against that decides
  // nothing, so the packet must not be treated as agreement OR as a mismatch -- see poll().
  bool layoutDecided = true;

  // SettingsOffer and SettingsApply: the settings to preflight, then to write. Points at storage the
  // session owns, and is valid for as long as the caller is handling this decision -- long enough to
  // evaluate or apply it, never long enough to keep.
  const PageFlipRenderSettings* settings = nullptr;

  // SettingsAnswer: how the push turned out, already checked against this device's own layout, so
  // Ok here really does mean "committing this converges the pair".
  PageFlipSyncResult syncResult = PageFlipSyncResult::Unknown;

  // The peer's own role, so a message about it can name a device: "the right device has no font
  // Bookerly". Distinct from applyRoleOffset, which only says whether the roles differ.
  PageFlipRole peerRole = PageFlipRole::Left;
};

class PageFlipSession {
 public:
  PageFlipSession(PageFlipTransport& transport, PageFlipRole role) : transport(transport), role(role) {}

  // The book in front of the reader. A packet for any other book is ignored outright, and one for
  // this book with a different compatHash reports Mismatch rather than desyncing silently.
  void setBook(uint32_t bookId, uint32_t compatHash);

  // The one device this session will listen to (docs/pageflip.md section 8.1). Without it the
  // filters are bookId and compatHash, which say "somebody reading the same book, laid out the same
  // way" -- true of any X4 in the room, and the pair a stranger joins is a pair whose pages move on
  // their own. Pass nullptr or an all-zero MAC to listen to anyone, which is what a device that has
  // never been through the pairing screen does.
  //
  // Filtering is per device and therefore asymmetric: pairing on one half and not the other leaves
  // that half open. It is a real state and it fails quietly, so the pairing screen tells both users.
  void setPeerMac(const uint8_t mac[PageFlipTransport::MAC_BYTES]);
  bool hasPeerMac() const { return peerMacSet; }

  bool begin();
  void end();
  bool isStarted() const { return transport.isStarted(); }

  // A local page turn that has already been applied here. Bumps turnSeq and broadcasts the
  // resulting position. Returns false if nothing could be sent (no peer yet) -- which is not an
  // error: reading must never block on the pair.
  bool announceLocalTurn(bool forward, int32_t spineIndex, int32_t pageNumber, bool atBookEnd);

  // Says "I am here, reading this, my counter is at N, and here is where in the text I am". Sent
  // when the reader opens the book and again whenever this device's layout changes. Presence has to
  // be established before turns may advance by two: a link that came up is not a peer that is
  // there, and a lone device advancing by two would turn two pages on every press.
  //
  // Always a greeting, never an answer: it opens a fresh join round. That is what makes section
  // 4.4's "rotation re-triggers the negotiation" the same code path as a first join instead of a
  // recovery mode of its own. The reply to a peer's greeting goes out from answerJoin() instead,
  // because a reply has a verdict to carry and this does not.
  bool announceHello(int32_t spineIndex, int32_t pageNumber, uint32_t visibleTextOffset);

  // The reader's half of the join negotiation (section 4.2), in answer to a decision whose
  // `joinProbe` was set: the verdict this device computed against the offset the probe reported,
  // plus where this device itself is. Both have to arrive together -- the classification needs this
  // device's own position for the Identical test, and that position can only be read when no render
  // is in flight, which is the same moment the verdict can be computed.
  //
  // Returns false without touching `resolution` when `round` is not the current one: the peer moved
  // after the probe went out, so an answer about the old offset would classify the pair from a
  // position neither device is at. `resolution.resolved` is set at most once per round, so the
  // caller may act on it directly -- a second Identical would step the right device forward twice.
  bool answerJoin(uint32_t round, PageFlipJoinVerdict verdict, int32_t spineIndex, int32_t pageNumber,
                  uint32_t visibleTextOffset, PageFlipJoinResolution& resolution);

  // The user's answer to a divergent join (section 4.3): the pair reads from where this device is.
  // The confirming device does not move -- the peer seeks here and takes its role offset from it,
  // which is what makes the gesture "pick a device" rather than "answer a question".
  bool proposeResume(int32_t spineIndex, uint32_t visibleTextOffset);

  // "I am here, this is my layout, this is where I am." Asks for nothing back and starts no join
  // round, which is what makes it usable for the two jobs that need exactly that:
  //
  //  - answering a greeting from a peer this device cannot pair with (section 5). The answer
  //    carries this device's hash, which is how the other user gets told about the mismatch too,
  //    and two mismatched devices greeting each other would never stop.
  //  - the periodic heartbeat that keeps presence alive once a peer has been seen (section 4).
  //    Presence licenses the two-step advance, so it must expire when a peer goes away -- and a
  //    peer that powered off or walked out of range says nothing on its way out.
  bool announcePresence(int32_t spineIndex, int32_t pageNumber, uint32_t visibleTextOffset);

  // Pumped once per frame. Returns true when a packet was received and `decision` was filled; the
  // decision may still be Ignore, which is not the same as "nothing arrived" -- only a decoded
  // packet from the paired book counts as peer contact for the auto-sleep timer (section 4).
  bool poll(PageFlipDecision& decision);

  // Settings force-sync (section 5.1). This device is the source: the user confirmed here, so these
  // settings are the ones the pair adopts. Refuses before the first render, because the offer names
  // the layout the peer has to reach and this device does not know its own yet.
  bool offerSettings(const PageFlipRenderSettings& settings);

  // The preflight verdict for the offer this device is holding. `resultHash` is the compatHash this
  // device would have after applying -- not a "yes I can", which would pass while leaving the two
  // devices laid out differently.
  bool answerOffer(PageFlipSyncResult result, uint32_t resultHash);

  // Commits the offer this device made, once its answer proved the peer converges. Separate from
  // the answer arriving so the caller can put the decision in front of the user first.
  bool commitOffer();

  // Drops both sides of the exchange: a refused answer, a peer that went quiet, or a book change.
  // Nothing has been written at this point, which is the whole reason the protocol has three steps.
  void cancelSync();

  bool hasOutstandingOffer() const { return offerOutstanding; }

  uint32_t getTurnSeq() const { return turnSeq; }

  // Join (section 3): a cold boot resets the counter, so the pair adopts max(local, peer) rather
  // than starting from zero. Without this a rebooted device's presses are all read as "already
  // applied" by an awake peer and do nothing at all, permanently.
  void adoptTurnSeq(uint32_t peerTurnSeq);

  PageFlipRole getRole() const { return role; }
  // Changing role invalidates the classification: which half of the spread this device shows is
  // what the join concluded, so it has to be concluded again. Done here rather than left to the
  // caller because a role change that kept the old classification is invisible -- the pair simply
  // sits one page wrong.
  void setRole(PageFlipRole newRole) {
    if (newRole == role) return;
    role = newRole;
    beginJoinRound();
  }

 private:
  // Lower MAC wins a conflict (section 3). Returns true when this device is the winner.
  bool winsTiebreakAgainst(const uint8_t peerMac[PageFlipTransport::MAC_BYTES]) const;

  // Forgets everything heard about the peer's position and re-arms the classification. Called when
  // either device greets, which is the definition of a join starting: a greeting is what a device
  // sends on opening the book and on changing its layout, and both must re-negotiate.
  void beginJoinRound();

  // The one place a hello goes on the wire, so the greeting, the reply and the decline cannot drift
  // apart in what they claim about this device.
  bool sendHello(int32_t spineIndex, int32_t pageNumber, uint32_t visibleTextOffset, bool wantsReply, bool startsJoin,
                 PageFlipJoinVerdict verdict);

  // Whether the two hashes can be compared at all. Zero is the "not computed yet" sentinel -- the
  // viewport is a render() output, so a device advertises zero from the moment its link comes up
  // until its first page has been laid out, which on a cold cache is seconds. Treating that as a
  // mismatch would report incompatibility between two identically configured devices every time
  // one rendered faster than the other.
  bool canCompareLayout(uint32_t peerHash) const { return compatHash != 0 && peerHash != 0; }

  // Whether a packet from this sender is one this session will look at at all.
  bool isPairedSender(const uint8_t senderMac[PageFlipTransport::MAC_BYTES]) const;

  PageFlipTransport& transport;
  PageFlipRole role;
  // The paired device, when the user has chosen one. Held as a flag plus the bytes rather than "all
  // zeroes means none", so a transport that cannot report a MAC can never be mistaken for a peer.
  bool peerMacSet = false;
  uint8_t peerMac[PageFlipTransport::MAC_BYTES] = {};
  uint32_t bookId = 0;
  uint32_t compatHash = 0;
  uint32_t turnSeq = 0;
  // Which way the last local press went, so a peer packet carrying the same turnSeq can be told
  // apart: same direction is the harmless simultaneous press, opposite is the conflict.
  bool lastLocalForward = true;
  bool hasLocalTurn = false;

  // Force-sync, as the source: an offer is out and an answer is expected for this exact layout.
  // Answers naming any other layout are stale -- the settings moved on after the offer went out.
  bool offerOutstanding = false;
  uint32_t offeredHash = 0;

  // Force-sync, as the device being pushed to: an offer preflighted and waiting on its commit. Held
  // here rather than by the caller so an apply arriving later cannot be honoured against settings
  // nobody checked.
  bool offerPending = false;
  uint32_t pendingOfferHash = 0;
  PageFlipRenderSettings pendingOffer;

  // The join negotiation (section 4.2). The round counter only has to outlive the gap between a
  // probe going out and its answer coming back, so it is never compared for order -- just equality.
  uint32_t joinRound = 0;
  bool joinPeerKnown = false;
  int32_t joinPeerSpineIndex = 0;
  uint32_t joinPeerVisibleTextOffset = 0;
  PageFlipRole joinPeerRole = PageFlipRole::Left;
  PageFlipJoinVerdict joinPeerVerdict = PageFlipJoinVerdict::Unknown;
  // A greeting owed a reply. Held here rather than by the caller because the reply carries the
  // verdict, and the verdict is not known until the caller answers -- so the two have to be the
  // same call.
  bool joinPeerWantsReply = false;
  // Latched so the pair is classified once per round. Without it every later greeting in the same
  // round would resolve again, and Identical would step the right device forward once per packet.
  bool joinResolved = false;
  // A resume this device proposed (section 4.3). Kept only for the tiebreak: two users confirming
  // in the same window would otherwise each seek to the other, and the pair would swap positions
  // instead of settling on one.
  bool resumeProposed = false;
};
