#include "ReadestSyncCoordinator.h"

#include <Logging.h>

#include "ReadestAccountStore.h"
#include "ReadestAuthRefresh.h"

namespace {
void recordResult(ReadestSyncClient::Error rc, const std::string* errMsg) {
  const bool ok = (rc == ReadestSyncClient::OK);
  READEST_STORE.recordSyncResult(ok, ok ? std::string() : (errMsg ? *errMsg : std::string()));
}
}  // namespace

ReadestSyncClient::Error ReadestSyncCoordinator::pullConfigWithRefresh(int64_t sinceMs, const std::string& bookHash,
                                                                       const std::string& metaHash,
                                                                       ReadestSyncClient::BookConfig* out,
                                                                       int64_t* maxUpdatedAtMs, std::string* errMsg) {
  auto rc = ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
  if (rc != ReadestSyncClient::AUTH_EXPIRED) {
    recordResult(rc, errMsg);
    return rc;
  }
  LOG_DBG("RSC", "pull AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("pull", errMsg)) {
    recordResult(ReadestSyncClient::AUTH_EXPIRED, errMsg);
    return ReadestSyncClient::AUTH_EXPIRED;
  }
  if (errMsg) errMsg->clear();
  rc = ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
  recordResult(rc, errMsg);
  return rc;
}

ReadestSyncClient::Error ReadestSyncCoordinator::pushConfigWithRefresh(const ReadestSyncClient::BookConfig& cfg,
                                                                       ReadestSyncClient::BookConfig* outAuthoritative,
                                                                       std::string* errMsg) {
  auto rc = ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg);
  if (rc != ReadestSyncClient::AUTH_EXPIRED) {
    recordResult(rc, errMsg);
    return rc;
  }
  LOG_DBG("RSC", "push AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("push", errMsg)) {
    recordResult(ReadestSyncClient::AUTH_EXPIRED, errMsg);
    return ReadestSyncClient::AUTH_EXPIRED;
  }
  if (errMsg) errMsg->clear();
  rc = ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg);
  recordResult(rc, errMsg);
  return rc;
}
