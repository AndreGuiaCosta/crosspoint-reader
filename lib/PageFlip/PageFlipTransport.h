#pragma once

#include <cstddef>
#include <cstdint>

// The link PageFlip talks over (docs/pageflip.md section 11).
//
// Three implementations sit behind this: ESP-NOW (a thin wrapper over
// freeink::nearby::EspNowTransport) on device, UDP loopback between two simulator instances, and
// UART as the escape hatch if the power budget in section 7 does not hold. The simulator one is
// mandatory rather than convenient -- the SDK transport's begin() returns false under SIMULATOR --
// which is why this interface exists on day one.
//
// No callbacks: the reader pumps poll() once per frame from loop(), matching the SDK transport's
// own poll-based event queue. No allocation either -- the caller owns every buffer, so a packet
// costs nothing on the heap.

class PageFlipTransport {
 public:
  static constexpr size_t MAC_BYTES = 6;
  // Largest datagram any implementation must carry. PageFlip packets are tens of bytes; this
  // leaves room for the join exchange without inviting anything framebuffer-sized.
  //
  // The settings offer of section 5.1 is the largest at 63 bytes with a full-length font name, so
  // the headroom above it is deliberate: the decoders tolerate trailing bytes precisely so a later
  // version can append fields, and a cap that only just fits today's worst case would make that
  // impossible. Oversized datagrams are dropped whole rather than truncated, so an undersized cap
  // does not present as an error -- it presents as a peer that never answers.
  static constexpr size_t MAX_PAYLOAD_BYTES = 96;

  virtual ~PageFlipTransport() = default;

  // Brings the link up. Returns false if the radio (or socket) is unavailable, in which case the
  // reader carries on solo -- pairing must never block reading.
  virtual bool begin() = 0;
  virtual void end() = 0;
  virtual bool isStarted() const = 0;

  // Sends to every peer on the link. PageFlip broadcasts because a turn is addressed to whoever is
  // paired, and ESP-NOW needs no association to receive it.
  virtual bool broadcast(const uint8_t* data, size_t length) = 0;

  // Non-blocking receive of one datagram. Returns false when nothing is queued. `length` is set to
  // the bytes written into `buffer`; a datagram larger than `capacity` is dropped rather than
  // truncated, so a malformed sender cannot be mistaken for a short packet.
  virtual bool poll(uint8_t* buffer, size_t capacity, size_t& length, uint8_t senderMac[MAC_BYTES]) = 0;

  // Identity of this device, used for the lower-MAC tiebreak in sections 3 and 4.3.
  virtual bool localMac(uint8_t out[MAC_BYTES]) const = 0;
};
