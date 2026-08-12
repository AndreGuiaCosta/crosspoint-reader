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

  // Pumped once per frame. Returns true when a packet was received and `decision` was filled; the
  // decision may still be Ignore, which is not the same as "nothing arrived" -- only a decoded
  // packet from the paired book counts as peer contact for the auto-sleep timer (section 4).
  bool poll(PageFlipDecision& decision);

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

  PageFlipTransport& transport;
  PageFlipRole role;
  uint32_t bookId = 0;
  uint32_t compatHash = 0;
  uint32_t turnSeq = 0;
  // Which way the last local press went, so a peer packet carrying the same turnSeq can be told
  // apart: same direction is the harmless simultaneous press, opposite is the conflict.
  bool lastLocalForward = true;
  bool hasLocalTurn = false;
};
