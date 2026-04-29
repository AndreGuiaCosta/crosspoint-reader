#include "ReadestSyncCoordinator.h"

#include <Logging.h>

#include "ReadestAuthRefresh.h"

ReadestSyncClient::Error ReadestSyncCoordinator::pullConfigWithRefresh(int64_t sinceMs, const std::string& bookHash,
                                                                       const std::string& metaHash,
                                                                       ReadestSyncClient::BookConfig* out,
                                                                       int64_t* maxUpdatedAtMs, std::string* errMsg) {
  auto rc = ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
  if (rc != ReadestSyncClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSC", "pull AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("pull", errMsg)) return ReadestSyncClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
}

ReadestSyncClient::Error ReadestSyncCoordinator::pushConfigWithRefresh(const ReadestSyncClient::BookConfig& cfg,
                                                                       ReadestSyncClient::BookConfig* outAuthoritative,
                                                                       std::string* errMsg) {
  auto rc = ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg);
  if (rc != ReadestSyncClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSC", "push AUTH_EXPIRED, attempting refresh");
  if (!ReadestAuthRefresh::refresh("push", errMsg)) return ReadestSyncClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg);
}
