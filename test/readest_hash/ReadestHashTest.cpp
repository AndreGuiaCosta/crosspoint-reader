#include <cstdio>

#include "lib/ReadestSync/ReadestHash.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                                                           \
  do {                                                                                                            \
    if ((a) != (b)) {                                                                                             \
      fprintf(stderr, "  FAIL: %s:%d: %s == %zu, expected %zu\n", __FILE__, __LINE__, #a, static_cast<size_t>(a), \
              static_cast<size_t>(b));                                                                            \
      testsFailed++;                                                                                              \
      return;                                                                                                     \
    }                                                                                                             \
  } while (0)

#define PASS()     \
  do {             \
    testsPassed++; \
  } while (0)

using SR = ReadestHash::SampleRange;

static void testEmptyFile() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(0, ranges);
  ASSERT_EQ(n, 0u);
  PASS();
}

// Files smaller than the per-sample size hash a single contiguous prefix.
static void testTinyFile() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(100, ranges);
  ASSERT_EQ(n, 1u);
  ASSERT_EQ(ranges[0].start, 0u);
  ASSERT_EQ(ranges[0].length, 100u);
  PASS();
}

// First-sample offset must be 0, not 256.
static void testFileExactly256() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(256, ranges);
  ASSERT_EQ(n, 1u);
  ASSERT_EQ(ranges[0].start, 0u);
  ASSERT_EQ(ranges[0].length, 256u);
  PASS();
}

// At exactly 1024 the second offset is right at EOF and must not produce a
// zero-length range.
static void testFileExactly1024() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(1024, ranges);
  ASSERT_EQ(n, 1u);
  ASSERT_EQ(ranges[0].start, 0u);
  ASSERT_EQ(ranges[0].length, 1024u);
  PASS();
}

// 2 KiB exactly: two adjacent samples that together hash bytes [0, 2048).
static void testFileExactly2048() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(2048, ranges);
  ASSERT_EQ(n, 2u);
  ASSERT_EQ(ranges[0].start, 0u);
  ASSERT_EQ(ranges[0].length, 1024u);
  ASSERT_EQ(ranges[1].start, 1024u);
  ASSERT_EQ(ranges[1].length, 1024u);
  PASS();
}

// 5 KB: three samples, last one truncated (4096 to 5000).
static void testMediumFile() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(5000, ranges);
  ASSERT_EQ(n, 3u);
  ASSERT_EQ(ranges[0].start, 0u);
  ASSERT_EQ(ranges[0].length, 1024u);
  ASSERT_EQ(ranges[1].start, 1024u);
  ASSERT_EQ(ranges[1].length, 1024u);
  ASSERT_EQ(ranges[2].start, 4096u);
  ASSERT_EQ(ranges[2].length, 904u);
  PASS();
}

// Boundary: file exactly 4096 — third offset is == fileSize, must break.
static void testFileExactly4096() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(4096, ranges);
  ASSERT_EQ(n, 2u);
  ASSERT_EQ(ranges[1].start, 1024u);
  ASSERT_EQ(ranges[1].length, 1024u);
  PASS();
}

// One byte past the third offset — yields a 1-byte third sample.
static void testFile4097() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(4097, ranges);
  ASSERT_EQ(n, 3u);
  ASSERT_EQ(ranges[2].start, 4096u);
  ASSERT_EQ(ranges[2].length, 1u);
  PASS();
}

// 1.5 GiB exercises all 12 offsets (last one is at 1 GiB exactly).
static void testFullSampleSet() {
  constexpr size_t SIZE = 1610612736u;  // 1.5 * 1024^3
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(SIZE, ranges);
  ASSERT_EQ(n, 12u);
  // Spot-check the offset table.
  ASSERT_EQ(ranges[0].start, 0u);
  ASSERT_EQ(ranges[1].start, 1024u);
  ASSERT_EQ(ranges[6].start, 1048576u);
  ASSERT_EQ(ranges[11].start, 1073741824u);
  // All samples should be the full 1024 bytes; SIZE - 1073741824 = 536870912.
  ASSERT_EQ(ranges[11].length, 1024u);
  PASS();
}

// Files at or beyond the 1 GiB last-offset boundary still terminate cleanly.
static void testFileAt1GiB() {
  constexpr size_t SIZE = 1073741824u;
  SR ranges[ReadestHash::MAX_SAMPLES];
  const size_t n = ReadestHash::partialMd5SampleRanges(SIZE, ranges);
  // Last offset is == size, so it breaks before adding the 12th range.
  ASSERT_EQ(n, 11u);
  ASSERT_EQ(ranges[10].start, 268435456u);
  PASS();
}

// Regression: first offset must be 0 (JS `1024 << -2` quirk).
static void testJsParityFirstOffsetIsZero() {
  SR ranges[ReadestHash::MAX_SAMPLES];
  ReadestHash::partialMd5SampleRanges(8192, ranges);
  ASSERT_EQ(ranges[0].start, 0u);
  PASS();
}

int main() {
  testEmptyFile();
  testTinyFile();
  testFileExactly256();
  testFileExactly1024();
  testFileExactly2048();
  testMediumFile();
  testFileExactly4096();
  testFile4097();
  testFullSampleSet();
  testFileAt1GiB();
  testJsParityFirstOffsetIsZero();

  fprintf(stderr, "ReadestHashTest: %d passed, %d failed\n", testsPassed, testsFailed);
  return testsFailed == 0 ? 0 : 1;
}
