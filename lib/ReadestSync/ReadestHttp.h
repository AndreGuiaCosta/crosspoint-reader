#pragma once
#include <ArduinoJson.h>

#include <string>
#include <utility>
#include <vector>

// One HTTPS round-trip for the Readest clients: TLS config, heap pre-flight,
// timeouts, headers, streamed JSON parse, and error-body extraction live here
// so the sync/storage/auth clients only build URLs and read fields.
namespace ReadestHttp {

// Returned instead of an HTTP status. Negative HTTPClient transport codes
// pass through unchanged; every client's mapHttpStatus treats code < 0 as
// NETWORK_ERROR, which is the right fallback for both sentinels too.
constexpr int JSON_PARSE_FAILED = -1000;  // got 200 but the body didn't parse
constexpr int LOW_HEAP = -1001;           // pre-TLS heap check failed

struct Request {
  const char* tag = "RHTTP";  // log tag
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  const std::string* body = nullptr;  // non-null = POST (json Content-Type added)
  int connectTimeoutMs = 5000;
  int readTimeoutMs = 10000;
  const JsonDocument* filter = nullptr;  // optional deserialization filter
};

// Perform the request. On 200, parse the body into `doc` (streamed straight
// off the socket; pass doc = nullptr to skip, e.g. logout). On any other
// status, extract a human-readable message from the error body into errMsg.
// Returns the HTTP status code, a negative transport code, or a sentinel.
int requestJson(const Request& req, JsonDocument* doc, std::string* errMsg);

// Standard headers for the authenticated sync/storage endpoints.
std::vector<std::pair<std::string, std::string>> bearerHeaders(const std::string& accessToken);

}  // namespace ReadestHttp
