#pragma once

#include <cstddef>
#include <cstdint>

#include "PageFlipRenderSettings.h"

// PageFlip wire format (docs/pageflip.md section 3).
//
// Two paired devices exchange positions, never framebuffers: a turn is a couple of dozen bytes.
// The packet is the ESP-NOW payload as-is -- the SDK's own encodePacket() framing is not used,
// because these fields already carry magic, version and type, and 16 bytes of extra header would
// double the payload for nothing.
//
// Encoding is explicit little-endian via memcpy: the wire must not depend on struct padding, and
// RISC-V faults on unaligned wide loads, so no field is ever read by casting the buffer.
//
// Deliberately free of Arduino/ESP-IDF includes so the host unit tests build it directly.

enum class PageFlipRole : uint8_t {
  Left = 0,
  Right = 1,
};

// The join negotiation (section 4.2), as one device's half of it: the answer to "does my next page
// start at the offset you just told me?". Neither device can test the other direction -- that would
// mean loading a section it does not have -- so the classification is these two answers combined.
//
// Rides in two bits of the hello's flags rather than a byte of its own, because the whole point of
// putting it on the greeting is that the join costs no extra packet.
enum class PageFlipJoinVerdict : uint8_t {
  // Not answerable yet, and the honest value to send: the section is still building, so "am I on my
  // last page" has no answer (section 3 -- pageCount is a watermark during a build), or no position
  // has been heard from the peer at all. Retried, never treated as a "no".
  Unknown = 0,
  // My next page begins exactly where you are. You are one page after me.
  Adjacent = 1,
  // My next page is somewhere else. Says nothing about which of us is ahead.
  NotAdjacent = 2,
};

// Discriminates the packets that share the transport.
enum class PageFlipMessage : uint8_t {
  Turn = 1,
  Hello = 2,
  // The three-step settings force-sync of section 5.1. Split into offer / answer / apply so that
  // an abort really does leave nothing written: the device being pushed to gets to preflight the
  // change and refuse it before anyone commits.
  SyncOffer = 3,
  SyncAnswer = 4,
  SyncApply = 5,
  // The user's answer to a divergent join (section 4.3).
  JoinResume = 6,
};

// A page turn that has already been applied on the sender.
struct PageFlipTurn {
  uint32_t compatHash = 0;  // section 5: layout compatibility, hashed from ReaderRenderSpec
  uint32_t bookId = 0;      // existing EPUB path hash
  uint32_t turnSeq = 0;     // shared logical turn count: dedup key and drift detector
  int32_t spineIndex = 0;   // the sender's own resulting position, never the peer's
  int32_t pageNumber = 0;
  PageFlipRole role = PageFlipRole::Left;
  bool forward = true;
  bool atBookEnd = false;
};

// A presence greeting, and the carrier of the join negotiation (section 4.2). A turn's 25 bytes
// plus the content offset and this device's verdict -- because a device has to answer the same
// questions either way: who am I, what am I reading, how far along is the pair's turn counter, and
// where in the text am I.
//
// Presence is not optional politeness: "the link came up" is not "a peer is there", and a device
// that advanced by two without a peer would turn two pages per press on its own.
struct PageFlipHello {
  uint32_t compatHash = 0;
  uint32_t bookId = 0;
  uint32_t turnSeq = 0;    // the join adopts max(local, peer) from this
  int32_t spineIndex = 0;  // where this device currently is
  int32_t pageNumber = 0;
  // The join's anchor. A page number is not comparable across two devices -- a force-sync (section
  // 5.1) invalidates the layout the receiving device's own saved page number was counted in -- so
  // the negotiation compares content offsets and nothing else. `pageNumber` above stays for the
  // heal path of section 3, which only ever runs between devices already proven to share a layout.
  uint32_t visibleTextOffset = 0;
  PageFlipRole role = PageFlipRole::Left;
  // A greeting asks for an answer; an answer does not, which is what stops two devices greeting
  // each other forever. Rides the same flag bit a turn uses for direction.
  bool wantsReply = true;
  // What this device made of the last position it heard from the peer.
  PageFlipJoinVerdict joinVerdict = PageFlipJoinVerdict::Unknown;
  // "Forget where I was; this is a fresh join." Set by a device opening a book or changing its
  // layout, and by nothing else -- a reply asking for one more round is still the same negotiation.
  //
  // Deliberately its own bit rather than inferred from `wantsReply`. A reply that asks for another
  // round would otherwise be indistinguishable from a greeting, and restarting the round on one
  // would classify the pair a second time: Identical, resolved twice, steps the right device
  // forward twice and lands the spread two pages apart.
  bool startsJoin = true;
};

// A push of one device's render settings onto the other (section 5.1). Sent by the device the user
// confirmed on; it is the source of truth for the whole exchange.
struct PageFlipSyncOffer {
  uint32_t compatHash = 0;  // the layout the peer must end up with for the push to be worth applying
  uint32_t bookId = 0;
  PageFlipRenderSettings settings;
  PageFlipRole role = PageFlipRole::Left;
};

