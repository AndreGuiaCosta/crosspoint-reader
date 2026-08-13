#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PageFlip/PageFlipMac.h"
#include "PageFlip/PageFlipPacket.h"
#include "PageFlip/PageFlipTransport.h"

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
    EXPECT_FALSE(PageFlipPacket::decodeTurn(wire.data(), length, received))
        << "accepted a " << length << "-byte packet";
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

// --- the greeting, which also carries the join negotiation (docs/pageflip.md section 4.2) ---

PageFlipHello sampleHello() {
  PageFlipHello hello;
  hello.compatHash = 0x0BADF00Du;
  hello.bookId = 0x01020304u;
  hello.turnSeq = 847u;
  hello.spineIndex = 7;
  hello.pageNumber = 3;
  hello.visibleTextOffset = 123456u;
  hello.role = PageFlipRole::Left;
  hello.wantsReply = true;
  hello.startsJoin = false;
  hello.joinVerdict = PageFlipJoinVerdict::NotAdjacent;
  return hello;
}

std::vector<uint8_t> encoded(const PageFlipHello& hello) {
  std::vector<uint8_t> buffer(PageFlipPacket::HELLO_BYTES);
  size_t length = 0;
  EXPECT_TRUE(PageFlipPacket::encodeHello(hello, buffer.data(), buffer.size(), length));
  EXPECT_EQ(length, PageFlipPacket::HELLO_BYTES);
  return buffer;
}

TEST(PageFlipPacket, HelloRoundTripsEveryField) {
  const PageFlipHello sent = sampleHello();
  const std::vector<uint8_t> wire = encoded(sent);

  PageFlipHello received;
  ASSERT_TRUE(PageFlipPacket::decodeHello(wire.data(), wire.size(), received));
  EXPECT_EQ(received.compatHash, sent.compatHash);
  EXPECT_EQ(received.bookId, sent.bookId);
  EXPECT_EQ(received.turnSeq, sent.turnSeq);
  EXPECT_EQ(received.spineIndex, sent.spineIndex);
  EXPECT_EQ(received.pageNumber, sent.pageNumber);
  EXPECT_EQ(received.visibleTextOffset, sent.visibleTextOffset);
  EXPECT_EQ(received.role, sent.role);
  EXPECT_EQ(received.wantsReply, sent.wantsReply);
  EXPECT_EQ(received.startsJoin, sent.startsJoin);
  EXPECT_EQ(received.joinVerdict, sent.joinVerdict);
}

// Four independent things share the flags byte, and they must not bleed into each other -- a
// verdict misread as a role would seat both devices as left, and a reply misread as a restart
// would re-open a negotiation that had already concluded.
TEST(PageFlipPacket, HelloFlagsRoundTripInEveryCombination) {
  for (const PageFlipRole role : {PageFlipRole::Left, PageFlipRole::Right}) {
    for (const bool wantsReply : {false, true}) {
      for (const bool startsJoin : {false, true}) {
        for (const PageFlipJoinVerdict verdict :
             {PageFlipJoinVerdict::Unknown, PageFlipJoinVerdict::Adjacent, PageFlipJoinVerdict::NotAdjacent}) {
          PageFlipHello sent = sampleHello();
          sent.role = role;
          sent.wantsReply = wantsReply;
          sent.startsJoin = startsJoin;
          sent.joinVerdict = verdict;
          const std::vector<uint8_t> wire = encoded(sent);

          PageFlipHello received;
          ASSERT_TRUE(PageFlipPacket::decodeHello(wire.data(), wire.size(), received));
          EXPECT_EQ(received.role, role);
          EXPECT_EQ(received.wantsReply, wantsReply);
          EXPECT_EQ(received.startsJoin, startsJoin);
          EXPECT_EQ(received.joinVerdict, verdict);
        }
      }
    }
  }
}

// The offset is the join's whole anchor, and it is a u32 that really does reach its top on a large
// chapter -- a sign slip here would classify an ordinary pair as divergent.
TEST(PageFlipPacket, HelloOffsetSurvivesItsFullRange) {
  for (const uint32_t offset : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
    PageFlipHello sent = sampleHello();
    sent.visibleTextOffset = offset;
    const std::vector<uint8_t> wire = encoded(sent);

    PageFlipHello received;
    ASSERT_TRUE(PageFlipPacket::decodeHello(wire.data(), wire.size(), received));
    EXPECT_EQ(received.visibleTextOffset, offset);
  }
}

