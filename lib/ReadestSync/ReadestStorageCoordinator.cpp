#include "ReadestStorageCoordinator.h"

#include <Logging.h>

#include "ReadestAuthClient.h"

namespace {
// True iff the auth refresh leaves the store with a usable access token.
// Mirrors the helper in ReadestSyncCoordinator — kept local rather than
// shared since the two coordinators have no other coupling.
bool refreshOrLog(const char* tag, std::string* errMsg) {
  std::string refreshErr;
  const auto rc = ReadestAuthClient::refresh(&refreshErr);
  if (rc == ReadestAuthClient::OK) {
    LOG_DBG("RSTC", "%s refreshed token", tag);
    return true;
  }
  LOG_ERR("RSTC", "%s refresh failed: %s (%s)", tag, ReadestAuthClient::errorString(rc), refreshErr.c_str());
  if (errMsg && errMsg->empty()) *errMsg = refreshErr;
  return false;
}
}  // namespace

ReadestStorageClient::Error ReadestStorageCoordinator::pullBooksSinceWithRefresh(
    int64_t sinceMs, std::vector<ReadestStorageClient::BookRow>* out, int64_t* maxUpdatedAtMs, std::string* errMsg) {
  auto rc = ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSTC", "books AUTH_EXPIRED, attempting refresh");
  if (!refreshOrLog("books", errMsg)) return ReadestStorageClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg);
}

ReadestStorageClient::Error ReadestStorageCoordinator::listFilesByBookHashWithRefresh(
    const std::string& bookHash, std::vector<ReadestStorageClient::FileRow>* out, std::string* errMsg) {
  auto rc = ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSTC", "list AUTH_EXPIRED, attempting refresh");
  if (!refreshOrLog("list", errMsg)) return ReadestStorageClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg);
}

ReadestStorageClient::Error ReadestStorageCoordinator::getDownloadUrlsWithRefresh(
    const std::vector<std::string>& fileKeys, std::map<std::string, std::string>* outUrls, std::string* errMsg) {
  auto rc = ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg);
  if (rc != ReadestStorageClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSTC", "sign AUTH_EXPIRED, attempting refresh");
  if (!refreshOrLog("sign", errMsg)) return ReadestStorageClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg);
}
