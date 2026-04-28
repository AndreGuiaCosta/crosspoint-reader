#pragma once
#include <cstdint>
#include <string>

/**
 * Readest Sync API client (handoff §4). V1 covers `book_configs` only —
 * one row per (user, book_hash) holding the user's reading progress and
 * position pointers.
 *
 * Two endpoints:
 *   GET  {sync_api}/sync?since=<ms>&type=configs&book=<bh>&meta_hash=<mh>
 *   POST {sync_api}/sync   body: { "configs": [ <BookConfig> ] }
 *
 * Both require `Authorization: Bearer <access_token>` from
 * READEST_STORE.getAccessToken(). On 401/403 the caller is expected to
 * refresh the token (via ReadestAuthClient::refresh) and retry once —
 * see handoff §8 step 3.
 *
 * The wire format has two well-known quirks (§13.5, §13.7) that this
 * client handles:
 *
 *   1. `progress` round-trips asymmetrically — pushed as a JSON array
 *      `[cur, total]`, returned as a stringified array `"[cur,total]"`.
 *   2. `updatedAt` on push is a unix-ms number; the response field
 *      `updated_at` is an ISO-8601 string. Both are converted to int64
 *      ms in BookConfig::updatedAtMs.
 */
class ReadestSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_AUTH,        // No access token present
    BAD_REQUEST,    // 400 — usually a missing required query param
    AUTH_EXPIRED,   // 401 / 403 — refresh and retry
    NETWORK_ERROR,  // HTTP < 0 (connect / timeout)
    SERVER_ERROR,   // 5xx
    JSON_ERROR,     // Response was not parseable JSON
  };

  // Single-book sync state. Field names match the camelCase wire format
  // used on push; pull responses (snake_case) are normalized into the
  // same struct.
  struct BookConfig {
    std::string bookHash;     // 32-hex partial-MD5 of the file
    std::string metaHash;     // 32-hex MD5 of normalized OPF metadata
    std::string xpointer;     // KOReader-format XPointer (our V1 position)
    std::string location;     // CFI string; V1 sends empty, may receive non-empty
    int progressCurrent = 0;  // 1-based current page
    int progressTotal = 0;    // total pages
    int64_t updatedAtMs = 0;  // unix ms — server's authoritative timestamp on echo
    bool deleted = false;     // true iff pull returned a non-null deleted_at
  };

  /**
   * GET the configs for a single book. The server's filter is
   * `book_hash = bookHash OR meta_hash = metaHash`, so the response may
   * contain rows for *other* books that happen to share metadata; we
   * pick the one matching `bookHash` exactly.
   *
   * @param sinceMs    Cursor — server returns rows with updated_at > since.
   *                   Use 0 for a full pull.
   * @param bookHash   Required — the open book's partial-MD5.
   * @param metaHash   Required — the open book's metadata MD5.
   * @param out        On OK with a match, populated with the config.
   *                   On OK without a match, `out->bookHash` is left empty.
   * @param maxUpdatedAtMs  Out — max(updated_at) across returned rows
   *                        (0 if none). Caller persists this as the new
   *                        sync cursor.
   * @param errMsg     Optional — server error message on 4xx/5xx.
   */
  static Error pullConfig(int64_t sinceMs, const std::string& bookHash, const std::string& metaHash, BookConfig* out,
                          int64_t* maxUpdatedAtMs, std::string* errMsg = nullptr);

  /**
   * POST a single-book config. Wraps `cfg` into `{ configs: [cfg] }`.
   *
   * @param cfg                The config to push. updatedAtMs must be set.
   * @param outAuthoritative   On OK, populated with the server's row
   *                           after the write — caller should adopt the
   *                           server-side updated_at (last-writer-wins
   *                           may have left the server row as the winner).
   * @param errMsg             Optional — server error message on 4xx/5xx.
   */
  static Error pushConfig(const BookConfig& cfg, BookConfig* outAuthoritative, std::string* errMsg = nullptr);

  static const char* errorString(Error err);
};
