#pragma once

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class KOReaderCredentialStore;
class ReadestAccountStore;
class ReadestLibraryStore;
class ReadestBookCatalog;
class RecentBooksStore;
class OpdsServerStore;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);

// ReadestAccountStore
bool saveReadest(const ReadestAccountStore& store, const char* path);
bool loadReadest(ReadestAccountStore& store, const char* json);

// ReadestLibraryStore
bool saveReadestLibrary(const ReadestLibraryStore& store, const char* path);
bool loadReadestLibrary(ReadestLibraryStore& store, const char* json);

// ReadestBookCatalog
bool saveReadestCatalog(const ReadestBookCatalog& cat, const char* path);
bool loadReadestCatalog(ReadestBookCatalog& cat, const char* json);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

// OpdsServerStore
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave = nullptr);

}  // namespace JsonSettingsIO