TEST(PageFlipPacket, HelloWireLayoutIsPinned) {
  PageFlipHello sent;
  sent.compatHash = 0x11223344u;
  sent.bookId = 0x55667788u;
  sent.turnSeq = 0x99AABBCCu;
  sent.spineIndex = 1;
  sent.pageNumber = -1;
  sent.visibleTextOffset = 0x0000FEDCu;
  sent.role = PageFlipRole::Right;
  sent.wantsReply = false;
  sent.startsJoin = true;
  sent.joinVerdict = PageFlipJoinVerdict::NotAdjacent;

  const std::vector<uint8_t> wire = encoded(sent);
  const std::vector<uint8_t> expected = {
      0x50, 0x46,              // magic 'PF', little-endian
      0x01,                    // protocol version
      0x02,                    // message: Hello
      0x33,                    // flags: right | answer | verdict NotAdjacent<<3 | join restart
      0x44, 0x33, 0x22, 0x11,  // compatHash
      0x88, 0x77, 0x66, 0x55,  // bookId
      0xCC, 0xBB, 0xAA, 0x99,  // turnSeq
      0x01, 0x00, 0x00, 0x00,  // spineIndex 1
      0xFF, 0xFF, 0xFF, 0xFF,  // pageNumber -1
      0xDC, 0xFE, 0x00, 0x00,  // visibleTextOffset
  };
  EXPECT_EQ(wire, expected);
}

// A hello is longer than a turn, so a buffer sized for a turn is not one it may be written into --
// the position and the verdict would both be missing, and the offset field would read as garbage
// from whatever the caller's buffer held.
TEST(PageFlipPacket, HelloEncodeRejectsATurnSizedBuffer) {
  std::vector<uint8_t> buffer(PageFlipPacket::TURN_BYTES, 0xAA);
  size_t length = 0;
  EXPECT_FALSE(PageFlipPacket::encodeHello(sampleHello(), buffer.data(), buffer.size(), length));
  EXPECT_EQ(buffer.front(), 0xAA);
  EXPECT_EQ(buffer.back(), 0xAA);
}

TEST(PageFlipPacket, HelloDecodeRejectsTruncatedPacketAtEveryLength) {
  const std::vector<uint8_t> wire = encoded(sampleHello());
  for (size_t length = 0; length < wire.size(); ++length) {
    PageFlipHello received;
    EXPECT_FALSE(PageFlipPacket::decodeHello(wire.data(), length, received))
        << "accepted a " << length << "-byte hello";
  }
}

// A verdict this build has no name for is the fourth value of a two-bit field. Rejecting the
// greeting would cost the pair its turnSeq adoption and its presence; reading it as Unknown costs
// one retry.
TEST(PageFlipPacket, HelloTreatsAnUnknownVerdictAsUnanswered) {
  std::vector<uint8_t> wire = encoded(sampleHello());
  wire[4] |= 0b11 << 3;

  PageFlipHello received;
  ASSERT_TRUE(PageFlipPacket::decodeHello(wire.data(), wire.size(), received));
  EXPECT_EQ(received.joinVerdict, PageFlipJoinVerdict::Unknown);
  EXPECT_EQ(received.visibleTextOffset, sampleHello().visibleTextOffset);
}

TEST(PageFlipPacket, HelloDecodeToleratesTrailingBytes) {
  std::vector<uint8_t> wire = encoded(sampleHello());
  wire.push_back(0xFF);

  PageFlipHello received;
  ASSERT_TRUE(PageFlipPacket::decodeHello(wire.data(), wire.size(), received));
  EXPECT_EQ(received.visibleTextOffset, sampleHello().visibleTextOffset);
}

// --- the divergent join's answer (docs/pageflip.md section 4.3) ---

PageFlipJoinResume sampleResume() {
  PageFlipJoinResume resume;
  resume.compatHash = 0x11223344u;
  resume.bookId = 0x55667788u;
  resume.spineIndex = -3;
  resume.visibleTextOffset = 0x0000FEDCu;
  resume.role = PageFlipRole::Right;
  return resume;
}

