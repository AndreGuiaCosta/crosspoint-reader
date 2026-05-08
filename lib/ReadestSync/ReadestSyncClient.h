#pragma once
#include <cstdint>
#include <string>

// Readest Sync API client. Covers `book_configs` — one row per (user,
// book_hash) holding reading progress and position pointers.
//
// Endpoints:
//   GET  {sync_api}/sync?since=<ms>&type=configs&book=<bh>&meta_hash=<mh>
//   POST {sync_api}/sync   body: { "configs": [ <BookConfig> ] }
//
// Both require `Authorization: Bearer <access_token>`. On 401/403 the
// caller refreshes the token and retries once.
//
// Wire-format quirks this client handles:
//   1. `progress` round-trips asymmetrically — pushed as `[cur, total]`,
//      returned as the stringified array `"[cur,total]"`.
//   2. `updatedAt` is unix-ms on push, ISO-8601 on response; both land
//      in BookConfig::updatedAtMs as int64 ms.
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

  // Field names match the camelCase wire format used on push; snake_case
  // pull responses are normalized into the same struct.
  struct BookConfig {
    std::string bookHash;     // 32-hex partial-MD5 of the file
    std::string metaHash;     // 32-hex MD5 of normalized OPF metadata
    std::string xpointer;     // XPointer position
    std::string location;     // CFI string; we send empty, may receive non-empty
    int progressCurrent = 0;  // 1-based current page
    int progressTotal = 0;    // total pages
    int64_t updatedAtMs = 0;  // unix ms — server-authoritative on echo
    bool deleted = false;     // true iff pull returned a non-null deleted_at
  };

  // GET configs. Server filter is `book_hash = bookHash OR meta_hash =
  // metaHash`, so the response may contain unrelated rows; we pick the
  // one matching `bookHash` exactly.
  // sinceMs = 0 for a full pull. maxUpdatedAtMs is the max(updated_at)
  // across returned rows (caller persists as the new cursor).
  // errMsg receives the server message on 4xx/5xx.
  static Error pullConfig(int64_t sinceMs, const std::string& bookHash, const std::string& metaHash, BookConfig* out,
                          int64_t* maxUpdatedAtMs, std::string* errMsg = nullptr);

  // POST a single-book config. cfg.updatedAtMs must be set. outAuthoritative
  // receives the server's echoed row — caller should adopt its updated_at,
  // since last-writer-wins may have left the server row as the winner.
  static Error pushConfig(const BookConfig& cfg, BookConfig* outAuthoritative, std::string* errMsg = nullptr);

  static const char* errorString(Error err);
};
