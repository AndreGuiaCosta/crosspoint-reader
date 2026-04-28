#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ReadestStorageClient.h"

/**
 * Auth-aware wrapper around ReadestStorageClient. Mirrors
 * ReadestSyncCoordinator: each helper makes one storage call, and on
 * AUTH_EXPIRED (HTTP 401/403) it calls ReadestAuthClient::refresh and
 * retries the original call exactly once.
 *
 * Activities and the boot probe call these instead of the bare client so
 * the lazy-refresh logic stays in one place. Errors other than
 * AUTH_EXPIRED are forwarded unchanged. If the retry itself returns
 * AUTH_EXPIRED, AUTH_EXPIRED is propagated and the caller should treat it
 * as "user must sign in again."
 */
namespace ReadestStorageCoordinator {
ReadestStorageClient::Error pullBooksSinceWithRefresh(int64_t sinceMs, std::vector<ReadestStorageClient::BookRow>* out,
                                                      int64_t* maxUpdatedAtMs, std::string* errMsg = nullptr);

ReadestStorageClient::Error listFilesByBookHashWithRefresh(const std::string& bookHash,
                                                           std::vector<ReadestStorageClient::FileRow>* out,
                                                           std::string* errMsg = nullptr);

ReadestStorageClient::Error getDownloadUrlsWithRefresh(const std::vector<std::string>& fileKeys,
                                                       std::map<std::string, std::string>* outUrls,
                                                       std::string* errMsg = nullptr);
}  // namespace ReadestStorageCoordinator