TEST(PageFlipPacket, JoinResumeRoundTripsAndPinsItsLayout) {
  std::vector<uint8_t> wire(PageFlipPacket::JOIN_RESUME_BYTES);
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodeJoinResume(sampleResume(), wire.data(), wire.size(), length));
  EXPECT_EQ(length, PageFlipPacket::JOIN_RESUME_BYTES);

  const std::vector<uint8_t> expected = {
      0x50, 0x46,              // magic 'PF', little-endian
      0x01,                    // protocol version
      0x06,                    // message: JoinResume
      0x01,                    // flags: right
      0x44, 0x33, 0x22, 0x11,  // compatHash
      0x88, 0x77, 0x66, 0x55,  // bookId
      0xFD, 0xFF, 0xFF, 0xFF,  // spineIndex -3
      0xDC, 0xFE, 0x00, 0x00,  // visibleTextOffset
  };
  EXPECT_EQ(wire, expected);

  PageFlipJoinResume received;
  ASSERT_TRUE(PageFlipPacket::decodeJoinResume(wire.data(), wire.size(), received));
  EXPECT_EQ(received.compatHash, sampleResume().compatHash);
  EXPECT_EQ(received.bookId, sampleResume().bookId);
  EXPECT_EQ(received.spineIndex, sampleResume().spineIndex);
  EXPECT_EQ(received.visibleTextOffset, sampleResume().visibleTextOffset);
  EXPECT_EQ(received.role, sampleResume().role);
}

TEST(PageFlipPacket, JoinResumeRejectsTruncationAndForeignTraffic) {
  std::vector<uint8_t> wire(PageFlipPacket::JOIN_RESUME_BYTES);
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodeJoinResume(sampleResume(), wire.data(), wire.size(), length));

  PageFlipJoinResume received;
  for (size_t truncated = 0; truncated < wire.size(); ++truncated) {
    EXPECT_FALSE(PageFlipPacket::decodeJoinResume(wire.data(), truncated, received))
        << "accepted a " << truncated << "-byte resume";
  }

  std::vector<uint8_t> otherMessage = wire;
  otherMessage[3] = static_cast<uint8_t>(PageFlipMessage::SyncApply);
  EXPECT_FALSE(PageFlipPacket::decodeJoinResume(otherMessage.data(), otherMessage.size(), received));

  std::vector<uint8_t> undersized(PageFlipPacket::JOIN_RESUME_BYTES - 1, 0xAA);
  EXPECT_FALSE(PageFlipPacket::encodeJoinResume(sampleResume(), undersized.data(), undersized.size(), length));
  EXPECT_EQ(undersized.front(), 0xAA);
}

// --- settings force-sync (docs/pageflip.md section 5.1) ---

PageFlipSyncOffer sampleOffer() {
  PageFlipSyncOffer offer;
  offer.compatHash = 0xABCDEF01u;
  offer.bookId = 0x01020304u;
  offer.role = PageFlipRole::Left;
  offer.settings.fontId = 4242;
  offer.settings.viewportWidth = 760;
  offer.settings.viewportHeight = 430;
  offer.settings.fontFamily = 2;
  offer.settings.fontPointSize = 16;
  offer.settings.lineSpacing = 1;
  offer.settings.paragraphAlignment = 1;
  offer.settings.screenMargin = 20;
  offer.settings.imageRendering = 1;
  offer.settings.extraParagraphSpacing = 1;
  offer.settings.hyphenationEnabled = 1;
  offer.settings.embeddedStyle = 1;
  offer.settings.focusReadingEnabled = 0;
  std::snprintf(offer.settings.sdFontFamilyName, sizeof(offer.settings.sdFontFamilyName), "Bookerly");
  return offer;
}

std::vector<uint8_t> encodedOffer(const PageFlipSyncOffer& offer) {
  std::vector<uint8_t> buffer(PageFlipPacket::SYNC_OFFER_MAX_BYTES);
  size_t length = 0;
  EXPECT_TRUE(PageFlipPacket::encodeSyncOffer(offer, buffer.data(), buffer.size(), length));
  buffer.resize(length);
  return buffer;
}

