#include "ReadestAuthClient.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include "ReadestAccountStore.h"
#include "ReadestHttp.h"

namespace {
// Timeouts in milliseconds.
constexpr int SIGN_IN_CONNECT_TIMEOUT = 5000;
constexpr int SIGN_IN_READ_TIMEOUT = 10000;
constexpr int REFRESH_CONNECT_TIMEOUT = 3000;
constexpr int REFRESH_READ_TIMEOUT = 7000;
constexpr int LOGOUT_CONNECT_TIMEOUT = 3000;
constexpr int LOGOUT_READ_TIMEOUT = 7000;

ReadestAuthClient::Error mapHttpStatus(int code) {
  using E = ReadestAuthClient::Error;
  if (code == 200) return E::OK;
  if (code == 400 || code == 401) return E::INVALID_CREDENTIALS;
  if (code == 403) return E::FORBIDDEN;
  if (code >= 500) return E::SERVER_ERROR;
  if (code < 0) return E::NETWORK_ERROR;
  return E::SERVER_ERROR;
}

// Apply session fields from a successful token response. Returns false on
// missing required fields.
bool applyTokenResponse(const JsonDocument& doc) {
  std::string accessToken = doc["access_token"] | std::string("");
  if (accessToken.empty()) {
    LOG_ERR("RAUTH", "Token response missing access_token");
    return false;
  }
  std::string refreshToken = doc["refresh_token"] | std::string("");
  int64_t expiresAt = doc["expires_at"] | static_cast<int64_t>(0);

  // user object only ships on initial sign-in; preserve existing on refresh.
  std::string userId = doc["user"]["id"] | READEST_STORE.getUserId();
  std::string userEmail = doc["user"]["email"] | READEST_STORE.getUserEmail();

  READEST_STORE.setSession(userEmail, userId, accessToken, refreshToken, expiresAt);
  return true;
}

// Shared POST /auth/v1/token flow; signIn and refresh differ only in
// grant_type, request body, and timeouts.
ReadestAuthClient::Error tokenRequest(const char* grantType, const JsonDocument& reqBody, int connectTimeoutMs,
                                      int readTimeoutMs, std::string* errMsg) {
  const std::string anonKey = READEST_STORE.getSupabaseAnonKey();
  if (anonKey.empty()) {
    LOG_ERR("RAUTH", "%s: anon key unavailable", grantType);
    return ReadestAuthClient::NO_ANON_KEY;
  }

  std::string body;
  serializeJson(reqBody, body);

  ReadestHttp::Request rq;
  rq.tag = "RAUTH";
  rq.url = READEST_STORE.getSupabaseUrl() + "/auth/v1/token?grant_type=" + grantType;
  rq.headers = {{"apikey", anonKey}, {"Accept", "application/json"}};
  rq.body = &body;
  rq.connectTimeoutMs = connectTimeoutMs;
  rq.readTimeoutMs = readTimeoutMs;

  JsonDocument resp;
  const int code = ReadestHttp::requestJson(rq, &resp, errMsg);
  if (code == ReadestHttp::JSON_PARSE_FAILED) return ReadestAuthClient::JSON_ERROR;
  // Don't clear tokens on 4xx; caller decides whether to prompt sign-in.
  if (code != 200) return mapHttpStatus(code);

  if (!applyTokenResponse(resp)) {
    return ReadestAuthClient::JSON_ERROR;
  }
  return ReadestAuthClient::OK;
}
}  // namespace

ReadestAuthClient::Error ReadestAuthClient::signIn(const std::string& email, const std::string& password,
                                                   std::string* errMsg) {
  if (email.empty() || password.empty()) {
    LOG_DBG("RAUTH", "signIn: empty email or password");
    return NO_CREDENTIALS;
  }

  JsonDocument req;
  req["email"] = email;
  req["password"] = password;
  return tokenRequest("password", req, SIGN_IN_CONNECT_TIMEOUT, SIGN_IN_READ_TIMEOUT, errMsg);
}

ReadestAuthClient::Error ReadestAuthClient::refresh(std::string* errMsg) {
  const std::string refreshToken = READEST_STORE.getRefreshToken();
  if (refreshToken.empty()) {
    LOG_DBG("RAUTH", "refresh: no refresh_token in store");
    return NO_CREDENTIALS;
  }

  JsonDocument req;
  req["refresh_token"] = refreshToken;
  return tokenRequest("refresh_token", req, REFRESH_CONNECT_TIMEOUT, REFRESH_READ_TIMEOUT, errMsg);
}

ReadestAuthClient::Error ReadestAuthClient::signOut() {
  const std::string accessToken = READEST_STORE.getAccessToken();
  // No token = nothing to revoke server-side; just clear locally and succeed.
  if (accessToken.empty()) {
    READEST_STORE.clearSession();
    return OK;
  }
  const std::string anonKey = READEST_STORE.getSupabaseAnonKey();

  ReadestHttp::Request rq;
  rq.tag = "RAUTH";
  rq.url = READEST_STORE.getSupabaseUrl() + "/auth/v1/logout";
  rq.headers = ReadestHttp::bearerHeaders(accessToken);
  if (!anonKey.empty()) rq.headers.emplace_back("apikey", anonKey);
  const std::string emptyBody;
  rq.body = &emptyBody;
  rq.connectTimeoutMs = LOGOUT_CONNECT_TIMEOUT;
  rq.readTimeoutMs = LOGOUT_READ_TIMEOUT;

  const int code = ReadestHttp::requestJson(rq, nullptr, nullptr);

  // Always clear local session; HTTP outcome shouldn't strand the user.
  READEST_STORE.clearSession();

  // 204 is the documented success code.
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
