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
// Hello only, appended past a turn's last field (section 4.2). Appended rather than inserted so
// encodeCommon still writes both messages from one set of offsets.
constexpr size_t OFF_HELLO_VISIBLE_OFFSET = 25;

// Sync offer field offsets (section 5.1). The first two fields sit where a turn's do, so the
// header plus hash plus book id is one shape across every message.
constexpr size_t OFF_SYNC_COMPAT_HASH = 5;
constexpr size_t OFF_SYNC_BOOK_ID = 9;
constexpr size_t OFF_OFFER_FONT_ID = 13;
constexpr size_t OFF_OFFER_VIEWPORT_WIDTH = 17;
constexpr size_t OFF_OFFER_VIEWPORT_HEIGHT = 19;
constexpr size_t OFF_OFFER_FONT_FAMILY = 21;
constexpr size_t OFF_OFFER_FONT_POINT_SIZE = 22;
constexpr size_t OFF_OFFER_LINE_SPACING = 23;
constexpr size_t OFF_OFFER_PARAGRAPH_ALIGNMENT = 24;
constexpr size_t OFF_OFFER_SCREEN_MARGIN = 25;
constexpr size_t OFF_OFFER_IMAGE_RENDERING = 26;
constexpr size_t OFF_OFFER_EXTRA_PARAGRAPH_SPACING = 27;
constexpr size_t OFF_OFFER_HYPHENATION = 28;
constexpr size_t OFF_OFFER_EMBEDDED_STYLE = 29;
constexpr size_t OFF_OFFER_FOCUS_READING = 30;
constexpr size_t OFF_OFFER_FONT_NAME_LENGTH = 31;
constexpr size_t OFF_OFFER_FONT_NAME = 32;

// The answer reuses the hash/book-id pair for "which offer is this about", then adds its verdict.
constexpr size_t OFF_ANSWER_RESULT_HASH = 13;
constexpr size_t OFF_ANSWER_RESULT = 17;

// The divergent join's answer (section 4.3), on the same header shape.
constexpr size_t OFF_RESUME_SPINE_INDEX = 13;
constexpr size_t OFF_RESUME_VISIBLE_OFFSET = 17;

// Smallest prefix that identifies a packet as ours: magic + version + message.
constexpr size_t HEADER_BYTES = 4;

constexpr uint8_t FLAG_ROLE_RIGHT = 1 << 0;
constexpr uint8_t FLAG_BACKWARD = 1 << 1;
constexpr uint8_t FLAG_AT_BOOK_END = 1 << 2;
// Hello only: the join verdict (section 4.2), two bits wide, in the bits a turn does not use.
constexpr uint8_t JOIN_VERDICT_SHIFT = 3;
constexpr uint8_t JOIN_VERDICT_MASK = 0b11 << JOIN_VERDICT_SHIFT;
constexpr uint8_t FLAG_JOIN_RESTART = 1 << 5;

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

// Turns and hellos carry the same fields in the same places; only the message byte and the meaning
// of the second flag bit differ. One writer keeps the two from drifting apart.
bool encodeCommon(uint8_t* output, size_t capacity, size_t& outputLength, PageFlipMessage message, uint8_t flags,
                  uint32_t compatHash, uint32_t bookId, uint32_t turnSeq, int32_t spineIndex, int32_t pageNumber) {
  if (output == nullptr || capacity < PageFlipPacket::TURN_BYTES) return false;

  writeU16(output + OFF_MAGIC, PageFlipPacket::MAGIC);
  output[OFF_VERSION] = PageFlipPacket::PROTOCOL_VERSION;
  output[OFF_MESSAGE] = static_cast<uint8_t>(message);
  output[OFF_FLAGS] = flags;
  writeU32(output + OFF_COMPAT_HASH, compatHash);
  writeU32(output + OFF_BOOK_ID, bookId);
  writeU32(output + OFF_TURN_SEQ, turnSeq);
  writeI32(output + OFF_SPINE_INDEX, spineIndex);
  writeI32(output + OFF_PAGE_NUMBER, pageNumber);

  outputLength = PageFlipPacket::TURN_BYTES;
  return true;
}