TEST(PageFlipPacket, SyncOfferRoundTripsEverySetting) {
  const PageFlipSyncOffer sent = sampleOffer();
  const std::vector<uint8_t> wire = encodedOffer(sent);

  PageFlipSyncOffer received;
  ASSERT_TRUE(PageFlipPacket::decodeSyncOffer(wire.data(), wire.size(), received));
  EXPECT_EQ(received.compatHash, sent.compatHash);
  EXPECT_EQ(received.bookId, sent.bookId);
  EXPECT_EQ(received.role, sent.role);
  EXPECT_EQ(received.settings.fontId, sent.settings.fontId);
  EXPECT_EQ(received.settings.viewportWidth, sent.settings.viewportWidth);
  EXPECT_EQ(received.settings.viewportHeight, sent.settings.viewportHeight);
  EXPECT_EQ(received.settings.fontFamily, sent.settings.fontFamily);
  EXPECT_EQ(received.settings.fontPointSize, sent.settings.fontPointSize);
  EXPECT_EQ(received.settings.lineSpacing, sent.settings.lineSpacing);
  EXPECT_EQ(received.settings.paragraphAlignment, sent.settings.paragraphAlignment);
  EXPECT_EQ(received.settings.screenMargin, sent.settings.screenMargin);
  EXPECT_EQ(received.settings.imageRendering, sent.settings.imageRendering);
  EXPECT_EQ(received.settings.extraParagraphSpacing, sent.settings.extraParagraphSpacing);
  EXPECT_EQ(received.settings.hyphenationEnabled, sent.settings.hyphenationEnabled);
  EXPECT_EQ(received.settings.embeddedStyle, sent.settings.embeddedStyle);
  EXPECT_EQ(received.settings.focusReadingEnabled, sent.settings.focusReadingEnabled);
  EXPECT_STREQ(received.settings.sdFontFamilyName, sent.settings.sdFontFamilyName);
}

// A built-in family sends no name at all, and the longest name the setting can hold must still fit
// the wire -- those are the two ends of the length-prefixed field.
TEST(PageFlipPacket, SyncOfferCarriesEveryFontNameLength) {
  for (size_t nameLength = 0; nameLength <= PageFlipRenderSettings::FONT_NAME_MAX_LENGTH; ++nameLength) {
    PageFlipSyncOffer sent = sampleOffer();
    const std::string name(nameLength, 'x');
    std::snprintf(sent.settings.sdFontFamilyName, sizeof(sent.settings.sdFontFamilyName), "%s", name.c_str());

    const std::vector<uint8_t> wire = encodedOffer(sent);
    EXPECT_EQ(wire.size(), PageFlipPacket::SYNC_OFFER_MIN_BYTES + nameLength);
    EXPECT_LE(wire.size(), PageFlipTransport::MAX_PAYLOAD_BYTES) << "an offer must fit one datagram";

    PageFlipSyncOffer received;
    ASSERT_TRUE(PageFlipPacket::decodeSyncOffer(wire.data(), wire.size(), received));
    EXPECT_STREQ(received.settings.sdFontFamilyName, name.c_str());
  }
}

// A name longer than the receiving setting must be refused, not truncated: a truncated family name
// resolves to a different font or to none, which is the silent divergence the preflight exists to
// catch. The encoder cannot produce one, so the check has to hold against a hand-built packet.
TEST(PageFlipPacket, SyncOfferRejectsAnOverlongFontName) {
  std::vector<uint8_t> wire = encodedOffer(sampleOffer());
  wire[31] = PageFlipRenderSettings::FONT_NAME_MAX_LENGTH + 1;
  wire.resize(PageFlipPacket::SYNC_OFFER_MIN_BYTES + PageFlipRenderSettings::FONT_NAME_MAX_LENGTH + 1, 'x');

  PageFlipSyncOffer received;
  EXPECT_FALSE(PageFlipPacket::decodeSyncOffer(wire.data(), wire.size(), received));
}

// The name is length-prefixed on the wire and handed to C string APIs on the other side, so the
// decoder owns the terminator. A shorter name landing in a buffer that held a longer one must not
// leave the old tail visible.
TEST(PageFlipPacket, SyncOfferTerminatesTheFontNameItWrites) {
  PageFlipSyncOffer sent = sampleOffer();
  std::snprintf(sent.settings.sdFontFamilyName, sizeof(sent.settings.sdFontFamilyName), "Lit");
  const std::vector<uint8_t> wire = encodedOffer(sent);

  PageFlipSyncOffer received;
  std::memset(received.settings.sdFontFamilyName, 'Z', sizeof(received.settings.sdFontFamilyName) - 1);
  received.settings.sdFontFamilyName[sizeof(received.settings.sdFontFamilyName) - 1] = '\0';
  ASSERT_TRUE(PageFlipPacket::decodeSyncOffer(wire.data(), wire.size(), received));
  EXPECT_STREQ(received.settings.sdFontFamilyName, "Lit");
}

