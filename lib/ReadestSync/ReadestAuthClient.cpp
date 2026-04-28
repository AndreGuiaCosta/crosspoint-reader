#include "ReadestAuthClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

#include <memory>

#include "ReadestAccountStore.h"

namespace {
// Timeouts in milliseconds, matching the koplugin reference (handoff §4.3).
constexpr int SIGN_IN_CONNECT_TIMEOUT = 5000;
constexpr int SIGN_IN_READ_TIMEOUT = 10000;
constexpr int REFRESH_CONNECT_TIMEOUT = 3000;
constexpr int REFRESH_READ_TIMEOUT = 7000;
constexpr int LOGOUT_CONNECT_TIMEOUT = 3000;
constexpr int LOGOUT_READ_TIMEOUT = 7000;

// Production path installs the Mozilla CA bundle; simulator path uses
// setInsecure(). The branch lives in lib/TestHooks/ReadestTlsConfig.h so
// this file stays free of build-mode conditionals.
void configureTls(WiFiClientSecure& client) { ReadestTls::configure(client); }

// Pull `error_description` (preferred) or `msg` from a Supabase error body.
// Mutates errMsg only if the input parses and one of the fields is present.
//
// Pass length explicitly to deserializeJson — Arduino-ESP32's `String` has a
// dedicated ArduinoJson reader, but the simulator's lookalike does not, and
// the auto-detected reader stops short of the full payload. Going through
// (c_str, length) is identical on both targets and dodges the ambiguity.
void extractErrorMessage(const String& body, std::string* errMsg) {
  if (!errMsg) return;
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length()) != DeserializationError::Ok) return;
  std::string msg = doc["error_description"] | std::string("");
  if (msg.empty()) msg = doc["msg"] | std::string("");
  if (msg.empty()) msg = doc["error"] | std::string("");
  if (!msg.empty()) *errMsg = std::move(msg);
}

ReadestAuthClient::Error mapHttpStatus(int code) {
  using E = ReadestAuthClient::Error;
  if (code == 200) return E::OK;
  if (code == 400 || code == 401) return E::INVALID_CREDENTIALS;
  if (code == 403) return E::FORBIDDEN;
  if (code >= 500) return E::SERVER_ERROR;
  if (code < 0) return E::NETWORK_ERROR;
  return E::SERVER_ERROR;
}

// Apply session fields from a successful token response to READEST_STORE.
// Returns false if the response is missing required fields (treated as
// JSON_ERROR by callers).
bool applyTokenResponse(const JsonDocument& doc) {
  std::string accessToken = doc["access_token"] | std::string("");
  if (accessToken.empty()) {
    LOG_ERR("RAUTH", "Token response missing access_token");
    return false;
  }
  std::string refreshToken = doc["refresh_token"] | std::string("");
  int64_t expiresAt = doc["expires_at"] | static_cast<int64_t>(0);
  int64_t expiresIn = doc["expires_in"] | static_cast<int64_t>(0);

  // The user object only ships on the initial sign-in response. On
  // refresh, Supabase returns the same id/email but they're equivalent
  // to what we already have — preserve existing values when absent.
  std::string userId = doc["user"]["id"] | READEST_STORE.getUserId();
  std::string userEmail = doc["user"]["email"] | READEST_STORE.getUserEmail();

  READEST_STORE.setSession(userEmail, userId, accessToken, refreshToken, expiresAt, expiresIn);
  return true;
}
}  // namespace

