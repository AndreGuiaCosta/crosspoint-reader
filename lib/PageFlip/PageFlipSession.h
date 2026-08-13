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

  bool begin();
  void end();
  bool isStarted() const { return transport.isStarted(); }

  // A local page turn that has already been applied here. Bumps turnSeq and broadcasts the
  // resulting position. Returns false if nothing could be sent (no peer yet) -- which is not an
  // error: reading must never block on the pair.
  bool announceLocalTurn(bool forward, int32_t spineIndex, int32_t pageNumber, bool atBookEnd);

  // Says "I am here, reading this, my counter is at N". Sent when the reader opens the book and
  // again as the answer to a peer's greeting. Presence has to be established before turns may
  // advance by two: a link that came up is not a peer that is there, and a lone device advancing
  // by two would turn two pages on every press.
  bool announceHello(int32_t spineIndex, int32_t pageNumber, bool wantsReply);

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
  void setRole(PageFlipRole newRole) { role = newRole; }

 private:
  // Lower MAC wins a conflict (section 3). Returns true when this device is the winner.
  bool winsTiebreakAgainst(const uint8_t peerMac[PageFlipTransport::MAC_BYTES]) const;

  // Whether the two hashes can be compared at all. Zero is the "not computed yet" sentinel -- the
  // viewport is a render() output, so a device advertises zero from the moment its link comes up
  // until its first page has been laid out, which on a cold cache is seconds. Treating that as a
  // mismatch would report incompatibility between two identically configured devices every time
  // one rendered faster than the other.
  bool canCompareLayout(uint32_t peerHash) const { return compatHash != 0 && peerHash != 0; }

  PageFlipTransport& transport;
  PageFlipRole role;
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
};
