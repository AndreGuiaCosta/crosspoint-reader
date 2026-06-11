#include "ReadestHttp.h"

#include <HTTPClient.h>
#include <Logging.h>
#include "ReadestTlsConfig.h"
#include <WiFiClientSecure.h>

namespace {
// Same rationale as KOReaderSyncClient: mbedTLS needs ~50KB of aggregate free
// heap or the handshake dies with MBEDTLS_ERR_X509_ALLOC_FAILED. Total free,
// not max contiguous block — the failure mode is aggregate exhaustion.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Union of the error-body shapes across the Readest endpoints: Supabase auth
// uses error_description/msg, the sync API uses error/message.
//
// Pass length explicitly to deserializeJson — the simulator's String
// lookalike has no dedicated ArduinoJson reader, and the auto-detected one
// stops short of the full payload.
void extractErrorMessage(const String& body, std::string* errMsg) {
  if (!errMsg) return;
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length()) != DeserializationError::Ok) return;
  std::string msg = doc["error_description"] | std::string("");
  if (msg.empty()) msg = doc["msg"] | std::string("");
  if (msg.empty()) msg = doc["error"] | std::string("");
  if (msg.empty()) msg = doc["message"] | std::string("");
  if (!msg.empty()) *errMsg = std::move(msg);
}
}  // namespace

namespace ReadestHttp {

std::vector<std::pair<std::string, std::string>> bearerHeaders(const std::string& accessToken) {
  return {{"Authorization", "Bearer " + accessToken}, {"Accept", "application/json"}};
}

int requestJson(const Request& req, JsonDocument* doc, std::string* errMsg) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR(req.tag, "Insufficient heap for TLS handshake: %u free (need %u)", (unsigned)freeHeap,
            (unsigned)MIN_HEAP_FOR_TLS);
    if (errMsg) *errMsg = "Low memory for TLS";
    return LOW_HEAP;
  }

  WiFiClientSecure secureClient;
  ReadestTls::configure(secureClient);

  HTTPClient http;
  http.setConnectTimeout(req.connectTimeoutMs);
  http.setTimeout(req.readTimeoutMs);
  // HTTP/1.0 disables chunked transfer-encoding so the success body can be
  // parsed straight off the socket (getStream() does not decode chunks).
  http.useHTTP10(true);
  http.begin(secureClient, req.url.c_str());
  for (const auto& h : req.headers) {
    http.addHeader(h.first.c_str(), h.second.c_str());
  }

  int code;
  if (req.body) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(req.body->c_str());
  } else {
    code = http.GET();
  }
  LOG_DBG(req.tag, "%s %s -> HTTP %d", req.body ? "POST" : "GET", req.url.c_str(), code);

  if (code != 200) {
    extractErrorMessage(http.getString(), errMsg);
    http.end();
    return code;
  }

  if (!doc) {
    http.end();
    return code;
  }

  // Streamed parse: never materialize the body as a String. With a filter,
  // peak heap is bounded by the kept fields, not the response size.
  const DeserializationError err = req.filter
                                       ? deserializeJson(*doc, http.getStream(), DeserializationOption::Filter(*req.filter))
                                       : deserializeJson(*doc, http.getStream());
  http.end();
  if (err) {
    LOG_ERR(req.tag, "JSON parse: %s", err.c_str());
    return JSON_PARSE_FAILED;
  }
  return code;
}

}  // namespace ReadestHttp
