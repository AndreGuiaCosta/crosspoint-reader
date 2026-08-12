#pragma once
#include <Epub/ReaderRenderSpec.h>

#include <cstdint>

// Layout compatibility between the two devices of a pair (docs/pageflip.md section 5).
//
// A spread is only coherent if "spine 2, page 5" names the same text on both devices, which holds
// exactly when they paginate identically. ReaderRenderSpec is the codebase's own answer to what
// decides pagination -- Section's cache validation discards a .bin built with any different field
// -- so the hash is derived from that struct rather than from a hand-picked list of settings.
//
// layoutVersion is Section::FILE_VERSION, the layout epoch. It is bumped whenever a change
// re-paginates existing books, which makes it the one thing the spec cannot express: two devices
// on different builds that lay out identical settings differently.
//
// Returns a non-zero hash. Zero is reserved as the reader's "not computed yet" sentinel, because
// the viewport is not known until the first render has run.
uint32_t computePageFlipCompatHash(const ReaderRenderSpec& spec, uint32_t bookId, uint8_t layoutVersion);
