#include "ReadestStorageCoordinator.h"

#include "ReadestAccountStore.h"
#include "ReadestAuthRetry.h"

namespace {
// Mirrors ReadestSyncCoordinator: library operations surface in the settings
// "Last sync"/"Last error" display too, not only progress sync.
ReadestStorageClient::Error recordResult(ReadestStorageClient::Error rc, const std::string* errMsg) {
  const bool ok = (rc == ReadestStorageClient::OK);
  READEST_STORE.recordSyncResult(ok, ok ? std::string() : (errMsg ? *errMsg : std::string()));
  return rc;
}

template <typename Call>
ReadestStorageClient::Error withRefresh(const char* tag, std::string* errMsg, Call&& call) {
  return recordResult(ReadestAuthRetry::run(tag, ReadestStorageClient::AUTH_EXPIRED, errMsg, std::forward<Call>(call)),
                      errMsg);
}
}  // namespace

ReadestStorageClient::Error ReadestStorageCoordinator::pullBooksSinceWithRefresh(
    int64_t sinceMs, std::vector<ReadestStorageClient::BookRow>* out, int64_t* maxUpdatedAtMs, std::string* errMsg) {
  return withRefresh("books", errMsg,
                     [&] { return ReadestStorageClient::pullBooksSince(sinceMs, out, maxUpdatedAtMs, errMsg); });
}

ReadestStorageClient::Error ReadestStorageCoordinator::listFilesByBookHashWithRefresh(
    const std::string& bookHash, std::vector<ReadestStorageClient::FileRow>* out, std::string* errMsg) {
  return withRefresh("list", errMsg, [&] { return ReadestStorageClient::listFilesByBookHash(bookHash, out, errMsg); });
}

ReadestStorageClient::Error ReadestStorageCoordinator::getDownloadUrlsWithRefresh(
    const std::vector<std::string>& fileKeys, std::map<std::string, std::string>* outUrls, std::string* errMsg) {
  return withRefresh("sign", errMsg, [&] { return ReadestStorageClient::getDownloadUrls(fileKeys, outUrls, errMsg); });
}
