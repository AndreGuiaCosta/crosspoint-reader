#include "ReadestHash.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include "ReadestMetaExtractor.h"

namespace {
// Normalize a single identifier value:
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

// IdScheme values encode priority (uuid > calibre > isbn > other); pickPreferredIdentifier
// relies on this ordering.
static_assert(ReadestMetaExtractor::ID_UUID > ReadestMetaExtractor::ID_CALIBRE &&
                  ReadestMetaExtractor::ID_CALIBRE > ReadestMetaExtractor::ID_ISBN &&
                  ReadestMetaExtractor::ID_ISBN > ReadestMetaExtractor::ID_OTHER,
              "IdScheme ordering encodes identifier priority");

const ReadestMetaExtractor::Identifier* pickPreferredIdentifier(
    const std::vector<ReadestMetaExtractor::Identifier>& ids) {
  const ReadestMetaExtractor::Identifier* best = nullptr;
  ReadestMetaExtractor::IdScheme bestScheme = ReadestMetaExtractor::ID_OTHER;
  for (const auto& id : ids) {
    if (id.scheme > bestScheme) {
      best = &id;
      bestScheme = id.scheme;
    }
  }
  return best;
}
}  // namespace

std::string ReadestHash::partialMd5(const std::string& filePath) {
  HalFile file;
  if (!Storage.openFileForRead("RHSH", filePath, file)) {
    LOG_DBG("RHSH", "Failed to open file: %s", filePath.c_str());
    return "";
  }

  const size_t fileSize = file.fileSize();
  LOG_DBG("RHSH", "partialMd5 for %s (size: %zu)", filePath.c_str(), fileSize);

  SampleRange ranges[MAX_SAMPLES];
  const size_t rangeCount = partialMd5SampleRanges(fileSize, ranges);

  MD5Builder md5;
  md5.begin();

  uint8_t buffer[SAMPLE_SIZE];
  size_t totalBytesRead = 0;

  for (size_t i = 0; i < rangeCount; ++i) {
    const size_t start = ranges[i].start;
    const size_t length = ranges[i].length;

    if (!file.seekSet(start)) {
      LOG_ERR("RHSH", "Seek to %zu failed", start);
      file.close();
      return "";
    }

    // read() returns a signed int: -1 on I/O error. Keep it signed — stuffing
    // it into size_t would turn -1 into SIZE_MAX and feed md5.add a wild length.
    const int bytesRead = file.read(buffer, length);
    if (bytesRead <= 0) {
      LOG_ERR("RHSH", "Read at offset %zu returned %d", start, bytesRead);
      file.close();
      return "";
    }

    md5.add(buffer, static_cast<size_t>(bytesRead));
    totalBytesRead += static_cast<size_t>(bytesRead);
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

  // Authors joined by "," (comma, no space). Extractor pre-trims and filters empties.
  std::string authorsJoined;
  for (const auto& a : extractor.authors) {
    if (!authorsJoined.empty()) authorsJoined += ',';
    authorsJoined += a;
  }

  // NFC normalization is intentionally not applied (see header). ASCII input
  // matches the server hash exactly; non-ASCII may surface as a mismatch.
  const std::string hashSource = extractor.title + "|" + authorsJoined + "|" + identifierPart;

  MD5Builder md5;
  md5.begin();
  md5.add(hashSource.c_str());
  md5.calculate();
  std::string result = md5.toString().c_str();

  LOG_DBG("RHSH", "metaMd5 = %s (source: %s)", result.c_str(), hashSource.c_str());
  return result;
}
