#pragma once
#include <cstddef>
#include <string>

class Epub;

// Book-identification hashes for Readest Sync. Must match the Readest web
// client bit-for-bit or the server treats books as different records.
class ReadestHash {
 public:
  // A contiguous byte range of the file that partialMd5 will hash.
  struct SampleRange {
    size_t start;
    size_t length;
  };

  // Upper bound on the number of samples partialMd5 ever produces.
  static constexpr size_t MAX_SAMPLES = 12;
  static constexpr size_t SAMPLE_SIZE = 1024;

  // Header-inline so the host test can exercise it without Arduino deps.
  static inline size_t partialMd5SampleRanges(size_t fileSize, SampleRange out[MAX_SAMPLES]) {
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

  // Partial-MD5 "book_hash" of the raw file bytes; 32-char lowercase hex,
  // or empty on read/open failure. NOT interchangeable with KOReader's
  // partial-MD5 — different offset table and small-file behaviour.
  static std::string partialMd5(const std::string& filePath);

  // Metadata-MD5 "meta_hash":
  //   hashSource = title + "|" + authors.join(",") + "|" + identifiers
  //   meta_hash  = md5(NFC_normalize(hashSource))
  //
  // Identifiers: prefer scheme uuid > calibre > isbn (case-insensitive
  // substring match). If a preferred scheme is found, use only that
  // identifier; otherwise join all by ",". Each identifier value is
  // normalized by stripping everything up to and including the last ":"
  // (for urn:...) or the first ":" (otherwise).
  //
  // Limitations:
  //   - NFC normalization is NOT applied. Non-ASCII may mismatch.
  //   - EPUB 3 refines-based altIdentifier precedence is NOT applied.
  //   - Multi-language title objects are NOT handled.
  //
  // Returns 32-char lowercase hex, or empty on parse failure.
  static std::string metaMd5(const Epub& epub);
};
