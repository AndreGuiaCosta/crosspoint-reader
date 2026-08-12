#pragma once

#include <cstdint>
#include <memory>

#include "PageFlipTransport.h"

// The real link: a thin wrapper over freeink::nearby::EspNowTransport (docs/pageflip.md section 2).
// PageFlip does not implement an ESP-NOW transport -- the SDK already ships one, with an ISR-safe
// event queue that maps onto the per-frame pump, and wrapping it is a `lib_deps` entry rather than
// a port.
//
// ESP-NOW is connectionless, so there is nothing to re-establish after the cold boot that every X4
// wake actually is: store nothing, broadcast, done.
//
// The radio state lives behind a pointer that only exists between begin() and end(). The SDK's
// transport carries a four-deep queue of 1100-byte events -- around 5 KB with the receive scratch,
// sized for its own file-transfer job, while a PageFlip packet is 25 bytes. Holding that only while
// a pair is actually active is the difference between a permanent 5 KB and a rented one.

class PageFlipEspNowTransport final : public PageFlipTransport {
 public:
  // Pinned rather than negotiated: ESP-NOW peers must share a channel, and PageFlip suspends
  // whenever WiFi is up (section 6) precisely because an AP would otherwise dictate it.
  static constexpr uint8_t CHANNEL = 1;

  // Both defined in the .cpp, not defaulted here: `Radio` is incomplete at this point, and
  // std::unique_ptr needs the complete type wherever the object is created or destroyed.
  PageFlipEspNowTransport();
  ~PageFlipEspNowTransport() override;

  bool begin() override;
  void end() override;
  bool isStarted() const override;
  bool broadcast(const uint8_t* data, size_t length) override;
  bool poll(uint8_t* buffer, size_t capacity, size_t& length, uint8_t senderMac[MAC_BYTES]) override;
  bool localMac(uint8_t out[MAC_BYTES]) const override;

  // True if the SDK queue dropped a packet since the last check. Harmless for turns -- the next
  // packet's absolute position corrects any gap -- but worth surfacing while tuning.
  bool overflowed() const;

 private:
  // Named rather than anonymous so the header does not need the SDK's types, which would put
  // esp_now.h on the include path of every translation unit that pairs.
  struct Radio;
  std::unique_ptr<Radio> radio;
};