ReadestAuthClient::Error ReadestAuthClient::signIn(const std::string& email, const std::string& password,
                                                   std::string* errMsg) {
  if (email.empty() || password.empty()) {
    LOG_DBG("RAUTH", "signIn: empty email or password");
    return NO_CREDENTIALS;
  }
  const std::string anonKey = READEST_STORE.getSupabaseAnonKey();
  if (anonKey.empty()) {
    LOG_ERR("RAUTH", "signIn: anon key unavailable");
    return NO_ANON_KEY;
  }

  const std::string url = READEST_STORE.getSupabaseUrl() + "/auth/v1/token?grant_type=password";
  LOG_DBG("RAUTH", "signIn: %s", url.c_str());

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(SIGN_IN_CONNECT_TIMEOUT);
  http.setTimeout(SIGN_IN_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  http.addHeader("apikey", anonKey.c_str());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  JsonDocument req;
  req["email"] = email;
  req["password"] = password;
  std::string body;
  serializeJson(req, body);

  const int code = http.POST(body.c_str());
  const String response = http.getString();
  http.end();
  LOG_DBG("RAUTH", "signIn HTTP %d", code);

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    return mapHttpStatus(code);
  }

  JsonDocument resp;
  const auto err = deserializeJson(resp, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RAUTH", "signIn JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }
  if (!applyTokenResponse(resp)) {
    return JSON_ERROR;
  }
  return OK;
}

ReadestAuthClient::Error ReadestAuthClient::refresh(std::string* errMsg) {
  const std::string refreshToken = READEST_STORE.getRefreshToken();
  if (refreshToken.empty()) {
    LOG_DBG("RAUTH", "refresh: no refresh_token in store");
    return NO_CREDENTIALS;
  }
  const std::string anonKey = READEST_STORE.getSupabaseAnonKey();
  if (anonKey.empty()) {
    return NO_ANON_KEY;
  }

  const std::string url = READEST_STORE.getSupabaseUrl() + "/auth/v1/token?grant_type=refresh_token";
  LOG_DBG("RAUTH", "refresh: %s", url.c_str());

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(REFRESH_CONNECT_TIMEOUT);
  http.setTimeout(REFRESH_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  http.addHeader("apikey", anonKey.c_str());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  JsonDocument req;
  req["refresh_token"] = refreshToken;
  std::string body;
  serializeJson(req, body);

  const int code = http.POST(body.c_str());
  const String response = http.getString();
  http.end();
  LOG_DBG("RAUTH", "refresh HTTP %d", code);

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    // 4xx on refresh means the refresh token is no longer valid — caller
    // should treat this as "user must re-authenticate." We don't clear
    // tokens here so the caller can decide whether to surface an error
    // or silently prompt for sign-in.
    return mapHttpStatus(code);
  }

  JsonDocument resp;
  const auto err = deserializeJson(resp, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RAUTH", "refresh JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }
  if (!applyTokenResponse(resp)) {
    return JSON_ERROR;
  }
  return OK;
}

ReadestAuthClient::Error ReadestAuthClient::signOut() {
  const std::string accessToken = READEST_STORE.getAccessToken();
  // No token = nothing to revoke server-side; just clear locally and succeed.
  if (accessToken.empty()) {
    READEST_STORE.clearSession();
    return OK;
  }
  const std::string anonKey = READEST_STORE.getSupabaseAnonKey();

  const std::string url = READEST_STORE.getSupabaseUrl() + "/auth/v1/logout";
  LOG_DBG("RAUTH", "signOut: %s", url.c_str());

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(LOGOUT_CONNECT_TIMEOUT);
  http.setTimeout(LOGOUT_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  if (!anonKey.empty()) http.addHeader("apikey", anonKey.c_str());
  http.addHeader("Authorization", (std::string("Bearer ") + accessToken).c_str());

  const int code = http.POST("");
  http.end();
  LOG_DBG("RAUTH", "signOut HTTP %d", code);

  // Always clear local session — being unable to reach Supabase shouldn't
  // strand the user. Supabase invalidates server-side on 204; if it didn't,
  // the token would have expired within an hour anyway.
  READEST_STORE.clearSession();

  // Treat 204 (the documented success code) and 200 as OK; anything else is
  // surfaced as SERVER_ERROR but the local state is already clean.
  if (code == 200 || code == 204) return OK;
  if (code < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

const char* ReadestAuthClient::errorString(Error err) {
  switch (err) {
    case OK:
      return "OK";
    case NO_CREDENTIALS:
      return "No credentials";
    case NO_ANON_KEY:
      return "Supabase anon key not configured";
    case NETWORK_ERROR:
      return "Network error";
    case INVALID_CREDENTIALS:
      return "Invalid email or password";
    case FORBIDDEN:
      return "Authentication required";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "Malformed response";
  }
  return "Unknown error";
}
