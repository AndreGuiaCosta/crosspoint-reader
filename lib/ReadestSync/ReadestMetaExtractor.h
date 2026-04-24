#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

/**
 * Streaming OPF parser that extracts only the metadata fields needed to
 * reproduce Readest's meta-hash:
 *   - dc:title (first one)
 *   - dc:creator values (all, in order)
 *   - dc:identifier values with their opf:scheme attribute (all, in order)
 *
 * Other dc:* fields and the manifest/spine are ignored — the full OPF has
 * already been parsed into CrossPoint's BookMetadataCache by this point;
 * this parser re-reads the OPF bytes only when the user invokes Readest
 * Sync, to avoid a book.bin format bump just for identifier fields.
 *
 * Known V1 limitation: EPUB 3 refines-based altIdentifier precedence
 * (`<meta refines="#id" property="alternate-script">`) is not implemented.
 */
class ReadestMetaExtractor final : public Print {
 public:
  struct Identifier {
    std::string scheme;  // Lowercase. Empty if no opf:scheme attribute.
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
  // Scheme attribute of the currently-open dc:identifier, lowercased.
  std::string currentScheme;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);
};
