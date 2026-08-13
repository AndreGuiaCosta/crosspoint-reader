#pragma once

#include <cstddef>
#include <cstdint>

#include "PageFlipTransport.h"

// The paired device's identity, moving between the radio (six bytes) and settings (text).
//
// Text is the storage form because settings persistence is driven by SettingsList.h, which holds
// uint8_t and char[] and nothing else -- and because a MAC a user can read in the web UI is one
// they can copy to the second device or clear when a pair breaks up.
//
// Deliberately free of Arduino includes, like the rest of lib/PageFlip, so the host tests build it
// directly: this is parsing, and parsing is where the off-by-one lives.
namespace PageFlipMac {

// "AA:BB:CC:DD:EE:FF" plus the terminator.
inline constexpr size_t TEXT_LENGTH = 18;

// Writes the canonical uppercase form. Returns false without touching `output` when the buffer is
// too small.
bool format(const uint8_t mac[PageFlipTransport::MAC_BYTES], char* output, size_t capacity);

// Reads the canonical form, and only that: exactly six colon-separated hexadecimal pairs, upper or
// lower case. An empty string is not a MAC and is rejected like any other malformed one -- callers
// treat "no paired device" as the absence of a value, never as a MAC of zeroes, so that an unparsed
// setting can never quietly become "paired with 00:00:00:00:00:00".
bool parse(const char* text, uint8_t output[PageFlipTransport::MAC_BYTES]);

}  // namespace PageFlipMac
