#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "PageFlip/PageFlipPacket.h"
#include "PageFlip/PageFlipTransportFactory.h"
#include "PageFlip/PageFlipUdpTransport.h"

namespace {

// Away from the 47190 default so a simulator left running does not collide with the suite.
constexpr const char* TEST_BASE_PORT = "47390";
constexpr uint16_t TEST_BASE_PORT_VALUE = 47390;

// Brings a transport up on a given slot. begin() reads the environment, so the variables are set
// immediately before each call rather than once for the process.
bool startOnSlot(PageFlipUdpTransport& transport, const char* slot) {
  ::setenv("CROSSPOINT_PAGEFLIP_PORT", TEST_BASE_PORT, 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOTS", "2", 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOT", slot, 1);
  return transport.begin();
}

// Loopback delivery is effectively immediate, but a bounded retry keeps the suite from flaking on
// a loaded machine rather than trading a real failure for a sleep.
bool pollWithRetry(PageFlipUdpTransport& transport, uint8_t* buffer, size_t capacity, size_t& length,
                   uint8_t senderMac[PageFlipTransport::MAC_BYTES]) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (transport.poll(buffer, capacity, length, senderMac)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

class PageFlipUdpTransportTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ::unsetenv("CROSSPOINT_PAGEFLIP_PORT");
    ::unsetenv("CROSSPOINT_PAGEFLIP_SLOTS");
    ::unsetenv("CROSSPOINT_PAGEFLIP_SLOT");
  }
};