TEST(PageFlipPacket, SyncAnswerAndApplyRoundTrip) {
  PageFlipSyncAnswer answer;
  answer.targetHash = 0x11112222u;
  answer.bookId = 0x33334444u;
  answer.resultHash = 0x55556666u;
  answer.result = PageFlipSyncResult::MissingFont;
  answer.role = PageFlipRole::Right;

  std::vector<uint8_t> answerWire(PageFlipPacket::SYNC_ANSWER_BYTES);
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodeSyncAnswer(answer, answerWire.data(), answerWire.size(), length));
  EXPECT_EQ(length, PageFlipPacket::SYNC_ANSWER_BYTES);

  PageFlipSyncAnswer decodedAnswer;
  ASSERT_TRUE(PageFlipPacket::decodeSyncAnswer(answerWire.data(), answerWire.size(), decodedAnswer));
  EXPECT_EQ(decodedAnswer.targetHash, answer.targetHash);
  EXPECT_EQ(decodedAnswer.bookId, answer.bookId);
  EXPECT_EQ(decodedAnswer.resultHash, answer.resultHash);
  EXPECT_EQ(decodedAnswer.result, answer.result);
  EXPECT_EQ(decodedAnswer.role, answer.role);

  PageFlipSyncApply apply;
  apply.targetHash = 0x77778888u;
  apply.bookId = 0x9999AAAAu;
  apply.role = PageFlipRole::Right;

  std::vector<uint8_t> applyWire(PageFlipPacket::SYNC_APPLY_BYTES);
  ASSERT_TRUE(PageFlipPacket::encodeSyncApply(apply, applyWire.data(), applyWire.size(), length));
  EXPECT_EQ(length, PageFlipPacket::SYNC_APPLY_BYTES);

  PageFlipSyncApply decodedApply;
  ASSERT_TRUE(PageFlipPacket::decodeSyncApply(applyWire.data(), applyWire.size(), decodedApply));
  EXPECT_EQ(decodedApply.targetHash, apply.targetHash);
  EXPECT_EQ(decodedApply.bookId, apply.bookId);
  EXPECT_EQ(decodedApply.role, apply.role);
}

// Only an explicit Ok may commit, so a verdict from a future build has to read as "not Ok" rather
// than as a decode failure -- rejecting it outright would leave the sender waiting forever for an
// answer that did arrive.
TEST(PageFlipPacket, SyncAnswerReadsAnUnknownVerdictAsNotOk) {
  PageFlipSyncAnswer answer;
  answer.result = PageFlipSyncResult::Ok;
  std::vector<uint8_t> wire(PageFlipPacket::SYNC_ANSWER_BYTES);
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodeSyncAnswer(answer, wire.data(), wire.size(), length));
  wire[17] = 0x7F;  // a result code this build does not know

  PageFlipSyncAnswer received;
  ASSERT_TRUE(PageFlipPacket::decodeSyncAnswer(wire.data(), wire.size(), received));
  EXPECT_EQ(received.result, PageFlipSyncResult::Unknown);
}

TEST(PageFlipPacket, SyncMessagesRejectTruncationAndForeignTraffic) {
  const std::vector<uint8_t> wire = encodedOffer(sampleOffer());
  for (size_t length = 0; length < wire.size(); ++length) {
    PageFlipSyncOffer received;
    EXPECT_FALSE(PageFlipPacket::decodeSyncOffer(wire.data(), length, received))
        << "accepted a " << length << "-byte offer";
  }

  // Each decoder must reject the other two messages, or a commit could be parsed out of an offer.
  PageFlipSyncAnswer asAnswer;
  EXPECT_FALSE(PageFlipPacket::decodeSyncAnswer(wire.data(), wire.size(), asAnswer));
  PageFlipSyncApply asApply;
  EXPECT_FALSE(PageFlipPacket::decodeSyncApply(wire.data(), wire.size(), asApply));
  PageFlipTurn asTurn;
  EXPECT_FALSE(PageFlipPacket::decodeTurn(wire.data(), wire.size(), asTurn));
}

