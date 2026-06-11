#pragma once

#include <expat.h>

#include <cstring>

// Safely tear down an expat parser: stop processing, clear callbacks, free, and null the pointer.
inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_StopParser(parser, XML_FALSE);
  XML_SetElementHandler(parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  parser = nullptr;
}

// Match an XML tag's local name, accepting any namespace prefix or none.
// Example: tagEquals(name, "metadata") matches <metadata>, <opf:metadata>, <dc:metadata>, ...
// Use when the parser doesn't care which namespace the element is in.
inline bool tagEquals(const XML_Char* name, const char* local) {
  const char* colon = strchr(name, ':');
  return strcmp(colon ? colon + 1 : name, local) == 0;
}
