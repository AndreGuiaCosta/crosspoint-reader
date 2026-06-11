#include "ReadestJsonIO.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <string>

#include "ReadestAccountStore.h"
#include "ReadestBookCatalog.h"
#include "ReadestLibraryStore.h"

namespace ReadestJsonIO {

// ---- ReadestAccountStore ----

bool saveAccount(const ReadestAccountStore& store, const char* path) {
  JsonDocument doc;
  doc["syncApiBase"] = store.getSyncApiBaseRaw();
  doc["supabaseUrl"] = store.getSupabaseUrlRaw();
  doc["supabaseAnonKey"] = store.getSupabaseAnonKeyRaw();
  doc["userEmail"] = store.getUserEmail();
  doc["userId"] = store.getUserId();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.password);
  doc["accessToken"] = store.getAccessToken();
  doc["refreshToken"] = store.getRefreshToken();
  doc["expiresAt"] = store.getExpiresAt();
  doc["expiresIn"] = store.getExpiresIn();
  doc["lastConfigsSyncAtMs"] = store.getLastConfigsSyncAtMs();
  doc["lastSyncAtMs"] = store.getLastSyncAtMs();
  doc["lastSyncError"] = store.getLastSyncError();

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool loadAccount(ReadestAccountStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RAS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.syncApiBase = doc["syncApiBase"] | std::string("");
  store.supabaseUrl = doc["supabaseUrl"] | std::string("");
  store.supabaseAnonKey = doc["supabaseAnonKey"] | std::string("");
  store.userEmail = doc["userEmail"] | std::string("");
  store.userId = doc["userId"] | std::string("");
  {
    bool ok = false;
    store.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
    if (!ok) store.password.clear();
  }
  store.accessToken = doc["accessToken"] | std::string("");
  store.refreshToken = doc["refreshToken"] | std::string("");
  store.expiresAt = doc["expiresAt"] | static_cast<int64_t>(0);
  store.expiresIn = doc["expiresIn"] | static_cast<int64_t>(0);
  store.lastConfigsSyncAtMs = doc["lastConfigsSyncAtMs"] | static_cast<int64_t>(0);
  store.lastSyncAtMs = doc["lastSyncAtMs"] | static_cast<int64_t>(0);
  store.lastSyncError = doc["lastSyncError"] | std::string("");

  LOG_DBG("RAS", "Loaded Readest account: user=%s hasToken=%d", store.userEmail.c_str(), !store.accessToken.empty());
  return true;
}

// ---- ReadestLibraryStore ----

bool saveLibrary(const ReadestLibraryStore& store, const char* path) {
  JsonDocument doc;
  JsonObject entries = doc["entries"].to<JsonObject>();
  for (const auto& kv : store.hashToPath) {
    entries[kv.first] = kv.second;
  }
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool loadLibrary(ReadestLibraryStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RLS", "JSON parse error: %s", error.c_str());
    return false;
  }
  store.hashToPath.clear();
  JsonObjectConst entries = doc["entries"].as<JsonObjectConst>();
  for (JsonPairConst kv : entries) {
    const std::string hash(kv.key().c_str());
    const std::string path = kv.value() | std::string("");
    if (!hash.empty() && !path.empty()) {
      store.hashToPath[hash] = path;
    }
  }
  LOG_DBG("RLS", "Loaded Readest library: %u entries", static_cast<unsigned>(store.hashToPath.size()));
  return true;
}

// ---- ReadestBookCatalog ----

bool saveCatalog(const ReadestBookCatalog& cat, const char* path) {
  JsonDocument doc;
  doc["cursor_ms"] = cat.cursorMs;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& b : cat.books) {
    JsonObject obj = arr.add<JsonObject>();
    obj["hash"] = b.hash;
    obj["metaHash"] = b.metaHash;
    obj["format"] = b.format;
    obj["title"] = b.title;
    obj["sourceTitle"] = b.sourceTitle;
    obj["author"] = b.author;
    obj["progressCurrent"] = b.progressCurrent;
    obj["progressTotal"] = b.progressTotal;
    obj["uploadedAtMs"] = b.uploadedAtMs;
    obj["updatedAtMs"] = b.updatedAtMs;
  }
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool loadCatalog(ReadestBookCatalog& cat, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RBC", "JSON parse error: %s", error.c_str());
    return false;
  }
  cat.cursorMs = doc["cursor_ms"] | 0LL;
  cat.books.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  cat.books.reserve(arr.size());
  for (JsonObjectConst obj : arr) {
    ReadestStorageClient::BookRow b;
    b.hash = obj["hash"] | std::string("");
    if (b.hash.empty()) continue;
    b.metaHash = obj["metaHash"] | std::string("");
    b.format = obj["format"] | std::string("");
    b.title = obj["title"] | std::string("");
    b.sourceTitle = obj["sourceTitle"] | std::string("");
    b.author = obj["author"] | std::string("");
    b.progressCurrent = obj["progressCurrent"] | 0;
    b.progressTotal = obj["progressTotal"] | 0;
    b.uploadedAtMs = obj["uploadedAtMs"] | 0LL;
    b.updatedAtMs = obj["updatedAtMs"] | 0LL;
    cat.books.push_back(std::move(b));
  }
  LOG_DBG("RBC", "Loaded Readest catalog: %u books, cursor=%lld", static_cast<unsigned>(cat.books.size()),
          static_cast<long long>(cat.cursorMs));
  return true;
}

}  // namespace ReadestJsonIO
