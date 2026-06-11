#include "ReadestBookCatalog.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "ReadestJsonIO.h"

namespace {
constexpr char CATALOG_FILE_JSON[] = "/.crosspoint/readest_catalog.json";
}  // namespace

ReadestBookCatalog ReadestBookCatalog::instance;

bool ReadestBookCatalog::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return ReadestJsonIO::saveCatalog(*this, CATALOG_FILE_JSON);
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
  return ReadestJsonIO::loadCatalog(*this, json.c_str());
}

size_t ReadestBookCatalog::mergeDelta(const std::vector<ReadestStorageClient::BookRow>& delta, int64_t maxUpdatedAtMs) {
  if (delta.empty() && maxUpdatedAtMs <= cursorMs) return books.size();

  if (books.empty()) {
    // First (full) pull — the per-row find_if below would be quadratic in
    // library size here; an empty catalog can just take the delta wholesale.
    books.reserve(delta.size());
    for (const auto& row : delta) {
      if (!row.hash.empty() && !row.deleted) books.push_back(row);
    }
  } else {
    // Incremental pulls carry only changed rows, so a linear probe per row
    // is fine.
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
