#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class ReadestStorageClient {
 public:
  enum Error {
    OK = 0,
    NO_AUTH,
    BAD_REQUEST,
    AUTH_EXPIRED,
    NOT_FOUND,
    NETWORK_ERROR,
    SERVER_ERROR,
    JSON_ERROR,
  };

  struct BookRow {
    std::string hash;
    std::string metaHash;
    std::string format;
    std::string title;
    std::string sourceTitle;
    std::string author;
    int progressCurrent = 0;
    int progressTotal = 0;
    int64_t uploadedAtMs = 0;
    int64_t updatedAtMs = 0;
    // Server-assigned pull cursor (books_set_synced_at trigger). The server
    // filters books on synced_at > since, so the cursor must be derived from
    // it — updated_at is client-supplied and clock-skew can run it ahead of
    // synced_at, which would silently skip later changes. 0 when the server
    // predates the synced_at column (e.g. an older self-hosted stack).
    int64_t syncedAtMs = 0;
    bool deleted = false;
  };

  struct FileRow {
    std::string fileKey;
    int64_t fileSize = 0;
    std::string bookHash;
    int64_t updatedAtMs = 0;
  };

  static Error pullBooksSince(int64_t sinceMs, std::vector<BookRow>* out, int64_t* maxUpdatedAtMs,
                              std::string* errMsg = nullptr);

  static Error listFilesByBookHash(const std::string& bookHash, std::vector<FileRow>* out,
                                   std::string* errMsg = nullptr);

  static Error getDownloadUrls(const std::vector<std::string>& fileKeys, std::map<std::string, std::string>* outUrls,
                               std::string* errMsg = nullptr);

  static const char* errorString(Error err);
};
