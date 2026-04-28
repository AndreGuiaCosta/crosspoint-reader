#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ReadestStorageClient.h"

class ReadestBookCatalog;
namespace JsonSettingsIO {
bool saveReadestCatalog(const ReadestBookCatalog& cat, const char* path);
bool loadReadestCatalog(ReadestBookCatalog& cat, const char* json);
}  // namespace JsonSettingsIO

/**
 * Cached snapshot of the user's Readest cloud library plus the delta cursor
 * we use to ask the server for incremental updates.
 *
 * Persisted at `/.crosspoint/readest_catalog.json`. On entry into the
 * Library activity we:
 *   1. load the cached catalog (full pull on first run, fast on warm cache);
 *   2. pull `since=cursor` to grab anything new/updated/deleted server-side;
 *   3. merge the delta into the catalog and advance the cursor;
 *   4. save back to disk.
 *
 * Deleted rows are dropped on merge — we never persist soft-deletes — so
 * the cached vector contains only books the server still considers live.
 * Callers should still apply `uploadedAtMs > 0 && !hash.empty()` at display
 * time since the catalog can include rows pending an upload.
 */
class ReadestBookCatalog {
 private:
  static ReadestBookCatalog instance;

  // max(updated_at) seen so far. Sent as `since=` on the next pull. 0 means
  // no successful pull yet → the next call performs a full scan.
  int64_t cursorMs = 0;

  // Cached rows, keyed by hash. We use a vector rather than a map because
  // the activity iterates in order and the catalog is small enough that
  // O(n) lookup during merges is fine.
  std::vector<ReadestStorageClient::BookRow> books;

  ReadestBookCatalog() = default;

  friend bool JsonSettingsIO::saveReadestCatalog(const ReadestBookCatalog&, const char*);
  friend bool JsonSettingsIO::loadReadestCatalog(ReadestBookCatalog&, const char*);

 public:
  ReadestBookCatalog(const ReadestBookCatalog&) = delete;
  ReadestBookCatalog& operator=(const ReadestBookCatalog&) = delete;

  static ReadestBookCatalog& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  int64_t getCursorMs() const { return cursorMs; }
  const std::vector<ReadestStorageClient::BookRow>& getBooks() const { return books; }

  // Apply a delta pulled from the server. For each row in `delta`:
  //   - deleted=true → remove the matching hash from the catalog;
  //   - else → upsert by hash.
  // Then advance the cursor to max(cursor, maxUpdatedAtMs) and persist.
  // Returns the post-merge book count.
  size_t mergeDelta(const std::vector<ReadestStorageClient::BookRow>& delta, int64_t maxUpdatedAtMs);

  // Wipe the catalog. Used when the user signs out so a future sign-in
  // doesn't see a previous user's cached library.
  void clear();
};

#define READEST_CATALOG ReadestBookCatalog::getInstance()
