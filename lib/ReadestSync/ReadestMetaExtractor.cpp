#include "ReadestMetaExtractor.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

namespace {
// Trim ASCII whitespace from both ends. Creator entries in the OPF often
// have leading newlines/indentation that must be stripped.
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

// Classify an opf:scheme attribute (case-insensitive substring match) into
// the priority byte. Avoids carrying the scheme string per-identifier.
ReadestMetaExtractor::IdScheme classifyScheme(const XML_Char* s) {
  if (!s) return ReadestMetaExtractor::ID_OTHER;
  std::string lower;
  lower.reserve(strlen(s));
  for (const XML_Char* p = s; *p; ++p) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
  }
  if (lower.find("uuid") != std::string::npos) return ReadestMetaExtractor::ID_UUID;
  if (lower.find("calibre") != std::string::npos) return ReadestMetaExtractor::ID_CALIBRE;
  if (lower.find("isbn") != std::string::npos) return ReadestMetaExtractor::ID_ISBN;
  return ReadestMetaExtractor::ID_OTHER;
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
      // Only the first dc:title wins; subsequent ones are subtitles.
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
      self->currentScheme = ID_OTHER;
      for (size_t i = 0; atts[i] != nullptr; i += 2) {
        if (tagEquals(atts[i], "scheme")) {
          self->currentScheme = classifyScheme(atts[i + 1]);
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
    // Title is used raw, NOT trimmed.
    self->title = self->currentText;
    self->state = IN_METADATA;
    self->currentText.clear();
    return;
  }

  if (self->state == IN_CREATOR && tagEquals(name, "creator")) {
    // Trim and skip empty entries before joining.
    const auto trimmed = trim(self->currentText);
    if (!trimmed.empty()) {
      self->authors.push_back(trimmed);
    }
    self->state = IN_METADATA;
    self->currentText.clear();
    return;
  }

  if (self->state == IN_IDENTIFIER && tagEquals(name, "identifier")) {
    // Value is NOT trimmed; normalization (strip-after-colon) handles
    // schemed IDs like "urn:isbn:...".
    self->identifiers.push_back({self->currentScheme, self->currentText});
    self->state = IN_METADATA;
    self->currentText.clear();
    self->currentScheme = ID_OTHER;
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
