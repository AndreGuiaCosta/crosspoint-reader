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

class ReadestBookCatalog {
 private:
  static ReadestBookCatalog instance;

  int64_t cursorMs = 0;
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

  size_t mergeDelta(const std::vector<ReadestStorageClient::BookRow>& delta, int64_t maxUpdatedAtMs);
  void clear();
};

#define READEST_CATALOG ReadestBookCatalog::getInstance()