// Same contract as the turn: these bytes are what the other device parses, so pin them.
TEST(PageFlipPacket, SyncOfferWireLayoutIsPinned) {
  PageFlipSyncOffer offer;
  offer.compatHash = 0x11223344u;
  offer.bookId = 0x55667788u;
  offer.role = PageFlipRole::Right;
  offer.settings.fontId = -2;
  offer.settings.viewportWidth = 0x0102;
  offer.settings.viewportHeight = 0x0304;
  offer.settings.fontFamily = 1;
  offer.settings.fontPointSize = 16;
  offer.settings.lineSpacing = 2;
  offer.settings.paragraphAlignment = 3;
  offer.settings.screenMargin = 20;
  offer.settings.imageRendering = 1;
  offer.settings.extraParagraphSpacing = 1;
  offer.settings.hyphenationEnabled = 0;
  offer.settings.embeddedStyle = 1;
  offer.settings.focusReadingEnabled = 0;
  std::snprintf(offer.settings.sdFontFamilyName, sizeof(offer.settings.sdFontFamilyName), "Ab");

  const std::vector<uint8_t> wire = encodedOffer(offer);
  const std::vector<uint8_t> expected = {
      0x50, 0x46,              // magic 'PF', little-endian
      0x01,                    // protocol version
      0x03,                    // message: SyncOffer
      0x01,                    // flags: right
      0x44, 0x33, 0x22, 0x11,  // compatHash (the layout the peer must reach)
      0x88, 0x77, 0x66, 0x55,  // bookId
      0xFE, 0xFF, 0xFF, 0xFF,  // fontId -2
      0x02, 0x01,              // viewportWidth
      0x04, 0x03,              // viewportHeight
      0x01,                    // fontFamily
      0x10,                    // fontPointSize 16
      0x02,                    // lineSpacing
      0x03,                    // paragraphAlignment
      0x14,                    // screenMargin 20
      0x01,                    // imageRendering
      0x01,                    // extraParagraphSpacing
      0x00,                    // hyphenationEnabled
      0x01,                    // embeddedStyle
      0x00,                    // focusReadingEnabled
      0x02,                    // font name length
      'A',  'b',               // font name, no terminator on the wire
  };
  EXPECT_EQ(wire, expected);
}

TEST(PageFlipPacket, PeekMessageDispatchesBeforeDecoding) {
  const std::vector<uint8_t> wire = encoded(sampleTurn());
  PageFlipMessage message = PageFlipMessage::Hello;
  ASSERT_TRUE(PageFlipPacket::peekMessage(wire.data(), wire.size(), message));
  EXPECT_EQ(message, PageFlipMessage::Turn);

  const std::vector<uint8_t> offerWire = encodedOffer(sampleOffer());
  ASSERT_TRUE(PageFlipPacket::peekMessage(offerWire.data(), offerWire.size(), message));
  EXPECT_EQ(message, PageFlipMessage::SyncOffer);

  // A header alone is enough to dispatch on.
  EXPECT_TRUE(PageFlipPacket::peekMessage(wire.data(), 4, message));
  EXPECT_FALSE(PageFlipPacket::peekMessage(wire.data(), 3, message));

  std::vector<uint8_t> unknown = wire;
  unknown[3] = 0x7F;
  EXPECT_FALSE(PageFlipPacket::peekMessage(unknown.data(), unknown.size(), message));
}

// The pairing beacon (section 8.1): the header and the role bit, and nothing else.
TEST(PageFlipPacket, PairBeaconRoundTrips) {
  uint8_t wire[PageFlipPacket::PAIR_BEACON_BYTES] = {};
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodePairBeacon(PageFlipRole::Right, wire, sizeof(wire), length));
  EXPECT_EQ(length, PageFlipPacket::PAIR_BEACON_BYTES);

  PageFlipRole role = PageFlipRole::Left;
  ASSERT_TRUE(PageFlipPacket::decodePairBeacon(wire, length, role));
  EXPECT_EQ(role, PageFlipRole::Right);

  ASSERT_TRUE(PageFlipPacket::encodePairBeacon(PageFlipRole::Left, wire, sizeof(wire), length));
  ASSERT_TRUE(PageFlipPacket::decodePairBeacon(wire, length, role));
  EXPECT_EQ(role, PageFlipRole::Left);
}

