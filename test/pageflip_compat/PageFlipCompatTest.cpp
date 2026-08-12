#include <gtest/gtest.h>

#include <set>

#include "PageFlip/PageFlipCompat.h"

namespace {

// A fully-populated spec, so every test below changes exactly one field away from a known state.
// Deliberately not the struct's defaults: a hash that ignored a field would still pass if the
// baseline left that field at a value the mutation could not move away from meaningfully.
ReaderRenderSpec baseline() {
  ReaderRenderSpec spec;
  spec.fontId = 0x5A5A1234;
  spec.lineCompression = 1.0f;
  spec.extraParagraphSpacing = false;
  spec.paragraphAlignment = 1;
  spec.viewportWidth = 760;
  spec.viewportHeight = 430;
  spec.hyphenationEnabled = true;
  spec.embeddedStyle = true;
  spec.imageRendering = 2;
  spec.focusReadingEnabled = false;
  return spec;
}

constexpr uint32_t BOOK_ID = 0xDEADBEEF;
constexpr uint8_t LAYOUT_VERSION = 36;

uint32_t hashOf(const ReaderRenderSpec& spec) { return computePageFlipCompatHash(spec, BOOK_ID, LAYOUT_VERSION); }

}  // namespace

TEST(PageFlipCompat, IdenticalSpecsAgree) {
  // The case the feature depends on: two devices configured the same way pair up.
  EXPECT_EQ(hashOf(baseline()), hashOf(baseline()));
}

TEST(PageFlipCompat, EveryRenderFieldChangesTheHash) {
  // Each field feeds pagination -- Section discards a cached .bin when any of them differs -- so
  // each must move the hash. A field silently omitted here is a pair that reports "compatible"
  // and then shows different text on each device, which is the failure section 5 exists to catch.
  const uint32_t base = hashOf(baseline());

  auto mutated = [](auto&& apply) {
    ReaderRenderSpec spec = baseline();
    apply(spec);
    return hashOf(spec);
  };

  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.fontId = 0x5A5A1235; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.lineCompression = 0.95f; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.extraParagraphSpacing = true; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.paragraphAlignment = 2; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.viewportWidth = 761; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.viewportHeight = 431; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.hyphenationEnabled = false; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.embeddedStyle = false; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.imageRendering = 3; }));
  EXPECT_NE(base, mutated([](ReaderRenderSpec& s) { s.focusReadingEnabled = true; }));
}

TEST(PageFlipCompat, LineCompressionDistinguishesNeighbouringValues) {
  // lineCompression folds in font family AND line spacing, and its steps are small (0.90/0.95/1.0
  // /1.1). Hashing it as a float value rather than as bits would be fine; rounding it to an
  // integer, which is a tempting way to avoid float hashing, would collapse three of those four.
  std::set<uint32_t> hashes;
  for (const float compression : {0.90f, 0.95f, 1.0f, 1.1f}) {
    ReaderRenderSpec spec = baseline();
    spec.lineCompression = compression;
    hashes.insert(hashOf(spec));
  }
  EXPECT_EQ(hashes.size(), 4u);
}

TEST(PageFlipCompat, BookIdChangesTheHash) {
  // Two devices on different books have nothing to say to each other. bookId is checked separately
  // before the hash, so this is belt and braces -- but it costs nothing and keeps the hash a
  // complete statement of "we are showing the same thing".
  const ReaderRenderSpec spec = baseline();
  EXPECT_NE(computePageFlipCompatHash(spec, BOOK_ID, LAYOUT_VERSION),
            computePageFlipCompatHash(spec, BOOK_ID + 1, LAYOUT_VERSION));
}

TEST(PageFlipCompat, LayoutVersionChangesTheHash) {
  // The cross-firmware guard: identical settings laid out by different builds are not compatible.
  const ReaderRenderSpec spec = baseline();
  EXPECT_NE(computePageFlipCompatHash(spec, BOOK_ID, LAYOUT_VERSION),
            computePageFlipCompatHash(spec, BOOK_ID, LAYOUT_VERSION + 1));
}

TEST(PageFlipCompat, NeverReturnsTheNotComputedSentinel) {
  // Zero means "this device has not fixed its viewport yet". A real hash of zero would advertise
  // compatibility with a device that has not decided, so the function remaps it.
  for (uint16_t width = 0; width < 512; ++width) {
    ReaderRenderSpec spec = baseline();
    spec.viewportWidth = width;
    spec.viewportHeight = static_cast<uint16_t>(1024 - width);
    EXPECT_NE(hashOf(spec), 0u) << "width " << width;
  }
}

TEST(PageFlipCompat, WireValueIsPinned) {
  // The hash travels between devices, so changing the algorithm silently makes every previously
  // paired device incompatible. This pins the value; if it fails, that is the change being
  // noticed, not a bug. Bump Section::FILE_VERSION alongside any deliberate change so already
  // built caches are invalidated too.
  EXPECT_EQ(hashOf(baseline()), 0x8DAC8BC4u);
}
