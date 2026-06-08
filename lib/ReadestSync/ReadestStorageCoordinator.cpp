#include "ReadestStorageCoordinator.h"

#include <Logging.h>

#include "ReadestAuthRefresh.h"

ReadestStorageClient::Error ReadestStorageCoordinator::pullBooksSinceWithRefresh(
    int64_t sinceMs, std::vector<ReadestStorageClient::BookRow>* out, int64_t* maxUpdatedAtMs, std::string* errMsg) {
  auto rc = ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSTC", "books AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("books", errMsg)) return ReadestStorageClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg);
}

ReadestStorageClient::Error ReadestStorageCoordinator::listFilesByBookHashWithRefresh(
    const std::string& bookHash, std::vector<ReadestStorageClient::FileRow>* out, std::string* errMsg) {
  auto rc = ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSTC", "list AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("list", errMsg)) return ReadestStorageClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg);
}

ReadestStorageClient::Error ReadestStorageCoordinator::getDownloadUrlsWithRefresh(
    const std::vector<std::string>& fileKeys, std::map<std::string, std::string>* outUrls, std::string* errMsg) {
  auto rc = ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSTC", "sign AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("sign", errMsg)) return ReadestStorageClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg);
}
