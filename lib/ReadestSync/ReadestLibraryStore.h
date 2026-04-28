#pragma once
#include <map>
#include <string>

class ReadestLibraryStore;
namespace JsonSettingsIO {
bool saveReadestLibrary(const ReadestLibraryStore& store, const char* path);
bool loadReadestLibrary(ReadestLibraryStore& store, const char* json);
}  // namespace JsonSettingsIO

/**
 * Tracks which Readest cloud books have been downloaded to this device's
 * SD card. Persisted as `/.crosspoint/readest_library.json`.
 *
 * Scope is intentionally narrow: only records books the user pulled
 * through the Readest Library activity. Sideloaded EPUBs and books
 * downloaded via OPDS are out of scope — even if their book_hash happens
 * to match a Readest entry, this store doesn't know about them. The
 * marker shown in the library UI therefore reflects "I downloaded this
 * via Readest already," not "this book exists somewhere on this device."
 */
class ReadestLibraryStore {
 private:
  static ReadestLibraryStore instance;

  // book_hash → absolute SD path of the downloaded EPUB. Hash matches
  // `BookRow::hash` (32-hex partial-MD5) so lookups are O(log n) by hash.
  std::map<std::string, std::string> hashToPath;

  ReadestLibraryStore() = default;

  friend bool JsonSettingsIO::saveReadestLibrary(const ReadestLibraryStore&, const char*);
  friend bool JsonSettingsIO::loadReadestLibrary(ReadestLibraryStore&, const char*);

 public:
  ReadestLibraryStore(const ReadestLibraryStore&) = delete;
  ReadestLibraryStore& operator=(const ReadestLibraryStore&) = delete;

  static ReadestLibraryStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  // Record a successful Readest download. Persists to disk.
  void recordDownload(const std::string& bookHash, const std::string& localPath);

  // Forget an entry. Persists to disk. Used when the local file disappears
  // (user deleted via file browser) so the marker doesn't lie.
  void forget(const std::string& bookHash);

  // Empty string if not recorded.
  std::string getLocalPath(const std::string& bookHash) const;
  bool hasLocalCopy(const std::string& bookHash) const;

  size_t getCount() const { return hashToPath.size(); }

  // Drop entries whose recorded file no longer exists on SD. Called from
  // loadFromFile so the in-memory map matches reality after each boot.
  // Returns the number of entries removed.
  size_t purgeMissing();
};

#define READEST_LIB_STORE ReadestLibraryStore::getInstance()
