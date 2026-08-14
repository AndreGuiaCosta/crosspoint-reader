#include "PageFlipEspNowTransport.h"

// Device builds only: the mirror of PageFlipUdpTransport's guard. The SDK's own transport is
// compiled out under SIMULATOR (its begin() returns false there), which is exactly why the UDP
// implementation exists.
#if defined(ARDUINO_ARCH_ESP32)

#include <Logging.h>
#include <Memory.h>
#include <NearbyTransfer.h>
#include <esp_wifi.h>

#include <cstring>

namespace {

constexpr const char* MODULE = "PFNOW";
constexpr uint8_t BROADCAST_MAC[PageFlipTransport::MAC_BYTES] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

}  // namespace

// The SDK transport plus the receive scratch it fills. Both are far too large for the stack (an
// Event alone is over 1 KB), and they share a lifetime, so they are allocated and released
// together.
struct PageFlipEspNowTransport::Radio {
  freeink::nearby::EspNowTransport transport;
  freeink::nearby::EspNowTransport::Event event;
};

PageFlipEspNowTransport::PageFlipEspNowTransport() = default;

PageFlipEspNowTransport::~PageFlipEspNowTransport() { end(); }

bool PageFlipEspNowTransport::begin() {
  if (radio) return true;

  auto allocated = makeUniqueNoThrow<Radio>();
  if (!allocated) {
    LOG_ERR(MODULE, "OOM: ESP-NOW radio state");
    return false;
  }
  if (!allocated->transport.begin(CHANNEL)) {
    LOG_ERR(MODULE, "ESP-NOW transport failed to start on channel %u", static_cast<unsigned>(CHANNEL));
    return false;  // `allocated` releases here: no radio state is kept for a link that never came up
  }

  radio = std::move(allocated);
  LOG_INF(MODULE, "Paired link up on channel %u", static_cast<unsigned>(CHANNEL));
  // Section 7, step one. The SDK's begin() forces WIFI_PS_NONE, so the radio never sleeps at all.
  // Modem sleep is the mechanism the hardware actually provides for this, and the prebuilt libs set
  // CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y, so it is available to a *disconnected* STA --
  // which is what both halves are. The wake window is deliberately left at its default (maximum):
  // one variable moves here, because "does modem sleep break ESP-NOW receive at all" has to be
  // answered before any window value is worth sweeping.
  //
  // This does not latch onto anyone else. The SDK's begin() re-asserts WIFI_PS_NONE on every
  // session and its end() takes the radio down with WiFi.mode(WIFI_OFF), so a later non-PageFlip
  // WiFi user (file transfer, OTA, sync) starts from the driver default rather than from this.
  const esp_err_t psResult = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  LOG_INF(MODULE, "Modem sleep %s (wake window: default)", psResult == ESP_OK ? "enabled" : "REFUSED");
  return true;
}

void PageFlipEspNowTransport::end() {
  if (!radio) return;
  radio->transport.end();
  radio.reset();  // gives the ~5 KB back: a device reading solo should not pay for a link it is not using
}

bool PageFlipEspNowTransport::isStarted() const { return radio && radio->transport.started(); }

bool PageFlipEspNowTransport::overflowed() const { return radio && radio->transport.overflowed(); }

bool PageFlipEspNowTransport::broadcast(const uint8_t* data, size_t length) {
  if (!isStarted() || data == nullptr || length == 0 || length > MAX_PAYLOAD_BYTES) return false;
  // The SDK registers an unknown destination as a peer on first use, so the broadcast address needs
  // no setup of its own.
  return radio->transport.send(BROADCAST_MAC, data, length);
}

bool PageFlipEspNowTransport::poll(uint8_t* buffer, size_t capacity, size_t& length, uint8_t senderMac[MAC_BYTES]) {
  if (!isStarted() || buffer == nullptr || capacity == 0) return false;
  if (!radio->transport.poll(radio->event)) return false;

  // Too large for the caller is dropped whole rather than delivered as a prefix: a truncated packet
  // that still passed the length check would be a position applied from half a message.
  if (radio->event.length == 0 || radio->event.length > capacity) return false;

  std::memcpy(buffer, radio->event.data.data(), radio->event.length);
  if (senderMac != nullptr) std::memcpy(senderMac, radio->event.sourceMac.data(), MAC_BYTES);
  length = radio->event.length;
  return true;
}

bool PageFlipEspNowTransport::localMac(uint8_t out[MAC_BYTES]) const {
  if (!isStarted() || out == nullptr) return false;
  return radio->transport.localMac(out);
}

#endif  // ARDUINO_ARCH_ESP32
