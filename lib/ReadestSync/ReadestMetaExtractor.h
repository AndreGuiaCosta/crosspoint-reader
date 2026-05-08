#pragma once
#include <Print.h>
#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

// Streaming OPF parser that extracts the fields needed for the meta-hash:
//   - dc:title (first only)
//   - dc:creator values (all, in order)
//   - dc:identifier values with their opf:scheme attribute (all, in order)
//
// EPUB 3 refines-based altIdentifier precedence is not implemented.
class ReadestMetaExtractor final : public Print {
 public:
  // Higher value = higher preference. Encoded as a byte to avoid carrying
  // the scheme string per-identifier.
  enum IdScheme : uint8_t { ID_OTHER = 0, ID_ISBN = 1, ID_CALIBRE = 2, ID_UUID = 3 };

  struct Identifier {
    IdScheme scheme = ID_OTHER;
    std::string value;
  };

  std::string title;
  std::vector<std::string> authors;
  std::vector<Identifier> identifiers;

  explicit ReadestMetaExtractor(size_t xmlSize) : remainingSize(xmlSize) {}
  ~ReadestMetaExtractor() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;

 private:
  enum State { START, IN_PACKAGE, IN_METADATA, IN_TITLE, IN_CREATOR, IN_IDENTIFIER };

  State state = START;
  XML_Parser parser = nullptr;
  size_t remainingSize;

  // Accumulator for character data of the currently-open dc:* element.
  std::string currentText;
  // Classified scheme of the currently-open dc:identifier.
  IdScheme currentScheme = ID_OTHER;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);
};
