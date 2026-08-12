#include "PageFlipCompat.h"

#include <cstring>

namespace {

constexpr uint32_t FNV_OFFSET_BASIS = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

// FNV-1a, the same construction SdCardFontManager::computeFontId uses, so the codebase has one
// hash idiom rather than two.
void mix8(uint32_t& hash, const uint8_t value) {
  hash ^= value;
  hash *= FNV_PRIME;
}

// Fields are mixed a byte at a time, little end first, for the same reason the wire encoder spells
// its integers out: the hash has to be identical on both devices, and hashing the struct's raw
// bytes would fold in padding this code does not control.
void mix16(uint32_t& hash, const uint16_t value) {
  mix8(hash, static_cast<uint8_t>(value & 0xFF));
  mix8(hash, static_cast<uint8_t>((value >> 8) & 0xFF));
}

void mix32(uint32_t& hash, const uint32_t value) {
  mix16(hash, static_cast<uint16_t>(value & 0xFFFF));
  mix16(hash, static_cast<uint16_t>((value >> 16) & 0xFFFF));
}

// The float's bit pattern, not its value: Section's cache check compares lineCompression with !=,
// and hashing the bits agrees with that for every value the settings can produce. memcpy rather
// than a pointer cast because ESP32-C3 faults on unaligned wide loads.
void mixFloat(uint32_t& hash, const float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  mix32(hash, bits);
}

}  // namespace

uint32_t computePageFlipCompatHash(const ReaderRenderSpec& spec, const uint32_t bookId, const uint8_t layoutVersion) {
  // Destructuring instead of reading spec.field directly is the point, not a style choice: a
  // structured binding must name every member, so adding an eleventh field to ReaderRenderSpec
  // stops compiling here until someone decides whether it belongs in the hash. Section 5 claims
  // the hash is "complete by construction" -- this is what makes that true. A sizeof() assert
  // would not: the struct carries two bytes of tail padding, so two more bool fields would fit
  // inside it without changing its size, and would be silently omitted.
  const auto& [fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
               hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled] = spec;

  uint32_t hash = FNV_OFFSET_BASIS;
  mix32(hash, static_cast<uint32_t>(fontId));
  mixFloat(hash, lineCompression);
  mix8(hash, extraParagraphSpacing ? 1 : 0);
  mix8(hash, paragraphAlignment);
  mix16(hash, viewportWidth);
  mix16(hash, viewportHeight);
  mix8(hash, hyphenationEnabled ? 1 : 0);
  mix8(hash, embeddedStyle ? 1 : 0);
  mix8(hash, imageRendering);
  mix8(hash, focusReadingEnabled ? 1 : 0);
  mix32(hash, bookId);
  mix8(hash, layoutVersion);

  // Never hand back the sentinel: a device that has not computed its hash yet advertises 0, and a
  // real hash colliding with it would read as "compatible with a device that has not decided".
  return hash != 0 ? hash : 1;
}