// Every sync message opens the same way -- header, role, then the hash and book id that say which
// exchange it belongs to -- so one writer keeps the three from drifting apart the way encodeCommon
// does for turns and hellos. Callers have already checked capacity for their own full length.
void writeSyncHeader(uint8_t* output, PageFlipMessage message, PageFlipRole role, uint32_t hash, uint32_t bookId) {
  writeU16(output + OFF_MAGIC, PageFlipPacket::MAGIC);
  output[OFF_VERSION] = PageFlipPacket::PROTOCOL_VERSION;
  output[OFF_MESSAGE] = static_cast<uint8_t>(message);
  output[OFF_FLAGS] = (role == PageFlipRole::Right) ? FLAG_ROLE_RIGHT : 0;
  writeU32(output + OFF_SYNC_COMPAT_HASH, hash);
  writeU32(output + OFF_SYNC_BOOK_ID, bookId);
}

// A result code this build does not know is not a decode failure: rejecting the packet would leave
// the sender waiting forever for an answer it already sent. Anything unrecognised is "not Ok",
// which is the safe reading -- only an explicit Ok may commit.
// Same tolerance, same reason: the field is two bits and one of its four values is unassigned, so a
// later version can add one without this build rejecting the greeting that carries it. Unknown is
// the safe reading -- it is "retry", never "no", so an unrecognised verdict cannot classify a pair
// as divergent and put a resume prompt in front of the user.
PageFlipJoinVerdict toJoinVerdict(uint8_t raw) {
  switch (static_cast<PageFlipJoinVerdict>(raw)) {
    case PageFlipJoinVerdict::Adjacent:
      return PageFlipJoinVerdict::Adjacent;
    case PageFlipJoinVerdict::NotAdjacent:
      return PageFlipJoinVerdict::NotAdjacent;
    case PageFlipJoinVerdict::Unknown:
      break;
  }
  return PageFlipJoinVerdict::Unknown;
}

PageFlipSyncResult toSyncResult(uint8_t raw) {
  switch (static_cast<PageFlipSyncResult>(raw)) {
    case PageFlipSyncResult::Ok:
      return PageFlipSyncResult::Ok;
    case PageFlipSyncResult::MissingFont:
      return PageFlipSyncResult::MissingFont;
    case PageFlipSyncResult::FontDiffers:
      return PageFlipSyncResult::FontDiffers;
    case PageFlipSyncResult::ScreenDiffers:
      return PageFlipSyncResult::ScreenDiffers;
    case PageFlipSyncResult::Unknown:
      break;
  }
  return PageFlipSyncResult::Unknown;
}

}  // namespace

