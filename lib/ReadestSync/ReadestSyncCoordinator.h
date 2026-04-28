#pragma once
#include <cstdint>
#include <string>

#include "ReadestSyncClient.h"

/**
 * Auth-aware wrapper around ReadestSyncClient. Each helper makes one sync
 * call; on AUTH_EXPIRED (HTTP 401/403) it calls ReadestAuthClient::refresh
 * and retries the original call exactly once. This is the lazy-refresh
 * strategy documented in `ReadestAccountStore::needsRefresh()`: cheaper
 * than proactive half-life refresh given Readest issues 7-day tokens.
 *
 * The activity / probes call these instead of the bare client so the retry
 * logic stays in one place. Errors other than AUTH_EXPIRED are forwarded
 * unchanged. If the retry itself returns AUTH_EXPIRED (refresh succeeded
 * server-side but the new token still gets rejected — should not happen
 * in practice), AUTH_EXPIRED is propagated and the caller should treat it
 * as "user must sign in again."
 */
namespace ReadestSyncCoordinator {
ReadestSyncClient::Error pullConfigWithRefresh(int64_t sinceMs, const std::string& bookHash,
                                               const std::string& metaHash, ReadestSyncClient::BookConfig* out,
                                               int64_t* maxUpdatedAtMs, std::string* errMsg = nullptr);

ReadestSyncClient::Error pushConfigWithRefresh(const ReadestSyncClient::BookConfig& cfg,
                                               ReadestSyncClient::BookConfig* outAuthoritative,
                                               std::string* errMsg = nullptr);
}  // namespace ReadestSyncCoordinator
