#include "ReadestMetaExtractor.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

namespace {
// Trim ASCII whitespace from both ends. OPF elements often contain leading
// newlines/indentation which Readest's JS trim() strips for creator entries.
std::string trim(const std::string& s) {
  size_t begin = 0;
  while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
    begin++;
  }
  size_t end = s.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(begin, end - begin);
}

std::string toLower(const std::string& s) {
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); i++) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  }
  return out;
}

// Match a tag name with or without the "dc:" / "opf:" prefix, since different
// EPUBs use different namespace declarations.
bool tagEquals(const XML_Char* name, const char* local) {
  if (strcmp(name, local) == 0) return true;
  const size_t localLen = strlen(local);
  // "dc:title" where local is "title", etc.
  if (const char* colon = strchr(name, ':')) {
    return strcmp(colon + 1, local) == 0 && strlen(colon + 1) == localLen;
  }
  return false;
}
}  // namespace

bool ReadestMetaExtractor::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("RME", "Couldn't allocate XML parser");
    return false;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ReadestMetaExtractor::~ReadestMetaExtractor() { destroyXmlParser(parser); }

size_t ReadestMetaExtractor::write(const uint8_t data) { return write(&data, 1); }

size_t ReadestMetaExtractor::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_ERR("RME", "Couldn't allocate XML parse buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("RME", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ReadestMetaExtractor::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ReadestMetaExtractor*>(userData);

  if (self->state == START && tagEquals(name, "package")) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && tagEquals(name, "metadata")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA) {
    if (tagEquals(name, "title")) {
      // Only the first dc:title wins; the Readest hashSource treats
      // subsequent titles as subtitles and does not include them.
      if (self->title.empty()) {
        self->state = IN_TITLE;
        self->currentText.clear();
      }
      return;
    }
    if (tagEquals(name, "creator")) {
      self->state = IN_CREATOR;
      self->currentText.clear();
      return;
    }
    if (tagEquals(name, "identifier")) {
      self->state = IN_IDENTIFIER;
      self->currentText.clear();
      self->currentScheme.clear();
      // Look for opf:scheme attribute. Attribute list is [name0, val0, ..., NULL].
      for (size_t i = 0; atts[i] != nullptr; i += 2) {
        if (tagEquals(atts[i], "scheme")) {
          self->currentScheme = toLower(atts[i + 1] ? atts[i + 1] : "");
          break;
        }
      }
      return;
    }
  }
}

void XMLCALL ReadestMetaExtractor::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ReadestMetaExtractor*>(userData);
  if (self->state == IN_TITLE || self->state == IN_CREATOR || self->state == IN_IDENTIFIER) {
    self->currentText.append(s, len);
  }
}

void XMLCALL ReadestMetaExtractor::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ReadestMetaExtractor*>(userData);

  if (self->state == IN_TITLE && tagEquals(name, "title")) {
    // Readest does NOT trim the title (book.ts:311 uses it raw).
    self->title = self->currentText;
    self->state = IN_METADATA;
    self->currentText.clear();
    return;
  }

  if (self->state == IN_CREATOR && tagEquals(name, "creator")) {
    // Readest's normalizeAuthor trims, and the join filters out empty strings.
    const auto trimmed = trim(self->currentText);
    if (!trimmed.empty()) {
      self->authors.push_back(trimmed);
    }
    self->state = IN_METADATA;
    self->currentText.clear();
    return;
  }

  if (self->state == IN_IDENTIFIER && tagEquals(name, "identifier")) {
    // The value itself is not trimmed in Readest's algorithm — the
    // normalization step (strip-after-last-colon / first-colon) is what
    // deduplicates schemed IDs like "urn:isbn:..." into the bare value.
    self->identifiers.push_back({self->currentScheme, self->currentText});
    self->state = IN_METADATA;
    self->currentText.clear();
    self->currentScheme.clear();
    return;
  }

  if (self->state == IN_METADATA && tagEquals(name, "metadata")) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && tagEquals(name, "package")) {
    self->state = START;
    return;
  }
}
