#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * Readest cloud-storage client (handoff §14). Phase 2 (V3) cares about three
 * read-only operations against the same auth-gated `getSyncApiBase()`:
 *
 *   GET  {sync_api}/sync?since=<ms>&type=books             — books table
 *   GET  {sync_api}/storage/list?bookHash=<hash>           — files table
 *   POST {sync_api}/storage/download                       — presigned URLs
 *
 * All three require `Authorization: Bearer <access_token>`. On 401/403 the
 * caller is expected to refresh and retry once via ReadestStorageCoordinator,
 * mirroring how the progress-sync side already does it.
 *
 * Wire quirks worth keeping in mind:
 *   - Books-table responses use snake_case columns (book_hash, meta_hash,
 *     uploaded_at, …). The `progress` field on books is a JSON array
 *     `[cur, total]` (NOT the stringified form configs use — handoff §13.5).
 *   - Timestamps come back as ISO-8601 strings; we normalize to unix ms.
 *   - The hotfix dummy book row with all-zero hash (handoff §13.1) is
 *     filtered out here so callers never see it.
 */
class ReadestStorageClient {
 public:
  enum Error {
    OK = 0,
    NO_AUTH,        // No access token present
    BAD_REQUEST,    // 400
    AUTH_EXPIRED,   // 401 / 403 — refresh and retry
    NOT_FOUND,      // 404 — used by storage endpoints when a key isn't owned
    NETWORK_ERROR,  // HTTP < 0 (connect / timeout)
    SERVER_ERROR,   // 5xx
    JSON_ERROR,     // Response was not parseable JSON
  };

  // One row from `books` (handoff §6.2). Server-side snake_case columns
  // are normalized to camelCase here.
  struct BookRow {
    std::string hash;      // 32-hex partial-MD5 of the file (book_hash)
    std::string metaHash;  // 32-hex MD5 of normalized OPF metadata
    std::string format;    // "EPUB" / "PDF" / etc — ext map at handoff §6.4
    std::string title;
    std::string sourceTitle;
    std::string author;
    int progressCurrent = 0;  // Mirrored from book_configs; 0 if absent
    int progressTotal = 0;
    int64_t uploadedAtMs = 0;  // Non-zero iff the file is in cloud storage
    int64_t updatedAtMs = 0;   // Last-writer-wins cursor
    bool deleted = false;      // True iff deleted_at is present (soft-delete)
  };

  // One row from `files` (handoff §6.4). Cover.png and the book file each
  // get their own row under the same book_hash group.
  struct FileRow {
    std::string fileKey;   // "<userId>/Readest/Books/<hash>/<name>.<ext>"
    int64_t fileSize = 0;  // Bytes
    std::string bookHash;
    int64_t updatedAtMs = 0;
  };

  /**
   * GET books rows newer than `sinceMs`.
   *
   * @param sinceMs    Cursor — server returns rows with updated_at > since,
   *                   plus rows where deleted_at > since (so soft-deletes
   *                   propagate). Use 0 for a full pull.
   * @param out        Populated with all returned rows EXCEPT the dummy
   *                   hotfix row (handoff §13.1). Caller still needs to
   *                   filter for `uploadedAt != 0 && !deleted` before
   *                   showing or downloading.
   * @param maxUpdatedAtMs  Out — max(updated_at) across returned rows
   *                        (0 if none). Caller persists for next pull.
   * @param errMsg     Optional — server error message on 4xx/5xx.
   */
  static Error pullBooksSince(int64_t sinceMs, std::vector<BookRow>* out, int64_t* maxUpdatedAtMs,
                              std::string* errMsg = nullptr);

  /**
   * GET the file rows for a single book group. The endpoint always returns
   * ALL files associated with the book_hash (book file + cover) — the
   * `bookHash` filter does not exclude covers (handoff §14.4.3).
   *
   * @param bookHash   32-hex hash to filter on.
   * @param out        Populated with one row per file (book + cover).
   * @param errMsg     Optional — server error message on 4xx/5xx.
   */
  static Error listFilesByBookHash(const std::string& bookHash, std::vector<FileRow>* out,
                                   std::string* errMsg = nullptr);

  /**
   * POST a batch of file_keys to obtain presigned GET URLs (30-min TTL,
   * handoff §14.4.2). Keys the server cannot resolve are silently omitted
   * from `outUrls`; the caller compares the input list against the map to
   * detect missing keys.
   *
   * @param fileKeys   Ordered list of file_keys; each typically obtained
   *                   from listFilesByBookHash or a books pull.
   * @param outUrls    Map of fileKey → signed download URL. Empty on error.
   * @param errMsg     Optional — server error message on 4xx/5xx.
   */
  static Error getDownloadUrls(const std::vector<std::string>& fileKeys, std::map<std::string, std::string>* outUrls,
                               std::string* errMsg = nullptr);

  static const char* errorString(Error err);
};