// The preflight verdict, from the device being pushed to. `resultHash` is the compatHash that
// device would have *after* applying, which is what the source actually decides on -- a boolean
// "I have that font" would pass while leaving the two devices laid out differently.
struct PageFlipSyncAnswer {
  uint32_t targetHash = 0;  // echo of the offer's compatHash: which offer this answers
  uint32_t bookId = 0;
  uint32_t resultHash = 0;  // 0 when the peer could not evaluate the offer at all
  PageFlipSyncResult result = PageFlipSyncResult::Unknown;
  PageFlipRole role = PageFlipRole::Left;  // names the device in the abort message
};

// The commit. Only sent once an answer has proven the peer converges, so the expensive part -- a
// settings write and a full layout rebuild -- never runs for a change that would not have worked.
struct PageFlipSyncApply {
  uint32_t targetHash = 0;
  uint32_t bookId = 0;
  PageFlipRole role = PageFlipRole::Left;
};

// "The pair reads from here" -- the user's choice, when the join found two unrelated positions
// (section 4.3). Sent by the device the user confirmed on, which then does not move: the other one
// seeks to this position and takes its role offset from it.
//
// The position is a content offset for the same reason the join is: it is the only anchor that
// means the same thing on both devices. The compat hash rides along so a choice made under a
// layout the pair has since left is ignored rather than acted on.
struct PageFlipJoinResume {
  uint32_t compatHash = 0;
  uint32_t bookId = 0;
  int32_t spineIndex = 0;
  uint32_t visibleTextOffset = 0;
  PageFlipRole role = PageFlipRole::Left;
};

namespace PageFlipPacket {

inline constexpr uint16_t MAGIC = 0x4650;  // 'PF', little-endian on the wire
inline constexpr uint8_t PROTOCOL_VERSION = 1;
// magic(2) + version(1) + message(1) + flags(1) + compatHash(4) + bookId(4) + turnSeq(4)
// + spineIndex(4) + pageNumber(4)
inline constexpr size_t TURN_BYTES = 25;
// A hello is a turn's 25 bytes plus the join's content offset (section 4.2). The extra field is
// appended rather than inserted, so every shared offset stays where a turn's is and one set of
// constants still serves both.
inline constexpr size_t HELLO_BYTES = TURN_BYTES + 4;
// The sync messages: header(5) + the fields listed on each encoder below. The offer is variable
// length because the font name is length-prefixed -- a built-in family sends no name at all, and
// the worst case is still well inside PageFlipTransport::MAX_PAYLOAD_BYTES.
inline constexpr size_t SYNC_OFFER_MIN_BYTES = 32;
inline constexpr size_t SYNC_OFFER_MAX_BYTES = SYNC_OFFER_MIN_BYTES + PageFlipRenderSettings::FONT_NAME_MAX_LENGTH;
inline constexpr size_t SYNC_ANSWER_BYTES = 18;
inline constexpr size_t SYNC_APPLY_BYTES = 13;
// header(5) + compatHash(4) + bookId(4) + spineIndex(4) + visibleTextOffset(4)
inline constexpr size_t JOIN_RESUME_BYTES = 21;

// Writes a Turn packet. Returns false without touching `output` if `capacity` is too small.
bool encodeTurn(const PageFlipTurn& turn, uint8_t* output, size_t capacity, size_t& outputLength);

// Reads a Turn packet, rejecting anything that is not one: wrong magic, unknown protocol version,
// a different message type, or a short buffer. Trailing bytes are tolerated so a future version
// can extend the packet without breaking older receivers. Returns false and leaves `turn`
// untouched on rejection.
bool decodeTurn(const uint8_t* data, size_t length, PageFlipTurn& turn);

// A turn's wire layout with the join's offset appended, so both share one set of field offsets.
bool encodeHello(const PageFlipHello& hello, uint8_t* output, size_t capacity, size_t& outputLength);
bool decodeHello(const uint8_t* data, size_t length, PageFlipHello& hello);

// Settings force-sync (section 5.1). Same rules as above: explicit little-endian, short buffers
// rejected, trailing bytes tolerated.
//
// The offer's font name rides length-prefixed rather than as a fixed 32-byte field, so the common
// case (a built-in family, no name) costs nothing. A name longer than the setting can hold is
// rejected outright rather than truncated -- a truncated family name would resolve to a different
// font, or to none, which is the exact failure the preflight exists to catch.
bool encodeSyncOffer(const PageFlipSyncOffer& offer, uint8_t* output, size_t capacity, size_t& outputLength);
bool decodeSyncOffer(const uint8_t* data, size_t length, PageFlipSyncOffer& offer);

bool encodeSyncAnswer(const PageFlipSyncAnswer& answer, uint8_t* output, size_t capacity, size_t& outputLength);
bool decodeSyncAnswer(const uint8_t* data, size_t length, PageFlipSyncAnswer& answer);

bool encodeSyncApply(const PageFlipSyncApply& apply, uint8_t* output, size_t capacity, size_t& outputLength);
bool decodeSyncApply(const uint8_t* data, size_t length, PageFlipSyncApply& apply);

// The divergent join's answer (section 4.3). Same rules again.
bool encodeJoinResume(const PageFlipJoinResume& resume, uint8_t* output, size_t capacity, size_t& outputLength);
bool decodeJoinResume(const uint8_t* data, size_t length, PageFlipJoinResume& resume);

// Peeks the message type without validating the rest, so a receive loop can dispatch before
// decoding. Returns false when the buffer is not a PageFlip packet at all.
bool peekMessage(const uint8_t* data, size_t length, PageFlipMessage& message);

}  // namespace PageFlipPacket
