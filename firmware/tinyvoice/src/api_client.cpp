#include "api_client.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

void ApiClient::setAuth(HTTPClient& http) {
    String auth = String("Bearer ") + DEVICE_TOKEN;
    http.addHeader("Authorization", auth);
}

String ApiClient::apiUrl(const char* path) {
    String base = API_BASE_URL;
    if (!base.endsWith("/")) base += "/";
    if (path[0] == '/') return base + String(path + 1);
    return base + path;
}

bool ApiClient::heartbeat() {
    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    http.begin(apiUrl("/api/v1/device/heartbeat"));
    setAuth(http);
    int code = http.POST("");
    http.end();
    return code == 200;
}

bool ApiClient::pollNext(NextMessage& out) {
    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    http.begin(apiUrl("/api/v1/device/messages/next"));
    setAuth(http);
    int code = http.GET();

    if (code != 200) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    out.available = doc["available"] | false;
    if (!out.available) return true;

    strlcpy(out.id, doc["id"] | "", sizeof(out.id));
    out.durationMs = doc["duration_ms"] | 0;
    out.sizeBytes = doc["size_bytes"] | 0L;
    return true;
}

bool ApiClient::uploadAudio(const uint8_t* data, size_t len) {
    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    http.begin(apiUrl("/api/v1/device/messages"));
    setAuth(http);

    String boundary = "----TinyVoiceBoundary7MA4YWxk";
    String contentType = "multipart/form-data; boundary=" + boundary;
    http.addHeader("Content-Type", contentType);

    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t totalLen = head.length() + len + tail.length();
    http.addHeader("Content-Length", String(totalLen));

    WiFiClient* client = http.getStreamPtr();
    http.sendRequest("POST");

    client->print(head);
    client->write(data, len);
    client->print(tail);

    int code = http.responseCode();
    http.end();
    return code == 201 || code == 200;
}

bool ApiClient::downloadAudio(const char* messageId, uint8_t** outData, size_t* outLen) {
    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    String path = String("/api/v1/device/messages/") + messageId + "/audio";
    http.begin(apiUrl(path.c_str()));
    setAuth(http);
    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0) {
        http.end();
        return false;
    }

    uint8_t* buf = (uint8_t*)malloc(len);
    if (!buf) {
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t pos = 0;
    while (http.connected() && pos < (size_t)len) {
        size_t avail = stream->available();
        if (avail) {
            int read = stream->readBytes(buf + pos, avail);
            pos += read;
        } else {
            delay(1);
        }
    }
    http.end();

    *outData = buf;
    *outLen = pos;
    return pos > 0;
}

bool ApiClient::markPlayed(const char* messageId) {
    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    String path = String("/api/v1/device/messages/") + messageId + "/played";
    http.begin(apiUrl(path.c_str()));
    setAuth(http);
    int code = http.POST("");
    http.end();
    return code == 200;
}