namespace PageFlipPacket {

bool encodeTurn(const PageFlipTurn& turn, uint8_t* output, size_t capacity, size_t& outputLength) {
  uint8_t flags = 0;
  if (turn.role == PageFlipRole::Right) flags |= FLAG_ROLE_RIGHT;
  if (!turn.forward) flags |= FLAG_BACKWARD;
  if (turn.atBookEnd) flags |= FLAG_AT_BOOK_END;
  return encodeCommon(output, capacity, outputLength, PageFlipMessage::Turn, flags, turn.compatHash, turn.bookId,
                      turn.turnSeq, turn.spineIndex, turn.pageNumber);
}

bool encodeHello(const PageFlipHello& hello, uint8_t* output, size_t capacity, size_t& outputLength) {
  // Checked here as well as in encodeCommon: a hello is longer than the turn that writer sizes for,
  // and a buffer big enough for one is not big enough for the other.
  if (output == nullptr || capacity < HELLO_BYTES) return false;

  uint8_t flags = 0;
  if (hello.role == PageFlipRole::Right) flags |= FLAG_ROLE_RIGHT;
  // The direction bit reads as "this is an answer" on a hello: a greeting sets wantsReply, so the
  // bit is clear, and the answer sets the bit and is never answered in turn.
  if (!hello.wantsReply) flags |= FLAG_BACKWARD;
  if (hello.startsJoin) flags |= FLAG_JOIN_RESTART;
  flags |= static_cast<uint8_t>(static_cast<uint8_t>(hello.joinVerdict) << JOIN_VERDICT_SHIFT) & JOIN_VERDICT_MASK;

  if (!encodeCommon(output, capacity, outputLength, PageFlipMessage::Hello, flags, hello.compatHash, hello.bookId,
                    hello.turnSeq, hello.spineIndex, hello.pageNumber)) {
    return false;
  }
  writeU32(output + OFF_HELLO_VISIBLE_OFFSET, hello.visibleTextOffset);
  outputLength = HELLO_BYTES;
  return true;
}

bool decodeHello(const uint8_t* data, size_t length, PageFlipHello& hello) {
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::Hello)) return false;
  if (length < HELLO_BYTES) return false;

  const uint8_t flags = data[OFF_FLAGS];
  hello.role = (flags & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
  hello.wantsReply = (flags & FLAG_BACKWARD) == 0;
  hello.startsJoin = (flags & FLAG_JOIN_RESTART) != 0;
  hello.joinVerdict = toJoinVerdict(static_cast<uint8_t>((flags & JOIN_VERDICT_MASK) >> JOIN_VERDICT_SHIFT));
  hello.compatHash = readU32(data + OFF_COMPAT_HASH);
  hello.bookId = readU32(data + OFF_BOOK_ID);
  hello.turnSeq = readU32(data + OFF_TURN_SEQ);
  hello.spineIndex = readI32(data + OFF_SPINE_INDEX);
  hello.pageNumber = readI32(data + OFF_PAGE_NUMBER);
  hello.visibleTextOffset = readU32(data + OFF_HELLO_VISIBLE_OFFSET);
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

bool encodeSyncOffer(const PageFlipSyncOffer& offer, uint8_t* output, size_t capacity, size_t& outputLength) {
  // strnlen rather than strlen: the name arrives from a fixed-size settings field, which is not
  // guaranteed to be terminated if it was ever filled by something other than this codebase.
  const size_t nameLength = strnlen(offer.settings.sdFontFamilyName, PageFlipRenderSettings::FONT_NAME_CAPACITY);
  if (nameLength > PageFlipRenderSettings::FONT_NAME_MAX_LENGTH) return false;

  const size_t total = SYNC_OFFER_MIN_BYTES + nameLength;
  if (output == nullptr || capacity < total) return false;

  writeSyncHeader(output, PageFlipMessage::SyncOffer, offer.role, offer.compatHash, offer.bookId);
  writeI32(output + OFF_OFFER_FONT_ID, offer.settings.fontId);
  writeU16(output + OFF_OFFER_VIEWPORT_WIDTH, offer.settings.viewportWidth);
  writeU16(output + OFF_OFFER_VIEWPORT_HEIGHT, offer.settings.viewportHeight);
  output[OFF_OFFER_FONT_FAMILY] = offer.settings.fontFamily;
  output[OFF_OFFER_FONT_POINT_SIZE] = offer.settings.fontPointSize;
  output[OFF_OFFER_LINE_SPACING] = offer.settings.lineSpacing;
  output[OFF_OFFER_PARAGRAPH_ALIGNMENT] = offer.settings.paragraphAlignment;
  output[OFF_OFFER_SCREEN_MARGIN] = offer.settings.screenMargin;
  output[OFF_OFFER_IMAGE_RENDERING] = offer.settings.imageRendering;
  output[OFF_OFFER_EXTRA_PARAGRAPH_SPACING] = offer.settings.extraParagraphSpacing;
  output[OFF_OFFER_HYPHENATION] = offer.settings.hyphenationEnabled;
  output[OFF_OFFER_EMBEDDED_STYLE] = offer.settings.embeddedStyle;
  output[OFF_OFFER_FOCUS_READING] = offer.settings.focusReadingEnabled;
  output[OFF_OFFER_FONT_NAME_LENGTH] = static_cast<uint8_t>(nameLength);
  std::memcpy(output + OFF_OFFER_FONT_NAME, offer.settings.sdFontFamilyName, nameLength);

  outputLength = total;
  return true;
}

bool decodeSyncOffer(const uint8_t* data, size_t length, PageFlipSyncOffer& offer) {
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::SyncOffer)) return false;
  if (length < SYNC_OFFER_MIN_BYTES) return false;

  const uint8_t nameLength = data[OFF_OFFER_FONT_NAME_LENGTH];
  // A name too long for the receiving setting is rejected, never truncated: a truncated family name
  // resolves to a different font or to none at all, which is precisely the silent divergence the
  // preflight exists to catch.
  if (nameLength > PageFlipRenderSettings::FONT_NAME_MAX_LENGTH) return false;
  if (length < SYNC_OFFER_MIN_BYTES + nameLength) return false;

  offer.role = (data[OFF_FLAGS] & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
  offer.compatHash = readU32(data + OFF_SYNC_COMPAT_HASH);
  offer.bookId = readU32(data + OFF_SYNC_BOOK_ID);
  offer.settings.fontId = readI32(data + OFF_OFFER_FONT_ID);
  offer.settings.viewportWidth = readU16(data + OFF_OFFER_VIEWPORT_WIDTH);
  offer.settings.viewportHeight = readU16(data + OFF_OFFER_VIEWPORT_HEIGHT);
  offer.settings.fontFamily = data[OFF_OFFER_FONT_FAMILY];
  offer.settings.fontPointSize = data[OFF_OFFER_FONT_POINT_SIZE];
  offer.settings.lineSpacing = data[OFF_OFFER_LINE_SPACING];
  offer.settings.paragraphAlignment = data[OFF_OFFER_PARAGRAPH_ALIGNMENT];
  offer.settings.screenMargin = data[OFF_OFFER_SCREEN_MARGIN];
  offer.settings.imageRendering = data[OFF_OFFER_IMAGE_RENDERING];
  offer.settings.extraParagraphSpacing = data[OFF_OFFER_EXTRA_PARAGRAPH_SPACING];
  offer.settings.hyphenationEnabled = data[OFF_OFFER_HYPHENATION];
  offer.settings.embeddedStyle = data[OFF_OFFER_EMBEDDED_STYLE];
  offer.settings.focusReadingEnabled = data[OFF_OFFER_FOCUS_READING];

  std::memcpy(offer.settings.sdFontFamilyName, data + OFF_OFFER_FONT_NAME, nameLength);
  // The wire carries no terminator, and the field is handed to C string APIs on the other side.
  offer.settings.sdFontFamilyName[nameLength] = '\0';
  return true;
}

