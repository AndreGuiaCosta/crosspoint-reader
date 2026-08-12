#pragma once

#include <cstdint>

#include "PageFlipTransport.h"

// Host-side stand-in for the ESP-NOW link, so two simulator instances can pair on one machine
// (docs/pageflip.md section 11). Mandatory rather than convenient: the SDK's EspNowTransport
// returns false from begin() under SIMULATOR, so without this there is no way to develop the
// protocol, join negotiation or force-sync without two physical X4s.
//
// Each instance owns a "slot" -- a small index that stands in for a device identity. The slot
// fixes the UDP port it listens on (base + slot) and the synthetic MAC it reports, so a scripted
// test run is reproducible and the lower-MAC tiebreak in sections 3 and 4.3 resolves the same way
// every time. broadcast() sends to every other slot's port, which is as close to ESP-NOW's
// connectionless broadcast as loopback gets without multicast plumbing.
//
// Configured by environment variable so a test script can launch two instances without touching
// settings: CROSSPOINT_PAGEFLIP_SLOT (default 0), CROSSPOINT_PAGEFLIP_SLOTS (default 2),
// CROSSPOINT_PAGEFLIP_PORT (default 47190, the base port).
//
// Implemented for host builds only; on ESP32 the translation unit is empty and the real transport
// wraps freeink::nearby::EspNowTransport instead. That platform guard is what NearbyTransfer.cpp
// itself does -- it is transport selection at the interface boundary, not simulator-conditional
// firmware behaviour.

class PageFlipUdpTransport final : public PageFlipTransport {
 public:
  static constexpr uint16_t DEFAULT_BASE_PORT = 47190;
  static constexpr uint8_t DEFAULT_SLOT_COUNT = 2;
  static constexpr uint8_t MAX_SLOT_COUNT = 4;

  PageFlipUdpTransport() = default;
  ~PageFlipUdpTransport() override;

  bool begin() override;
  void end() override;
  bool isStarted() const override { return socketFd >= 0; }
  bool broadcast(const uint8_t* data, size_t length) override;
  bool poll(uint8_t* buffer, size_t capacity, size_t& length, uint8_t senderMac[MAC_BYTES]) override;
  bool localMac(uint8_t out[MAC_BYTES]) const override;

  uint8_t getSlot() const { return slot; }

  // Why the last begin()/poll() failed, for the caller to log. The transport deliberately does no
  // logging of its own: Logging.h pulls in Arduino.h, and keeping this translation unit free of it
  // is what lets the host unit tests drive real sockets. Empty when nothing has failed.
  const char* lastError() const { return errorText; }

 private:
  void setError(const char* format, ...) __attribute__((format(printf, 2, 3)));

  int socketFd = -1;
  uint8_t slot = 0;
  uint8_t slotCount = DEFAULT_SLOT_COUNT;
  uint16_t basePort = DEFAULT_BASE_PORT;
  // Fixed buffer, not a std::string: this is a diagnostic on an error path, not worth a heap
  // allocation, and the longest message it holds is the bind failure with a port in it.
  char errorText[96] = {};
};
