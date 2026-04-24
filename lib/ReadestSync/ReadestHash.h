#pragma once
#include <string>

class Epub;

/**
 * Hashing functions required for Readest Sync's book identification.
 * Both must match Readest's web client bit-for-bit, or the server will
 * treat CrossPoint's books as different records from the phone/web app
 * and progress sync will silently fail.
 *
 * Algorithms are documented in CROSSPOINT_SYNC_HANDOFF.md §5.1 and §5.2.
 */
class ReadestHash {
 public:
  /**
   * Partial-MD5 "book_hash" of the raw file bytes.
   *
   * Samples up to 1 KiB at offsets 256, 1024, 4096, 16384, ..., 1 GiB,
   * stopping when the offset passes EOF. Matches Readest's
   * apps/readest-app/src/utils/md5.ts partialMD5.
   *
   * CrossPoint already has a different partial-MD5 for KOReader sync
   * (offset 0 instead of 256, no early-break on small files) — these
   * are NOT interchangeable.
   *
   * @param filePath Absolute path to the EPUB on SD card.
   * @return 32-char lowercase hex, or empty string on read/open failure.
   */
  static std::string partialMd5(const std::string& filePath);

  /**
   * Metadata-MD5 "meta_hash" per Readest's getMetadataHash
   * (apps/readest-app/src/utils/book.ts:311-323):
   *
   *   hashSource = title + "|" + authors.join(",") + "|" + identifiers
   *   meta_hash  = md5(NFC_normalize(hashSource))
   *
   * Identifiers: prefer scheme uuid > calibre > isbn (case-insensitive
   * substring match on the scheme). If a preferred scheme is found, use
   * only that identifier; otherwise join all identifiers by ",". Each
   * identifier value is normalized by stripping everything up to and
   * including the last ":" (for urn:...) or the first ":" (otherwise).
   *
   * Known V1 limitations:
   *   - NFC normalization is NOT applied. ASCII titles/authors/IDs match
   *     Readest; Chinese/Japanese/accented strings may not. Logged when
   *     detected.
   *   - EPUB 3 refines-based altIdentifier precedence is NOT applied.
   *   - Multi-language title objects are NOT handled (CrossPoint stores
   *     only one dc:title regardless).
   *
   * @param epub A loaded Epub — its OPF will be re-read to extract the
   *             identifiers and author list that BookMetadataCache discards.
   * @return 32-char lowercase hex, or empty string on parse failure.
   */
  static std::string metaMd5(const Epub& epub);
};