bool encodeSyncAnswer(const PageFlipSyncAnswer& answer, uint8_t* output, size_t capacity, size_t& outputLength) {
  if (output == nullptr || capacity < SYNC_ANSWER_BYTES) return false;

  writeSyncHeader(output, PageFlipMessage::SyncAnswer, answer.role, answer.targetHash, answer.bookId);
  writeU32(output + OFF_ANSWER_RESULT_HASH, answer.resultHash);
  output[OFF_ANSWER_RESULT] = static_cast<uint8_t>(answer.result);

  outputLength = SYNC_ANSWER_BYTES;
  return true;
}

bool decodeSyncAnswer(const uint8_t* data, size_t length, PageFlipSyncAnswer& answer) {
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::SyncAnswer)) return false;
  if (length < SYNC_ANSWER_BYTES) return false;

  answer.role = (data[OFF_FLAGS] & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
  answer.targetHash = readU32(data + OFF_SYNC_COMPAT_HASH);
  answer.bookId = readU32(data + OFF_SYNC_BOOK_ID);
  answer.resultHash = readU32(data + OFF_ANSWER_RESULT_HASH);
  answer.result = toSyncResult(data[OFF_ANSWER_RESULT]);
  return true;
}

bool encodeSyncApply(const PageFlipSyncApply& apply, uint8_t* output, size_t capacity, size_t& outputLength) {
  if (output == nullptr || capacity < SYNC_APPLY_BYTES) return false;

  writeSyncHeader(output, PageFlipMessage::SyncApply, apply.role, apply.targetHash, apply.bookId);

  outputLength = SYNC_APPLY_BYTES;
  return true;
}

