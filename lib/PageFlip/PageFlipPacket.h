#pragma once

#include <cstddef>
#include <cstdint>

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

// Discriminates the packets that share the transport. Only Turn is implemented so far; Hello
// belongs to the join negotiation (section 4.2) and is reserved here so adding it later does not
// break the wire.
enum class PageFlipMessage : uint8_t {
  Turn = 1,
  Hello = 2,
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

// A presence greeting. Same 25 bytes as a turn -- only `message` and the meaning of one flag
// differ -- because a device has to answer the same questions either way: who am I, what am I
// reading, and how far along is the pair's turn counter.
//
// Presence is not optional politeness: "the link came up" is not "a peer is there", and a device
// that advanced by two without a peer would turn two pages per press on its own.
struct PageFlipHello {
  uint32_t compatHash = 0;
  uint32_t bookId = 0;
  uint32_t turnSeq = 0;   // the join adopts max(local, peer) from this
  int32_t spineIndex = 0; // where this device currently is
  int32_t pageNumber = 0;
  PageFlipRole role = PageFlipRole::Left;
  // A greeting asks for an answer; an answer does not, which is what stops two devices greeting
  // each other forever. Rides the same flag bit a turn uses for direction.
  bool wantsReply = true;
};

namespace PageFlipPacket {

inline constexpr uint16_t MAGIC = 0x4650;  // 'PF', little-endian on the wire
inline constexpr uint8_t PROTOCOL_VERSION = 1;
// magic(2) + version(1) + message(1) + flags(1) + compatHash(4) + bookId(4) + turnSeq(4)
// + spineIndex(4) + pageNumber(4)
inline constexpr size_t TURN_BYTES = 25;

// Writes a Turn packet. Returns false without touching `output` if `capacity` is too small.
bool encodeTurn(const PageFlipTurn& turn, uint8_t* output, size_t capacity, size_t& outputLength);

// Reads a Turn packet, rejecting anything that is not one: wrong magic, unknown protocol version,
// a different message type, or a short buffer. Trailing bytes are tolerated so a future version
// can extend the packet without breaking older receivers. Returns false and leaves `turn`
// untouched on rejection.
bool decodeTurn(const uint8_t* data, size_t length, PageFlipTurn& turn);

// Same wire layout as a turn, so both share one set of field offsets.
bool encodeHello(const PageFlipHello& hello, uint8_t* output, size_t capacity, size_t& outputLength);
bool decodeHello(const uint8_t* data, size_t length, PageFlipHello& hello);

// Peeks the message type without validating the rest, so a receive loop can dispatch before
// decoding. Returns false when the buffer is not a PageFlip packet at all.
bool peekMessage(const uint8_t* data, size_t length, PageFlipMessage& message);

}  // namespace PageFlipPacket
