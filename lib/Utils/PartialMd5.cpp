#include "PartialMd5.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

namespace PartialMd5 {

std::string hashFile(const char* tag, const std::string& filePath, ReadErrorPolicy policy) {
  FsFile file;
  if (!Storage.openFileForRead(tag, filePath, file)) {
    LOG_DBG(tag, "Failed to open file: %s", filePath.c_str());
    return "";
  }

  const size_t fileSize = file.fileSize();
  LOG_DBG(tag, "partial MD5 for %s (size: %zu)", filePath.c_str(), fileSize);

  SampleRange ranges[MAX_SAMPLES];
  const size_t rangeCount = sampleRanges(fileSize, ranges);

  MD5Builder md5;
  md5.begin();

  uint8_t buffer[SAMPLE_SIZE];
  size_t totalBytesRead = 0;

  for (size_t i = 0; i < rangeCount; ++i) {
    const size_t start = ranges[i].start;
    const size_t length = ranges[i].length;

    if (!file.seekSet(start)) {
      LOG_ERR(tag, "Seek to %zu failed", start);
      if (policy == ReadErrorPolicy::ABORT) {
        file.close();
        return "";
      }
      continue;
    }

    // read() returns a signed int: -1 on I/O error. Keep it signed — stuffing
    // it into size_t would turn -1 into SIZE_MAX and feed md5.add a wild length.
    const int bytesRead = file.read(buffer, length);
    if (bytesRead <= 0) {
      LOG_ERR(tag, "Read at offset %zu returned %d", start, bytesRead);
      if (policy == ReadErrorPolicy::ABORT) {
        file.close();
        return "";
      }
      continue;
    }

    md5.add(buffer, static_cast<size_t>(bytesRead));
    totalBytesRead += static_cast<size_t>(bytesRead);
  }

  file.close();

  md5.calculate();
  std::string result = md5.toString().c_str();
  LOG_DBG(tag, "partial MD5 = %s (from %zu bytes sampled)", result.c_str(), totalBytesRead);
  return result;
}

}  // namespace PartialMd5
