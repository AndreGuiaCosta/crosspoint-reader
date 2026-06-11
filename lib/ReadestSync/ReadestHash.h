#pragma once
#include <cstddef>
#include <string>

#include "../Utils/PartialMd5.h"

class Epub;

// Book-identification hashes for Readest Sync. Must match the Readest web
// client bit-for-bit or the server treats books as different records.
class ReadestHash {
 public:
  // Sampling lives in lib/Utils/PartialMd5 (shared with KOReaderDocumentId —
  // both protocols specify the identical offset table). Aliases keep the
  // host test and existing callers source-compatible.
  using SampleRange = PartialMd5::SampleRange;
  static constexpr size_t MAX_SAMPLES = PartialMd5::MAX_SAMPLES;
  static constexpr size_t SAMPLE_SIZE = PartialMd5::SAMPLE_SIZE;

  static inline size_t partialMd5SampleRanges(size_t fileSize, SampleRange out[MAX_SAMPLES]) {
    return PartialMd5::sampleRanges(fileSize, out);
  }

  // Partial-MD5 "book_hash" of the raw file bytes; 32-char lowercase hex,
  // or empty on read/open failure. Same sampling as KOReader's partial-MD5;
  // policies differ only on mid-hash read errors (Readest aborts, KOReader
  // skips the sample).
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
