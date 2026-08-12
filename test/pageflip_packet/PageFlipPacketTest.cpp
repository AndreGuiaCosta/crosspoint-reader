#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "PageFlip/PageFlipPacket.h"

namespace {

PageFlipTurn sampleTurn() {
  PageFlipTurn turn;
  turn.compatHash = 0xDEADBEEFu;
  turn.bookId = 0x01020304u;
  turn.turnSeq = 847u;
  turn.spineIndex = 7;
  turn.pageNumber = 3;
  turn.role = PageFlipRole::Right;
  turn.forward = true;
  turn.atBookEnd = false;
  return turn;
}

std::vector<uint8_t> encoded(const PageFlipTurn& turn) {
  std::vector<uint8_t> buffer(PageFlipPacket::TURN_BYTES);
  size_t length = 0;
  EXPECT_TRUE(PageFlipPacket::encodeTurn(turn, buffer.data(), buffer.size(), length));
  EXPECT_EQ(length, PageFlipPacket::TURN_BYTES);
  return buffer;
}

TEST(PageFlipPacket, TurnRoundTripsEveryField) {
  const PageFlipTurn sent = sampleTurn();
  const std::vector<uint8_t> wire = encoded(sent);

  PageFlipTurn received;
  ASSERT_TRUE(PageFlipPacket::decodeTurn(wire.data(), wire.size(), received));
  EXPECT_EQ(received.compatHash, sent.compatHash);
  EXPECT_EQ(received.bookId, sent.bookId);
  EXPECT_EQ(received.turnSeq, sent.turnSeq);
  EXPECT_EQ(received.spineIndex, sent.spineIndex);
  EXPECT_EQ(received.pageNumber, sent.pageNumber);
  EXPECT_EQ(received.role, sent.role);
  EXPECT_EQ(received.forward, sent.forward);
  EXPECT_EQ(received.atBookEnd, sent.atBookEnd);
}

TEST(PageFlipPacket, FlagsRoundTripInEveryCombination) {
  for (const PageFlipRole role : {PageFlipRole::Left, PageFlipRole::Right}) {
    for (const bool forward : {false, true}) {
      for (const bool atBookEnd : {false, true}) {
        PageFlipTurn sent = sampleTurn();
        sent.role = role;
        sent.forward = forward;
        sent.atBookEnd = atBookEnd;
        const std::vector<uint8_t> wire = encoded(sent);

        PageFlipTurn received;
        ASSERT_TRUE(PageFlipPacket::decodeTurn(wire.data(), wire.size(), received));
        EXPECT_EQ(received.role, role);
        EXPECT_EQ(received.forward, forward);
        EXPECT_EQ(received.atBookEnd, atBookEnd);
      }
    }
  }
}

// Backward page turns near the top of a book, and the sentinel-ish extremes, must survive the
// two's-complement trip through the u32 wire fields.
TEST(PageFlipPacket, NegativeAndExtremePositionsSurvive) {
  const int32_t values[] = {-1, -2147483647 - 1, 0, 2147483647};
  for (const int32_t spine : values) {
    for (const int32_t page : values) {
      PageFlipTurn sent = sampleTurn();
      sent.spineIndex = spine;
      sent.pageNumber = page;
      const std::vector<uint8_t> wire = encoded(sent);

      PageFlipTurn received;
      ASSERT_TRUE(PageFlipPacket::decodeTurn(wire.data(), wire.size(), received));
      EXPECT_EQ(received.spineIndex, spine);
      EXPECT_EQ(received.pageNumber, page);
    }
  }
}

// The wire layout is a compatibility contract with the peer device: pin the bytes so a field
// reorder or an endianness slip fails here rather than as a desync between two X4s.
TEST(PageFlipPacket, WireLayoutIsPinned) {
  PageFlipTurn sent;
  sent.compatHash = 0x11223344u;
  sent.bookId = 0x55667788u;
  sent.turnSeq = 0x99AABBCCu;
  sent.spineIndex = 1;
  sent.pageNumber = -1;
  sent.role = PageFlipRole::Right;
  sent.forward = false;
  sent.atBookEnd = true;

  const std::vector<uint8_t> wire = encoded(sent);
  const std::vector<uint8_t> expected = {
      0x50, 0x46,              // magic 'PF', little-endian
      0x01,                    // protocol version
      0x01,                    // message: Turn
      0x07,                    // flags: right | backward | atBookEnd
      0x44, 0x33, 0x22, 0x11,  // compatHash
      0x88, 0x77, 0x66, 0x55,  // bookId
      0xCC, 0xBB, 0xAA, 0x99,  // turnSeq
      0x01, 0x00, 0x00, 0x00,  // spineIndex 1
      0xFF, 0xFF, 0xFF, 0xFF,  // pageNumber -1
  };
  EXPECT_EQ(wire, expected);
}

TEST(PageFlipPacket, EncodeRejectsUndersizedBuffer) {
  std::vector<uint8_t> buffer(PageFlipPacket::TURN_BYTES - 1, 0xAA);
  size_t length = 0;
  EXPECT_FALSE(PageFlipPacket::encodeTurn(sampleTurn(), buffer.data(), buffer.size(), length));
  // Nothing partially written.
  EXPECT_EQ(buffer.front(), 0xAA);
  EXPECT_EQ(buffer.back(), 0xAA);
}

TEST(PageFlipPacket, DecodeRejectsTruncatedPacketAtEveryLength) {
  const std::vector<uint8_t> wire = encoded(sampleTurn());
  for (size_t length = 0; length < wire.size(); ++length) {
    PageFlipTurn received;
    EXPECT_FALSE(PageFlipPacket::decodeTurn(wire.data(), length, received)) << "accepted a " << length << "-byte packet";
  }
}

// ESP-NOW is a broadcast medium shared with anything else on the channel, so foreign traffic must
// be rejected rather than parsed into a position.
TEST(PageFlipPacket, DecodeRejectsForeignAndFutureTraffic) {
  PageFlipTurn received;

  std::vector<uint8_t> badMagic = encoded(sampleTurn());
  badMagic[0] = 0x00;
  EXPECT_FALSE(PageFlipPacket::decodeTurn(badMagic.data(), badMagic.size(), received));

  std::vector<uint8_t> badVersion = encoded(sampleTurn());
  badVersion[2] = PageFlipPacket::PROTOCOL_VERSION + 1;
  EXPECT_FALSE(PageFlipPacket::decodeTurn(badVersion.data(), badVersion.size(), received));

  std::vector<uint8_t> otherMessage = encoded(sampleTurn());
  otherMessage[3] = static_cast<uint8_t>(PageFlipMessage::Hello);
  EXPECT_FALSE(PageFlipPacket::decodeTurn(otherMessage.data(), otherMessage.size(), received));

  EXPECT_FALSE(PageFlipPacket::decodeTurn(nullptr, PageFlipPacket::TURN_BYTES, received));
}

// A later protocol version may append fields; today's receiver must still read the prefix it knows.
TEST(PageFlipPacket, DecodeToleratesTrailingBytes) {
  std::vector<uint8_t> wire = encoded(sampleTurn());
  wire.push_back(0xFF);
  wire.push_back(0xFF);

  PageFlipTurn received;
  ASSERT_TRUE(PageFlipPacket::decodeTurn(wire.data(), wire.size(), received));
  EXPECT_EQ(received.turnSeq, sampleTurn().turnSeq);
}

TEST(PageFlipPacket, PeekMessageDispatchesBeforeDecoding) {
  const std::vector<uint8_t> wire = encoded(sampleTurn());
  PageFlipMessage message = PageFlipMessage::Hello;
  ASSERT_TRUE(PageFlipPacket::peekMessage(wire.data(), wire.size(), message));
  EXPECT_EQ(message, PageFlipMessage::Turn);

  // A header alone is enough to dispatch on.
  EXPECT_TRUE(PageFlipPacket::peekMessage(wire.data(), 4, message));
  EXPECT_FALSE(PageFlipPacket::peekMessage(wire.data(), 3, message));

  std::vector<uint8_t> unknown = wire;
  unknown[3] = 0x7F;
  EXPECT_FALSE(PageFlipPacket::peekMessage(unknown.data(), unknown.size(), message));
}

}  // namespace
