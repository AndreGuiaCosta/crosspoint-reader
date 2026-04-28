#include "ReadestSyncCoordinator.h"

#include <Logging.h>

#include "ReadestAuthClient.h"

namespace {
// True iff the auth refresh call leaves the store with a usable access
// token. Any other outcome means we cannot retry — surface AUTH_EXPIRED
// up so the activity prompts the user to sign in again.
bool refreshOrLog(const char* tag, std::string* errMsg) {
  std::string refreshErr;
  const auto rc = ReadestAuthClient::refresh(&refreshErr);
  if (rc == ReadestAuthClient::OK) {
    LOG_DBG("RSC", "%s refreshed token", tag);
    return true;
  }
  LOG_ERR("RSC", "%s refresh failed: %s (%s)", tag, ReadestAuthClient::errorString(rc), refreshErr.c_str());
  if (errMsg && errMsg->empty())
    *errMsg = refreshErr;  // Bubble refresh-side detail if push/pull didn't already set it.
  return false;
}
}  // namespace

ReadestSyncClient::Error ReadestSyncCoordinator::pullConfigWithRefresh(int64_t sinceMs, const std::string& bookHash,
                                                                       const std::string& metaHash,
                                                                       ReadestSyncClient::BookConfig* out,
                                                                       int64_t* maxUpdatedAtMs, std::string* errMsg) {
  auto rc = ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
  if (rc != ReadestSyncClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSC", "pull AUTH_EXPIRED, attempting refresh");
  if (!refreshOrLog("pull", errMsg)) return ReadestSyncClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestSyncClient::pullConfig(sinceMs, bookHash, metaHash, out, maxUpdatedAtMs, errMsg);
}

ReadestSyncClient::Error ReadestSyncCoordinator::pushConfigWithRefresh(const ReadestSyncClient::BookConfig& cfg,
                                                                       ReadestSyncClient::BookConfig* outAuthoritative,
                                                                       std::string* errMsg) {
  auto rc = ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg);
  if (rc != ReadestSyncClient::AUTH_EXPIRED) return rc;
  LOG_DBG("RSC", "push AUTH_EXPIRED, attempting refresh");
  if (!refreshOrLog("push", errMsg)) return ReadestSyncClient::AUTH_EXPIRED;
  if (errMsg) errMsg->clear();
  return ReadestSyncClient::pushConfig(cfg, outAuthoritative, errMsg);
}