TEST_F(PageFlipUdpTransportTest, CarriesATurnBetweenTwoSlots) {
  PageFlipUdpTransport left;
  PageFlipUdpTransport right;
  ASSERT_TRUE(startOnSlot(left, "0")) << left.lastError();
  ASSERT_TRUE(startOnSlot(right, "1")) << right.lastError();

  PageFlipTurn sent;
  sent.compatHash = 0xABCD1234u;
  sent.bookId = 0x0BADF00Du;
  sent.turnSeq = 42;
  sent.spineIndex = 5;
  sent.pageNumber = 11;
  sent.role = PageFlipRole::Left;

  uint8_t wire[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t wireLength = 0;
  ASSERT_TRUE(PageFlipPacket::encodeTurn(sent, wire, sizeof(wire), wireLength));
  ASSERT_TRUE(left.broadcast(wire, wireLength));

  uint8_t received[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t receivedLength = 0;
  uint8_t senderMac[PageFlipTransport::MAC_BYTES] = {};
  ASSERT_TRUE(pollWithRetry(right, received, sizeof(received), receivedLength, senderMac));
  EXPECT_EQ(receivedLength, wireLength);

  PageFlipTurn decoded;
  ASSERT_TRUE(PageFlipPacket::decodeTurn(received, receivedLength, decoded));
  EXPECT_EQ(decoded.turnSeq, sent.turnSeq);
  EXPECT_EQ(decoded.spineIndex, sent.spineIndex);
  EXPECT_EQ(decoded.pageNumber, sent.pageNumber);
  EXPECT_EQ(decoded.compatHash, sent.compatHash);

  uint8_t expectedMac[PageFlipTransport::MAC_BYTES] = {};
  ASSERT_TRUE(left.localMac(expectedMac));
  EXPECT_EQ(std::memcmp(senderMac, expectedMac, sizeof(expectedMac)), 0);
}

// ESP-NOW does not loop a broadcast back to its sender. The shim must not either, or every device
// would apply its own turn twice.
TEST_F(PageFlipUdpTransportTest, SenderNeverReceivesItsOwnBroadcast) {
  PageFlipUdpTransport left;
  PageFlipUdpTransport right;
  ASSERT_TRUE(startOnSlot(left, "0")) << left.lastError();
  ASSERT_TRUE(startOnSlot(right, "1")) << right.lastError();

  const uint8_t payload[] = {'P', 'F', 1, 1, 0};
  ASSERT_TRUE(left.broadcast(payload, sizeof(payload)));

  uint8_t buffer[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t length = 0;
  uint8_t senderMac[PageFlipTransport::MAC_BYTES] = {};
  EXPECT_FALSE(left.poll(buffer, sizeof(buffer), length, senderMac));
  // ...and the peer did get it, so the negative above is not just a lost packet.
  EXPECT_TRUE(pollWithRetry(right, buffer, sizeof(buffer), length, senderMac));
}

TEST_F(PageFlipUdpTransportTest, PollIsNonBlockingWhenIdle) {
  PageFlipUdpTransport transport;
  ASSERT_TRUE(startOnSlot(transport, "0")) << transport.lastError();

  uint8_t buffer[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t length = 0;
  uint8_t senderMac[PageFlipTransport::MAC_BYTES] = {};
  EXPECT_FALSE(transport.poll(buffer, sizeof(buffer), length, senderMac));
}

// Two instances launched with the same slot is the likeliest test-script mistake, and it presents
// as a peer that never answers. The error has to name it.
TEST_F(PageFlipUdpTransportTest, DuplicateSlotFailsAndSaysSo) {
  PageFlipUdpTransport first;
  PageFlipUdpTransport second;
  ASSERT_TRUE(startOnSlot(first, "0")) << first.lastError();

  EXPECT_FALSE(startOnSlot(second, "0"));
  EXPECT_FALSE(second.isStarted());
  EXPECT_NE(std::strstr(second.lastError(), "slot 0"), nullptr) << "unhelpful error: " << second.lastError();
}

// A datagram too large for the caller's buffer must be dropped whole. Delivering its prefix would
// hand the protocol a truncated packet that could still pass the length check.
TEST_F(PageFlipUdpTransportTest, OversizedDatagramIsDroppedNotTruncated) {
  PageFlipUdpTransport right;
  ASSERT_TRUE(startOnSlot(right, "1")) << right.lastError();

  const int raw = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(raw, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(TEST_BASE_PORT_VALUE + 1);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const std::vector<uint8_t> oversized(PageFlipTransport::MAX_PAYLOAD_BYTES + 32, 0xAB);
  ASSERT_GE(::sendto(raw, oversized.data(), oversized.size(), 0, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)),
            0);
  ::close(raw);

  uint8_t buffer[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t length = 0;
  uint8_t senderMac[PageFlipTransport::MAC_BYTES] = {};
  for (int attempt = 0; attempt < 20; ++attempt) {
    EXPECT_FALSE(right.poll(buffer, sizeof(buffer), length, senderMac));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

TEST_F(PageFlipUdpTransportTest, BroadcastRejectsAnOversizedPayload) {
  PageFlipUdpTransport transport;
  ASSERT_TRUE(startOnSlot(transport, "0")) << transport.lastError();

  const std::vector<uint8_t> oversized(PageFlipTransport::MAX_PAYLOAD_BYTES + 1, 0xCD);
  EXPECT_FALSE(transport.broadcast(oversized.data(), oversized.size()));
  EXPECT_FALSE(transport.broadcast(nullptr, 4));
  EXPECT_FALSE(transport.broadcast(oversized.data(), 0));
}

// The synthetic MAC feeds the lower-MAC tiebreak, so it must be stable across runs and ordered by
// slot -- not derived from anything random.
TEST_F(PageFlipUdpTransportTest, LocalMacIsStableAndOrderedBySlot) {
  PageFlipUdpTransport left;
  PageFlipUdpTransport right;
  ASSERT_TRUE(startOnSlot(left, "0")) << left.lastError();
  ASSERT_TRUE(startOnSlot(right, "1")) << right.lastError();

  uint8_t leftMac[PageFlipTransport::MAC_BYTES] = {};
  uint8_t rightMac[PageFlipTransport::MAC_BYTES] = {};
  ASSERT_TRUE(left.localMac(leftMac));
  ASSERT_TRUE(right.localMac(rightMac));

  EXPECT_LT(std::memcmp(leftMac, rightMac, sizeof(leftMac)), 0);
  // Locally administered, so it can never collide with a real vendor MAC.
  EXPECT_EQ(leftMac[0] & 0x02, 0x02);

  left.end();
  uint8_t afterRestart[PageFlipTransport::MAC_BYTES] = {};
  ASSERT_TRUE(startOnSlot(left, "0")) << left.lastError();
  ASSERT_TRUE(left.localMac(afterRestart));
  EXPECT_EQ(std::memcmp(leftMac, afterRestart, sizeof(leftMac)), 0);
}

// The factory's host branch: anything above lib/PageFlip holds the interface and never names an
// implementation, so this is what the reader will actually be given off-device.
TEST_F(PageFlipUdpTransportTest, FactoryProducesAUsableHostTransport) {
  ::setenv("CROSSPOINT_PAGEFLIP_PORT", TEST_BASE_PORT, 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOTS", "2", 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOT", "0", 1);

  const std::unique_ptr<PageFlipTransport> transport = makePageFlipTransport();
  ASSERT_NE(transport, nullptr);
  ASSERT_TRUE(transport->begin());
  EXPECT_TRUE(transport->isStarted());

  uint8_t mac[PageFlipTransport::MAC_BYTES] = {};
  EXPECT_TRUE(transport->localMac(mac));
  transport->end();
  EXPECT_FALSE(transport->isStarted());
}

// An out-of-range slot in the environment falls back to the default rather than binding somewhere
// arbitrary, so a typo in a script fails visibly at the pairing step instead of silently.
TEST_F(PageFlipUdpTransportTest, InvalidSlotEnvironmentFallsBackToDefault) {
  PageFlipUdpTransport transport;
  ::setenv("CROSSPOINT_PAGEFLIP_PORT", TEST_BASE_PORT, 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOTS", "2", 1);
  ::setenv("CROSSPOINT_PAGEFLIP_SLOT", "nonsense", 1);
  ASSERT_TRUE(transport.begin()) << transport.lastError();
  EXPECT_EQ(transport.getSlot(), 0);
}

}  // namespace
