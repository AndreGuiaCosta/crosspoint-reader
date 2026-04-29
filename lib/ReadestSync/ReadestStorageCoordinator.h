#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ReadestStorageClient.h"

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
