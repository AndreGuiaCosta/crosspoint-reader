#include "ReadestSyncCoordinator.h"

#include "ReadestAccountStore.h"
#include "ReadestAuthRetry.h"

namespace {
ReadestSyncClient::Error recordResult(ReadestSyncClient::Error rc, const std::string* errMsg) {
  const bool ok = (rc == ReadestSyncClient::OK);
  READEST_STORE.recordSyncResult(ok, ok ? std::string() : (errMsg ? *errMsg : std::string()));
  return rc;
}

template <typename Call>
ReadestSyncClient::Error withRefresh(const char* tag, std::string* errMsg, Call&& call) {
  return recordResult(ReadestAuthRetry::run(tag, ReadestSyncClient::AUTH_EXPIRED, errMsg, std::forward<Call>(call)),
                      errMsg);
}
}  // namespace

ReadestSyncClient::Error ReadestSyncCoordinator::pullConfigWithRefresh(int64_t sinceMs, const std::string& bookHash,
                                                                       const std::string& metaHash,
                                                                       ReadestSyncClient::BookConfig* out,
                                                                       int64_t* maxUpdatedAtMs, std::string* errMsg) {
  return withRefresh("pull", errMsg, [&] {
    return ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
  });
}

ReadestSyncClient::Error ReadestSyncCoordinator::pushConfigWithRefresh(const ReadestSyncClient::BookConfig& cfg,
                                                                       ReadestSyncClient::BookConfig* outAuthoritative,
                                                                       std::string* errMsg) {
  return withRefresh("push", errMsg, [&] { return ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg); });
}