TEST(PageFlipPacket, PairBeaconIsNotAnyOtherMessage) {
  uint8_t wire[PageFlipPacket::PAIR_BEACON_BYTES] = {};
  size_t length = 0;
  ASSERT_TRUE(PageFlipPacket::encodePairBeacon(PageFlipRole::Left, wire, sizeof(wire), length));

  PageFlipMessage message = PageFlipMessage::Turn;
  ASSERT_TRUE(PageFlipPacket::peekMessage(wire, length, message));
  EXPECT_EQ(message, PageFlipMessage::PairBeacon);

  // Decoding one as a reading message would be reading five bytes as twenty-five.
  PageFlipTurn turn;
  EXPECT_FALSE(PageFlipPacket::decodeTurn(wire, length, turn));
  PageFlipHello hello;
  EXPECT_FALSE(PageFlipPacket::decodeHello(wire, length, hello));

  // And a turn is not a beacon, whatever length it is offered at.
  const std::vector<uint8_t> turnWire = encoded(sampleTurn());
  PageFlipRole role = PageFlipRole::Left;
  EXPECT_FALSE(PageFlipPacket::decodePairBeacon(turnWire.data(), turnWire.size(), role));
}

TEST(PageFlipPacket, PairBeaconRejectsAShortBuffer) {
  uint8_t wire[PageFlipPacket::PAIR_BEACON_BYTES] = {};
  size_t length = 0;
  EXPECT_FALSE(PageFlipPacket::encodePairBeacon(PageFlipRole::Left, wire, sizeof(wire) - 1, length));
  ASSERT_TRUE(PageFlipPacket::encodePairBeacon(PageFlipRole::Left, wire, sizeof(wire), length));

  PageFlipRole role = PageFlipRole::Right;
  EXPECT_FALSE(PageFlipPacket::decodePairBeacon(wire, length - 1, role));
}

// The paired device's identity, moving between the radio and settings (section 8.1).
TEST(PageFlipMacText, FormatsCanonically) {
  const uint8_t mac[PageFlipTransport::MAC_BYTES] = {0x02, 0x50, 0x46, 0x00, 0x0A, 0xFF};
  char text[PageFlipMac::TEXT_LENGTH] = {};
  ASSERT_TRUE(PageFlipMac::format(mac, text, sizeof(text)));
  EXPECT_STREQ(text, "02:50:46:00:0A:FF");

  EXPECT_FALSE(PageFlipMac::format(mac, text, sizeof(text) - 1));
}

TEST(PageFlipMacText, RoundTripsThroughText) {
  const uint8_t mac[PageFlipTransport::MAC_BYTES] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11};
  char text[PageFlipMac::TEXT_LENGTH] = {};
  ASSERT_TRUE(PageFlipMac::format(mac, text, sizeof(text)));

  uint8_t parsed[PageFlipTransport::MAC_BYTES] = {};
  ASSERT_TRUE(PageFlipMac::parse(text, parsed));
  EXPECT_EQ(std::memcmp(mac, parsed, sizeof(mac)), 0);

  // Lower case is what a user copying one out of another tool will paste in.
  uint8_t lower[PageFlipTransport::MAC_BYTES] = {};
  ASSERT_TRUE(PageFlipMac::parse("de:ad:be:ef:00:11", lower));
  EXPECT_EQ(std::memcmp(mac, lower, sizeof(mac)), 0);
}

// Every rejection here is a string that must not become a MAC of zeroes: a device paired with
// nobody listens to nobody, which on screen looks exactly like a peer that is switched off.
TEST(PageFlipMacText, RejectsAnythingButTheCanonicalForm) {
  uint8_t parsed[PageFlipTransport::MAC_BYTES] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  EXPECT_FALSE(PageFlipMac::parse("", parsed));
  EXPECT_FALSE(PageFlipMac::parse(nullptr, parsed));
  EXPECT_FALSE(PageFlipMac::parse("02:50:46:00:0A", parsed));
  EXPECT_FALSE(PageFlipMac::parse("02:50:46:00:0A:FF:11", parsed));
  EXPECT_FALSE(PageFlipMac::parse("02-50-46-00-0A-FF", parsed));
  EXPECT_FALSE(PageFlipMac::parse("02:50:46:00:0A:GG", parsed));
  EXPECT_FALSE(PageFlipMac::parse("2:50:46:00:0A:FF", parsed));

  // And a rejected string leaves the output alone rather than half written.
  for (const uint8_t byte : parsed) EXPECT_EQ(byte, 0xAA);
}

}  // namespace
