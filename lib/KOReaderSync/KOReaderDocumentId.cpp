#include "KOReaderDocumentId.h"

#include <Logging.h>
#include <MD5Builder.h>

#include "../Utils/PartialMd5.h"

namespace {
// Extract filename from path (everything after last '/')
std::string getFilename(const std::string& path) {
  const size_t pos = path.rfind('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}
}  // namespace

std::string KOReaderDocumentId::calculateFromFilename(const std::string& filePath) {
  const std::string filename = getFilename(filePath);
  if (filename.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(filename.c_str());
  md5.calculate();

  std::string result = md5.toString().c_str();
  LOG_DBG("KODoc", "Filename hash: %s (from '%s')", result.c_str(), filename.c_str());
  return result;
}

std::string KOReaderDocumentId::calculate(const std::string& filePath) {
  // KOReader's reference implementation skips samples that fail to read.
  return PartialMd5::hashFile("KODoc", filePath, PartialMd5::ReadErrorPolicy::SKIP);
}
