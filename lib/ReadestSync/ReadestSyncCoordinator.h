#pragma once
#include <cstdint>
#include <string>

#include "ReadestSyncClient.h"

// Auth-aware wrapper around ReadestSyncClient: on AUTH_EXPIRED, refresh the
// token and retry once. Other errors forward unchanged. A retry that still
// returns AUTH_EXPIRED is propagated (caller should prompt sign-in).
namespace ReadestSyncCoordinator {
ReadestSyncClient::Error pullConfigWithRefresh(int64_t sinceMs, const std::string& bookHash,
                                               const std::string& metaHash, ReadestSyncClient::BookConfig* out,
                                               int64_t* maxUpdatedAtMs, std::string* errMsg = nullptr);

ReadestSyncClient::Error pushConfigWithRefresh(const ReadestSyncClient::BookConfig& cfg,
                                               ReadestSyncClient::BookConfig* outAuthoritative,
                                               std::string* errMsg = nullptr);
}  // namespace ReadestSyncCoordinator
