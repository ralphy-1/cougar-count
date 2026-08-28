#include "net.h"
#include "wire.h"
#include "wrap.h"
#include "config.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copy firmware/src/secrets.h.example to firmware/src/secrets.h and fill it in. secrets.h is gitignored and must stay that way."
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {

WiFiClientSecure tls;

String   idToken_;
String   refreshToken_;
uint32_t tokenExpiresAt_ = 0;      // millis() when the token stops being valid
uint32_t nextAuthAttempt_ = 0;
bool     warnedInsecure_ = false;

// Refresh with five minutes to spare rather than on the deadline: a token that
// expires mid-request fails a write that had nothing else wrong with it.
const uint32_t TOKEN_MARGIN_MS = 5 * 60 * 1000;
const uint32_t AUTH_RETRY_MS   = 15 * 1000;

void applyTrust() {
  if (sizeof(FIREBASE_ROOT_CA) > 1) {
    tls.setCACert(FIREBASE_ROOT_CA);
    return;
  }
  // Loudly, and every time. An unverified TLS connection on a network we do not
  // control can be read and rewritten by whoever does control it, and the whole
  // reason this device is allowed on that network is that it is boring and
  // predictable. Do not ship like this.
  if (!warnedInsecure_) {
    Serial.println("[net] WARNING: no root CA set, TLS certificate NOT verified");
    warnedInsecure_ = true;
  }
  tls.setInsecure();
}

bool postJson(const String& url, const String& body, String& out) {
  HTTPClient http;
  if (!http.begin(tls, url)) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  const bool ok = (code >= 200 && code < 300);
  out = http.getString();
  http.end();
  if (!ok) Serial.printf("[net] POST %d\n", code);
  return ok;
}

bool putJson(const String& url, const char* body) {
  HTTPClient http;
  if (!http.begin(tls, url)) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  const int code = http.PUT((uint8_t*)body, strlen(body));
  const bool ok = (code >= 200 && code < 300);
  if (!ok) Serial.printf("[net] PUT %d %s\n", code, http.getString().c_str());
  http.end();
  return ok;
}

bool readTokens(const String& payload, uint32_t now_ms) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[net] auth response unreadable: %s\n", err.c_str());
    return false;
  }

  // The sign-in and refresh endpoints return the same three values under
  // different names, so each is looked up twice rather than assuming which
  // endpoint produced this payload.
  const char* id      = doc["idToken"]      | doc["id_token"]      | (const char*)nullptr;
  const char* refresh = doc["refreshToken"] | doc["refresh_token"] | (const char*)nullptr;
  const char* expires = doc["expiresIn"]    | doc["expires_in"]    | "3600";
  if (!id || !*id) return false;

  idToken_      = id;
  if (refresh && *refresh) refreshToken_ = refresh;

  const uint32_t lifetimeMs = (uint32_t)atol(expires) * 1000UL;
  tokenExpiresAt_ = now_ms + (lifetimeMs > TOKEN_MARGIN_MS
                                ? lifetimeMs - TOKEN_MARGIN_MS
                                : lifetimeMs / 2);
  return true;
}

bool signIn(uint32_t now_ms) {
  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=";
  url += FIREBASE_API_KEY;

  JsonDocument body;
  body["email"]             = DEVICE_EMAIL;
  body["password"]          = DEVICE_PASSWORD;
  body["returnSecureToken"] = true;

  String out, payload;
  serializeJson(body, payload);
  if (!postJson(url, payload, out)) return false;
  return readTokens(out, now_ms);
}

bool refresh(uint32_t now_ms) {
  if (refreshToken_.isEmpty()) return false;

  String url = "https://securetoken.googleapis.com/v1/token?key=";
  url += FIREBASE_API_KEY;

  String body = "grant_type=refresh_token&refresh_token=" + refreshToken_;

  HTTPClient http;
  if (!http.begin(tls, url)) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int code = http.POST(body);
  const String out = http.getString();
  http.end();
  if (code < 200 || code >= 300) return false;
  return readTokens(out, now_ms);
}

String rtdbUrl(const char* path) {
  String url = "https://";
  url += FIREBASE_HOST;
  url += path;
  url += "?auth=";
  url += idToken_;
  return url;
}

}  // namespace

namespace Net {

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  applyTrust();
}

void service(uint32_t now_ms) {
  if (WiFi.status() != WL_CONNECTED) {
    idToken_ = "";                       // a token is worthless without a link
    return;
  }
  if (!idToken_.isEmpty() && !reached(now_ms, tokenExpiresAt_)) return;
  if (!reached(now_ms, nextAuthAttempt_)) return;

  nextAuthAttempt_ = now_ms + AUTH_RETRY_MS;

  const bool ok = idToken_.isEmpty() ? signIn(now_ms) : refresh(now_ms);
  if (!ok) {
    // Fall all the way back to a fresh sign-in next time. A refresh token that
    // has been revoked will never start working again, and retrying it forever
    // is how a device goes quiet for a week.
    idToken_      = "";
    refreshToken_ = "";
    Serial.println("[net] authentication failed");
  }
}

bool ready() {
  return WiFi.status() == WL_CONNECTED && !idToken_.isEmpty();
}

bool sendCrossing(Cross c) {
  const char* path = counterPath(c);
  if (!path || !ready()) return false;
  return putJson(rtdbUrl(path), INCREMENT_BODY);
}

bool sendHeartbeat() {
  if (!ready()) return false;
  return putJson(rtdbUrl(HEARTBEAT_PATH), HEARTBEAT_BODY);
}

bool sendLanesOk(bool ok) {
  if (!ready()) return false;
  return putJson(rtdbUrl(LANES_OK_PATH), boolBody(ok));
}

}  // namespace Net
