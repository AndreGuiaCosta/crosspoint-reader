#pragma once
#include <map>
#include <string>

class ReadestLibraryStore;
namespace JsonSettingsIO {
bool saveReadestLibrary(const ReadestLibraryStore& store, const char* path);
bool loadReadestLibrary(ReadestLibraryStore& store, const char* json);
}  // namespace JsonSettingsIO

class ReadestLibraryStore {
 private:
  static ReadestLibraryStore instance;
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

  void recordDownload(const std::string& bookHash, const std::string& localPath);
  void forget(const std::string& bookHash);

  std::string getLocalPath(const std::string& bookHash) const;
  bool hasLocalCopy(const std::string& bookHash) const;
  size_t getCount() const { return hashToPath.size(); }
  size_t purgeMissing();
};

#define READEST_LIB_STORE ReadestLibraryStore::getInstance()
