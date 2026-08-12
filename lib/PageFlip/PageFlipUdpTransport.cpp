#include "PageFlipUdpTransport.h"

// Host builds only. On device the real transport wraps freeink::nearby::EspNowTransport, and this
// translation unit is deliberately empty rather than #ifdef'd internally -- the guard selects a
// transport implementation, it does not vary firmware behaviour. It is the same guard
// NearbyTransfer.cpp uses on its own platform code.
#if !defined(ARDUINO_ARCH_ESP32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Locally-administered MAC (bit 1 of the first octet), 'P' 'F', then the slot. Stable across runs
// and ordered by slot, which is what the lower-MAC tiebreak in sections 3 and 4.3 needs.
void macForSlot(uint8_t slot, uint8_t out[PageFlipTransport::MAC_BYTES]) {
  out[0] = 0x02;
  out[1] = 'P';
  out[2] = 'F';
  out[3] = 0x00;
  out[4] = 0x00;
  out[5] = slot;
}

// Reads an unsigned environment override, falling back to `fallback` when unset, unparseable or
// out of range. A typo in a test script must not silently produce a transport that listens
// somewhere nobody is sending.
long readEnvNumber(const char* name, long fallback, long minimum, long maximum) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return fallback;

  char* end = nullptr;
  const long value = std::strtol(raw, &end, 10);
  if (end == raw || *end != '\0' || value < minimum || value > maximum) return fallback;
  return value;
}

sockaddr_in loopbackAddress(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

}  // namespace

PageFlipUdpTransport::~PageFlipUdpTransport() { end(); }

void PageFlipUdpTransport::setError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vsnprintf(errorText, sizeof(errorText), format, args);
  va_end(args);
}

bool PageFlipUdpTransport::begin() {
  if (isStarted()) return true;
  errorText[0] = '\0';

  slotCount = static_cast<uint8_t>(readEnvNumber("CROSSPOINT_PAGEFLIP_SLOTS", DEFAULT_SLOT_COUNT, 2, MAX_SLOT_COUNT));
  slot = static_cast<uint8_t>(readEnvNumber("CROSSPOINT_PAGEFLIP_SLOT", 0, 0, slotCount - 1));
  basePort =
      static_cast<uint16_t>(readEnvNumber("CROSSPOINT_PAGEFLIP_PORT", DEFAULT_BASE_PORT, 1024, 65535 - MAX_SLOT_COUNT));

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    setError("socket() failed: %s", std::strerror(errno));
    return false;
  }

  // Deliberately no SO_REUSEADDR. UDP has no TIME_WAIT to wait out, so it buys nothing here, and
  // it would let two instances launched with the same slot both bind the port -- after which the
  // kernel delivers each datagram to only one of them and the pair looks intermittently deaf.
  // Without it the second bind fails loudly, which is the diagnosis we want.
  const sockaddr_in address = loopbackAddress(static_cast<uint16_t>(basePort + slot));
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    // Almost always a second instance launched with the same slot. Name that: it otherwise
    // presents as a peer that never answers, which looks exactly like the peer-offline path.
    setError("bind 127.0.0.1:%u failed (%s) -- slot %u already in use?", static_cast<unsigned>(basePort + slot),
             std::strerror(errno), static_cast<unsigned>(slot));
    ::close(fd);
    return false;
  }

  socketFd = fd;
  return true;
}

void PageFlipUdpTransport::end() {
  if (socketFd >= 0) {
    ::close(socketFd);
    socketFd = -1;
  }
}

bool PageFlipUdpTransport::broadcast(const uint8_t* data, size_t length) {
  if (!isStarted() || data == nullptr || length == 0 || length > MAX_PAYLOAD_BYTES) return false;

  // Every slot but our own: ESP-NOW does not loop a broadcast back to the sender, so neither does
  // this. Otherwise the protocol would need self-filtering that exists purely for the shim.
  bool sentAny = false;
  for (uint8_t peer = 0; peer < slotCount; ++peer) {
    if (peer == slot) continue;
    const sockaddr_in address = loopbackAddress(static_cast<uint16_t>(basePort + peer));
    const ssize_t sent =
        ::sendto(socketFd, data, length, MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    // A peer that is not running yet refuses the datagram. That is the normal solo case, not an
    // error worth recording on every page turn.
    if (sent >= 0) sentAny = true;
  }
  return sentAny;
}

bool PageFlipUdpTransport::poll(uint8_t* buffer, size_t capacity, size_t& length, uint8_t senderMac[MAC_BYTES]) {
  if (!isStarted() || buffer == nullptr || capacity == 0) return false;

  sockaddr_in from{};
  socklen_t fromLength = sizeof(from);
  // MSG_TRUNC reports the datagram's real size even when it did not fit, so an oversized packet is
  // dropped rather than silently delivered as a short one -- a truncated packet that still decodes
  // would be a position applied from half a message.
  const ssize_t received = ::recvfrom(socketFd, buffer, capacity, MSG_DONTWAIT | MSG_TRUNC,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
  if (received < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) setError("recvfrom failed: %s", std::strerror(errno));
    return false;
  }
  if (static_cast<size_t>(received) > capacity) return false;

  if (senderMac != nullptr) {
    const uint16_t senderPort = ntohs(from.sin_port);
    // A datagram from outside the slot range keeps a distinguishable identity rather than
    // impersonating slot 0.
    const bool knownSlot = senderPort >= basePort && senderPort < basePort + slotCount;
    macForSlot(knownSlot ? static_cast<uint8_t>(senderPort - basePort) : 0xFF, senderMac);
  }

  length = static_cast<size_t>(received);
  return true;
}

bool PageFlipUdpTransport::localMac(uint8_t out[MAC_BYTES]) const {
  if (out == nullptr) return false;
  macForSlot(slot, out);
  return true;
}

#endif  // !ARDUINO_ARCH_ESP32
