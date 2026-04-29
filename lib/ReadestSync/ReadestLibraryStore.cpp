#include "ReadestLibraryStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "../../src/JsonSettingsIO.h"

namespace {
constexpr char LIBRARY_FILE_JSON[] = "/.crosspoint/readest_library.json";
}  // namespace

ReadestLibraryStore ReadestLibraryStore::instance;

bool ReadestLibraryStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadestLibrary(*this, LIBRARY_FILE_JSON);
}

bool ReadestLibraryStore::loadFromFile() {
  hashToPath.clear();
  if (!Storage.exists(LIBRARY_FILE_JSON)) {
    LOG_DBG("RLS", "No readest_library.json — starting empty");
    return false;
  }
  String json = Storage.readFile(LIBRARY_FILE_JSON);
  if (json.isEmpty()) {
    LOG_ERR("RLS", "readest_library.json present but empty");
    return false;
  }
  if (!JsonSettingsIO::loadReadestLibrary(*this, json.c_str())) return false;
  if (purgeMissing() > 0) saveToFile();
  return true;
}

void ReadestLibraryStore::recordDownload(const std::string& bookHash, const std::string& localPath) {
  if (bookHash.empty() || localPath.empty()) return;
  hashToPath[bookHash] = localPath;
  LOG_DBG("RLS", "Recorded download: %.8s… → %s", bookHash.c_str(), localPath.c_str());
  saveToFile();
}

void ReadestLibraryStore::forget(const std::string& bookHash) {
  if (hashToPath.erase(bookHash) > 0) {
    LOG_DBG("RLS", "Forgot %.8s…", bookHash.c_str());
    saveToFile();
  }
}

std::string ReadestLibraryStore::getLocalPath(const std::string& bookHash) const {
  const auto it = hashToPath.find(bookHash);
  return it == hashToPath.end() ? std::string() : it->second;
}

bool ReadestLibraryStore::hasLocalCopy(const std::string& bookHash) const {
  return hashToPath.find(bookHash) != hashToPath.end();
}

size_t ReadestLibraryStore::purgeMissing() {
  size_t purged = 0;
  for (auto it = hashToPath.begin(); it != hashToPath.end();) {
    if (Storage.exists(it->second.c_str())) {
      ++it;
    } else {
      LOG_DBG("RLS", "Purging stale entry: %.8s… (file %s gone)", it->first.c_str(), it->second.c_str());
      it = hashToPath.erase(it);
      ++purged;
    }
  }
  return purged;
}