bool decodeSyncApply(const uint8_t* data, size_t length, PageFlipSyncApply& apply) {
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::SyncApply)) return false;
  if (length < SYNC_APPLY_BYTES) return false;

  apply.role = (data[OFF_FLAGS] & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
  apply.targetHash = readU32(data + OFF_SYNC_COMPAT_HASH);
  apply.bookId = readU32(data + OFF_SYNC_BOOK_ID);
  return true;
}

bool encodeJoinResume(const PageFlipJoinResume& resume, uint8_t* output, size_t capacity, size_t& outputLength) {
  if (output == nullptr || capacity < JOIN_RESUME_BYTES) return false;

  writeSyncHeader(output, PageFlipMessage::JoinResume, resume.role, resume.compatHash, resume.bookId);
  writeI32(output + OFF_RESUME_SPINE_INDEX, resume.spineIndex);
  writeU32(output + OFF_RESUME_VISIBLE_OFFSET, resume.visibleTextOffset);

  outputLength = JOIN_RESUME_BYTES;
  return true;
}

bool decodeJoinResume(const uint8_t* data, size_t length, PageFlipJoinResume& resume) {
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::JoinResume)) return false;
  if (length < JOIN_RESUME_BYTES) return false;

  resume.role = (data[OFF_FLAGS] & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
  resume.compatHash = readU32(data + OFF_SYNC_COMPAT_HASH);
  resume.bookId = readU32(data + OFF_SYNC_BOOK_ID);
  resume.spineIndex = readI32(data + OFF_RESUME_SPINE_INDEX);
  resume.visibleTextOffset = readU32(data + OFF_RESUME_VISIBLE_OFFSET);
  return true;
}

bool encodePairBeacon(const PageFlipRole role, uint8_t* output, size_t capacity, size_t& outputLength) {
  if (output == nullptr || capacity < PAIR_BEACON_BYTES) return false;

  writeU16(output + OFF_MAGIC, PageFlipPacket::MAGIC);
  output[OFF_VERSION] = PageFlipPacket::PROTOCOL_VERSION;
  output[OFF_MESSAGE] = static_cast<uint8_t>(PageFlipMessage::PairBeacon);
  output[OFF_FLAGS] = (role == PageFlipRole::Right) ? FLAG_ROLE_RIGHT : 0;

  outputLength = PAIR_BEACON_BYTES;
  return true;
}

bool decodePairBeacon(const uint8_t* data, size_t length, PageFlipRole& role) {
  if (!hasPageFlipHeader(data, length)) return false;
  if (data[OFF_MESSAGE] != static_cast<uint8_t>(PageFlipMessage::PairBeacon)) return false;
  if (length < PAIR_BEACON_BYTES) return false;

  role = (data[OFF_FLAGS] & FLAG_ROLE_RIGHT) ? PageFlipRole::Right : PageFlipRole::Left;
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
    case PageFlipMessage::SyncOffer:
      message = PageFlipMessage::SyncOffer;
      return true;
    case PageFlipMessage::SyncAnswer:
      message = PageFlipMessage::SyncAnswer;
      return true;
    case PageFlipMessage::SyncApply:
      message = PageFlipMessage::SyncApply;
      return true;
    case PageFlipMessage::JoinResume:
      message = PageFlipMessage::JoinResume;
      return true;
    case PageFlipMessage::PairBeacon:
      message = PageFlipMessage::PairBeacon;
      return true;
  }
  return false;  // a message type this build does not know
}

}  // namespace PageFlipPacket
