#pragma once

class ReadestAccountStore;
class ReadestLibraryStore;
class ReadestBookCatalog;

// Lib-local JSON persistence for the Readest stores (mirrors
// lib/KOReaderSync/KOReaderJsonIO). Keeps serialization next to the store it
// serializes so lib/ReadestSync has no dependency on src/.
namespace ReadestJsonIO {

// ReadestAccountStore — /.crosspoint/readest.json
bool saveAccount(const ReadestAccountStore& store, const char* path);
bool loadAccount(ReadestAccountStore& store, const char* json);

// ReadestLibraryStore — /.crosspoint/readest_library.json
bool saveLibrary(const ReadestLibraryStore& store, const char* path);
bool loadLibrary(ReadestLibraryStore& store, const char* json);

// ReadestBookCatalog — /.crosspoint/readest_catalog.json
bool saveCatalog(const ReadestBookCatalog& cat, const char* path);
bool loadCatalog(ReadestBookCatalog& cat, const char* json);

}  // namespace ReadestJsonIO
