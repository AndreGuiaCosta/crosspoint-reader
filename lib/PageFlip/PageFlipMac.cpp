#include "PageFlipMac.h"

#include <cstdio>
#include <cstring>

namespace {

// -1 for anything that is not a hexadecimal digit, so the caller tests one value rather than three
// ranges at every call site.
int hexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

namespace PageFlipMac {

bool format(const uint8_t mac[PageFlipTransport::MAC_BYTES], char* output, size_t capacity) {
  if (mac == nullptr || output == nullptr || capacity < TEXT_LENGTH) return false;
  std::snprintf(output, capacity, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

bool parse(const char* text, uint8_t output[PageFlipTransport::MAC_BYTES]) {
  if (text == nullptr || output == nullptr) return false;
  // Length first: it rules out both the empty string and a trailing "AA:BB:CC:DD:EE:FF:" in one
  // test, and it means the loop below can read fixed offsets without bounds-checking each one.
  if (std::strlen(text) != TEXT_LENGTH - 1) return false;

  uint8_t parsed[PageFlipTransport::MAC_BYTES] = {};
  for (size_t i = 0; i < PageFlipTransport::MAC_BYTES; ++i) {
    const size_t at = i * 3;
    const int high = hexDigit(text[at]);
    const int low = hexDigit(text[at + 1]);
    if (high < 0 || low < 0) return false;
    // Every pair but the last is followed by a separator. Checking it explicitly is what makes this
    // a parser rather than a scanner that happens to accept "AA-BB-CC-DD-EE-FF" too.
    if (i + 1 < PageFlipTransport::MAC_BYTES && text[at + 2] != ':') return false;
    parsed[i] = static_cast<uint8_t>((high << 4) | low);
  }

  // Written only once the whole string has parsed, so a caller that ignores the return value is
  // left with what it had rather than half a MAC.
  std::memcpy(output, parsed, sizeof(parsed));
  return true;
}

}  // namespace PageFlipMac
