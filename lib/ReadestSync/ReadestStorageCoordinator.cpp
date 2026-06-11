#include "ReadestStorageCoordinator.h"

#include <Logging.h>

#include "ReadestAccountStore.h"
#include "ReadestAuthRefresh.h"

namespace {
// Mirrors ReadestSyncCoordinator: library operations surface in the settings
// "Last sync"/"Last error" display too, not only progress sync.
ReadestStorageClient::Error recordResult(ReadestStorageClient::Error rc, const std::string* errMsg) {
  const bool ok = (rc == ReadestStorageClient::OK);
  READEST_STORE.recordSyncResult(ok, ok ? std::string() : (errMsg ? *errMsg : std::string()));
  return rc;
}
}  // namespace

ReadestStorageClient::Error ReadestStorageCoordinator::pullBooksSinceWithRefresh(
    int64_t sinceMs, std::vector<ReadestStorageClient::BookRow>* out, int64_t* maxUpdatedAtMs, std::string* errMsg) {
  auto rc = ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return recordResult(rc, errMsg);
  LOG_DBG("RSTC", "books AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("books", errMsg))
    return recordResult(ReadestStorageClient::AUTH_EXPIRED, errMsg);
  if (errMsg) errMsg->clear();
  return recordResult(ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg), errMsg);
}

ReadestStorageClient::Error ReadestStorageCoordinator::listFilesByBookHashWithRefresh(
    const std::string& bookHash, std::vector<ReadestStorageClient::FileRow>* out, std::string* errMsg) {
  auto rc = ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return recordResult(rc, errMsg);
  LOG_DBG("RSTC", "list AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("list", errMsg))
    return recordResult(ReadestStorageClient::AUTH_EXPIRED, errMsg);
  if (errMsg) errMsg->clear();
  return recordResult(ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg), errMsg);
}

ReadestStorageClient::Error ReadestStorageCoordinator::getDownloadUrlsWithRefresh(
    const std::vector<std::string>& fileKeys, std::map<std::string, std::string>* outUrls, std::string* errMsg) {
  auto rc = ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return recordResult(rc, errMsg);
  LOG_DBG("RSTC", "sign AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("sign", errMsg))
    return recordResult(ReadestStorageClient::AUTH_EXPIRED, errMsg);
  if (errMsg) errMsg->clear();
  return recordResult(ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg), errMsg);
}
