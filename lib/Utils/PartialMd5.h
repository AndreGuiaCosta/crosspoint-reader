#pragma once
#include <cstddef>
#include <string>

// Sparse partial-MD5 of a file: up to 1KB sampled at offsets {0, 1024 << 2i}.
// KOReader's binary fast-digest and Readest's book_hash independently specify
// the exact same sampling, so both delegate here. The offset table and chunk
// size must match those clients bit-for-bit — the hash IS the book identity
// on their servers.
namespace PartialMd5 {

// A contiguous byte range of the file that hashFile will hash.
struct SampleRange {
  size_t start;
  size_t length;
};

// Upper bound on the number of samples ever produced.
constexpr size_t MAX_SAMPLES = 12;
constexpr size_t SAMPLE_SIZE = 1024;

// Header-inline so host tests can exercise it without Arduino deps.
inline size_t sampleRanges(size_t fileSize, SampleRange out[MAX_SAMPLES]) {
  // Offsets evaluated under JS int32 shift semantics: `1024 << -2` is
  // `1024 << 30` (overflow → 0), NOT `1024 >> 2 = 256`.
  constexpr size_t OFFSETS[MAX_SAMPLES] = {
      0u, 1024u, 4096u, 16384u, 65536u, 262144u, 1048576u, 4194304u, 16777216u, 67108864u, 268435456u, 1073741824u,
  };
  size_t count = 0;
  for (size_t i = 0; i < MAX_SAMPLES; ++i) {
    const size_t start = OFFSETS[i];
    if (start >= fileSize) break;
    const size_t remaining = fileSize - start;
    out[count].start = start;
    out[count].length = remaining < SAMPLE_SIZE ? remaining : SAMPLE_SIZE;
    ++count;
  }
  return count;
}

// What to do when a seek/read inside a sample fails mid-hash.
enum class ReadErrorPolicy {
  ABORT,  // return "" — Readest: a partial hash is a wrong book identity
  SKIP,   // drop the sample and continue — KOReader reference behaviour
};

// 32-char lowercase hex, or "" when the file can't be opened (or on any
// read/seek failure under ABORT). `tag` is the log context.
std::string hashFile(const char* tag, const std::string& filePath, ReadErrorPolicy policy);

}  // namespace PartialMd5
