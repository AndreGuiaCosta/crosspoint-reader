#include "ReadestBookCatalog.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "../../src/JsonSettingsIO.h"

namespace {
constexpr char CATALOG_FILE_JSON[] = "/.crosspoint/readest_catalog.json";
}  // namespace

ReadestBookCatalog ReadestBookCatalog::instance;

bool ReadestBookCatalog::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadestCatalog(*this, CATALOG_FILE_JSON);
}

bool ReadestBookCatalog::loadFromFile() {
  cursorMs = 0;
  books.clear();
  if (!Storage.exists(CATALOG_FILE_JSON)) {
    LOG_DBG("RBC", "No readest_catalog.json — starting empty");
    return false;
  }
  String json = Storage.readFile(CATALOG_FILE_JSON);
  if (json.isEmpty()) {
    LOG_ERR("RBC", "readest_catalog.json present but empty");
    return false;
  }
  return JsonSettingsIO::loadReadestCatalog(*this, json.c_str());
}

size_t ReadestBookCatalog::mergeDelta(const std::vector<ReadestStorageClient::BookRow>& delta, int64_t maxUpdatedAtMs) {
  for (const auto& row : delta) {
    if (row.hash.empty()) continue;
    auto it = std::find_if(books.begin(), books.end(),
                           [&](const ReadestStorageClient::BookRow& b) { return b.hash == row.hash; });
    if (row.deleted) {
      if (it != books.end()) books.erase(it);
      continue;
    }
    if (it == books.end()) {
      books.push_back(row);
    } else {
      *it = row;
    }
  }
  if (maxUpdatedAtMs > cursorMs) cursorMs = maxUpdatedAtMs;
  saveToFile();
  return books.size();
}

void ReadestBookCatalog::clear() {
  cursorMs = 0;
  books.clear();
  saveToFile();
}
