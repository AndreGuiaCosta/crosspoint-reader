#include "PageFlipPacket.h"

#include <cstring>

namespace {

// Field offsets, shared by the encoder and decoder so they cannot drift apart.
constexpr size_t OFF_MAGIC = 0;
constexpr size_t OFF_VERSION = 2;
constexpr size_t OFF_MESSAGE = 3;
constexpr size_t OFF_FLAGS = 4;
constexpr size_t OFF_COMPAT_HASH = 5;
constexpr size_t OFF_BOOK_ID = 9;
constexpr size_t OFF_TURN_SEQ = 13;
constexpr size_t OFF_SPINE_INDEX = 17;
constexpr size_t OFF_PAGE_NUMBER = 21;

// Smallest prefix that identifies a packet as ours: magic + version + message.
constexpr size_t HEADER_BYTES = 4;

constexpr uint8_t FLAG_ROLE_RIGHT = 1 << 0;
constexpr uint8_t FLAG_BACKWARD = 1 << 1;
constexpr uint8_t FLAG_AT_BOOK_END = 1 << 2;

// Explicit little-endian, byte at a time: the wire layout must not inherit the host's endianness,
// and a memcpy of a wider type would.
void writeU16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFF);
  data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void writeU32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFF);
  data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

// Signed fields ride the wire as two's-complement u32. Converting back through a cast is
// implementation-defined before C++20 and still worth spelling out, so the round trip is explicit.
void writeI32(uint8_t* data, int32_t value) { writeU32(data, static_cast<uint32_t>(value)); }

int32_t readI32(const uint8_t* data) {
  const uint32_t raw = readU32(data);
  int32_t value;
  static_assert(sizeof(value) == sizeof(raw), "int32_t must be 4 bytes");
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

bool hasPageFlipHeader(const uint8_t* data, size_t length) {
  if (data == nullptr || length < HEADER_BYTES) return false;
  if (readU16(data + OFF_MAGIC) != PageFlipPacket::MAGIC) return false;
  return data[OFF_VERSION] == PageFlipPacket::PROTOCOL_VERSION;
}

}  // namespace

namespace PageFlipPacket {

bool encodeTurn(const PageFlipTurn& turn, uint8_t* output, size_t capacity, size_t& outputLength) {
  if (output == nullptr || capacity < TURN_BYTES) return false;

  uint8_t flags = 0;
  if (turn.role == PageFlipRole::Right) flags |= FLAG_ROLE_RIGHT;
  if (!turn.forward) flags |= FLAG_BACKWARD;
  if (turn.atBookEnd) flags |= FLAG_AT_BOOK_END;

  writeU16(output + OFF_MAGIC, MAGIC);
  output[OFF_VERSION] = PROTOCOL_VERSION;
  output[OFF_MESSAGE] = static_cast<uint8_t>(PageFlipMessage::Turn);
  output[OFF_FLAGS] = flags;
  writeU32(output + OFF_COMPAT_HASH, turn.compatHash);
  writeU32(output + OFF_BOOK_ID, turn.bookId);
  writeU32(output + OFF_TURN_SEQ, turn.turnSeq);
  writeI32(output + OFF_SPINE_INDEX, turn.spineIndex);
  writeI32(output + OFF_PAGE_NUMBER, turn.pageNumber);

  outputLength = TURN_BYTES;
  return true;
}

bool decodeTurn(const uint8_t* data, size_t length, PageFlipTurn& turn) {
  // Order matters, and it is the pattern every future decoder must follow: hasPageFlipHeader()
  // only guarantees HEADER_BYTES, so nothing past that may be read until the length check below.
  // OFF_MESSAGE sits inside the header, which is why it can be read here.
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::Turn)) return false;
  // Trailing bytes are fine (a later version may append fields); a short packet is not.
  if (length < TURN_BYTES) return false;

  const uint8_t flags = data[OFF_FLAGS];
  turn.role = (flags & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
  turn.forward = (flags & FLAG_BACKWARD) == 0;
  turn.atBookEnd = (flags & FLAG_AT_BOOK_END) != 0;
  turn.compatHash = readU32(data + OFF_COMPAT_HASH);
  turn.bookId = readU32(data + OFF_BOOK_ID);
  turn.turnSeq = readU32(data + OFF_TURN_SEQ);
  turn.spineIndex = readI32(data + OFF_SPINE_INDEX);
  turn.pageNumber = readI32(data + OFF_PAGE_NUMBER);
  return true;
}

bool peekMessage(const uint8_t* data, size_t length, PageFlipMessage& message) {
  if (!hasPageFlipHeader(data, length)) return false;
  switch (static_cast<PageFlipMessage>(data[OFF_MESSAGE])) {
    case PageFlipMessage::Turn:
      message = PageFlipMessage::Turn;
      return true;
    case PageFlipMessage::Hello:
      message = PageFlipMessage::Hello;
      return true;
  }
  return false;  // a message type this build does not know
}

}  // namespace PageFlipPacket
