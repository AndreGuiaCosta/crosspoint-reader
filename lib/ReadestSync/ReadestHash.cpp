#include "ReadestHash.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <algorithm>
#include <cctype>

#include "ReadestMetaExtractor.h"

namespace {
// One sample is 1 KiB (matches Readest's `sample = 1024`).
constexpr size_t SAMPLE_SIZE = 1024;

// Offsets the Readest JS code uses. For i = -1: 1024 >> 2 = 256. For
// i = 0..10: 1024 << (2 * i) = 1024, 4096, 16384, 65536, 262144, 1048576,
// 4194304, 16777216, 67108864, 268435456, 1073741824 (1 GiB).
size_t offsetForIndex(int i) {
  if (i < 0) {
    return SAMPLE_SIZE >> (2 * -i);
  }
  return SAMPLE_SIZE << (2 * i);
}

// Returns true if any byte in `s` has the high bit set (i.e. non-ASCII).
// Used to log a warning about skipped NFC normalization — CJK and
// accented titles may hash differently than Readest produces.
bool containsNonAscii(const std::string& s) {
  for (const auto c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) return true;
  }
  return false;
}

// Normalize a single identifier value per Readest's logic
// (apps/readest-app/src/utils/book.ts:261-274):
//   - if contains "urn:" anywhere, take everything after the LAST ":"
//   - else if contains any ":", take everything after the FIRST ":"
//   - else pass through unchanged
std::string normalizeIdentifier(const std::string& value) {
  if (value.find("urn:") != std::string::npos) {
    const size_t lastColon = value.rfind(':');
    if (lastColon != std::string::npos) {
      return value.substr(lastColon + 1);
    }
  }
  const size_t firstColon = value.find(':');
  if (firstColon != std::string::npos) {
    return value.substr(firstColon + 1);
  }
  return value;
}

// Pick the preferred identifier per Readest's priority order: uuid > calibre > isbn.
// Matching is done against the opf:scheme attribute (case-insensitive substring).
// Returns a pointer to the selected Identifier, or nullptr if none of the
// preferred schemes are present.
const ReadestMetaExtractor::Identifier* pickPreferredIdentifier(
    const std::vector<ReadestMetaExtractor::Identifier>& ids) {
  static const char* const PREFERENCE[] = {"uuid", "calibre", "isbn"};
  for (const char* preferred : PREFERENCE) {
    for (const auto& id : ids) {
      if (id.scheme.find(preferred) != std::string::npos) {
        return &id;
      }
    }
  }
  return nullptr;
}
}  // namespace

std::string ReadestHash::partialMd5(const std::string& filePath) {
  FsFile file;
  if (!Storage.openFileForRead("RHSH", filePath, file)) {
    LOG_DBG("RHSH", "Failed to open file: %s", filePath.c_str());
    return "";
  }

  const size_t fileSize = file.fileSize();
  LOG_DBG("RHSH", "partialMd5 for %s (size: %zu)", filePath.c_str(), fileSize);

  MD5Builder md5;
  md5.begin();

  uint8_t buffer[SAMPLE_SIZE];
  size_t totalBytesRead = 0;

  for (int i = -1; i <= 10; i++) {
    const size_t start = offsetForIndex(i);

    // Matches the JS `if (start >= size) break;` after clamping.
    // Breaking (not continuing) is important: larger i values produce
    // strictly larger offsets, so there's no point trying them.
    if (start >= fileSize) {
      break;
    }

    if (!file.seekSet(start)) {
      LOG_ERR("RHSH", "Seek to %zu failed", start);
      file.close();
      return "";
    }

    const size_t bytesToRead = std::min(SAMPLE_SIZE, fileSize - start);
    const size_t bytesRead = file.read(buffer, bytesToRead);
    if (bytesRead == 0) {
      LOG_ERR("RHSH", "Read at offset %zu returned 0 bytes", start);
      file.close();
      return "";
    }

    md5.add(buffer, bytesRead);
    totalBytesRead += bytesRead;
  }

  file.close();

  md5.calculate();
  std::string result = md5.toString().c_str();
  LOG_DBG("RHSH", "partialMd5 = %s (from %zu bytes sampled)", result.c_str(), totalBytesRead);
  return result;
}

std::string ReadestHash::metaMd5(const Epub& epub) {
  std::string opfHref;
  if (!epub.findContentOpfFile(&opfHref)) {
    LOG_ERR("RHSH", "metaMd5: could not resolve OPF path");
    return "";
  }

  size_t opfSize = 0;
  if (!epub.getItemSize(opfHref, &opfSize) || opfSize == 0) {
    LOG_ERR("RHSH", "metaMd5: could not size OPF item");
    return "";
  }

  ReadestMetaExtractor extractor(opfSize);
  if (!extractor.setup()) {
    return "";
  }
  if (!epub.readItemContentsToStream(opfHref, extractor, 512)) {
    LOG_ERR("RHSH", "metaMd5: failed to stream OPF");
    return "";
  }

  // Build the identifier portion of the hash source per the prefer-scheme
  // rules. If any preferred scheme (uuid/calibre/isbn) is present, only
  // that identifier contributes; otherwise all identifiers are joined by ",".
  std::string identifierPart;
  if (const auto* preferred = pickPreferredIdentifier(extractor.identifiers)) {
    identifierPart = normalizeIdentifier(preferred->value);
  } else {
    for (const auto& id : extractor.identifiers) {
      if (!identifierPart.empty()) identifierPart += ',';
      identifierPart += normalizeIdentifier(id.value);
    }
  }

  // Authors joined by "," (comma, no space). Readest's normalizeAuthor
  // already trimmed each; the extractor also trimmed, and empty entries
  // are filtered out at extraction time.
  std::string authorsJoined;
  for (const auto& a : extractor.authors) {
    if (!authorsJoined.empty()) authorsJoined += ',';
    authorsJoined += a;
  }

  const std::string hashSource = extractor.title + "|" + authorsJoined + "|" + identifierPart;

  if (containsNonAscii(hashSource)) {
    LOG_ERR("RHSH", "metaMd5: non-ASCII input — NFC normalization skipped; hash may not match Readest");
  }

  MD5Builder md5;
  md5.begin();
  md5.add(hashSource.c_str());
  md5.calculate();
  std::string result = md5.toString().c_str();

  LOG_DBG("RHSH", "metaMd5 = %s (source: %s)", result.c_str(), hashSource.c_str());
  return result;
}
