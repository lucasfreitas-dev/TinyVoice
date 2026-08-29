#include "api_client.h"
#include "config.h"
#include "storage_lock.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <cstring>
#include <HTTPClient.h>

namespace {

struct ApiEndpoint {
    String host;
    uint16_t port;
    String uri;
};

ApiEndpoint parseEndpoint(const char* path) {
    ApiEndpoint ep;
    ep.port = 80;

    String base = API_BASE_URL;
    if (base.startsWith("http://")) {
        base.remove(0, 7);
    } else if (base.startsWith("https://")) {
        base.remove(0, 8);
        ep.port = 443;
    }

    int slash = base.indexOf('/');
    String hostPart = slash >= 0 ? base.substring(0, slash) : base;
    String uriPrefix = slash >= 0 ? base.substring(slash) : "";

    int colon = hostPart.indexOf(':');
    ep.host = colon >= 0 ? hostPart.substring(0, colon) : hostPart;
    if (colon >= 0) {
        ep.port = (uint16_t)hostPart.substring(colon + 1).toInt();
    }

    ep.uri = uriPrefix;
    if (!ep.uri.endsWith("/")) {
        ep.uri += "/";
    }
    if (path[0] == '/') {
        ep.uri += path + 1;
    } else {
        ep.uri += path;
    }
    return ep;
}

void configureHttp(HTTPClient& http) {
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    http.setReuse(false);
}

void configureHttpUpload(HTTPClient& http) {
    http.setConnectTimeout(5000);
    http.setTimeout(API_TIMEOUT_MS);
    http.setReuse(false);
}

bool beginRequest(HTTPClient& http, WiFiClient& client, const char* path, bool forUpload) {
    ApiEndpoint ep = parseEndpoint(path);
    if (forUpload) {
        configureHttpUpload(http);
    } else {
        configureHttp(http);
    }
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    return http.begin(client, ep.host, ep.port, ep.uri);
}

bool beginRequest(HTTPClient& http, WiFiClient& client, const char* path) {
    return beginRequest(http, client, path, false);
}

void logHttpResult(const char* op, int code) {
    if (code > 0) {
        Serial.printf("%s http code: %d\n", op, code);
    } else {
        Serial.printf("%s http error: %d (%s)\n", op, code, HTTPClient::errorToString(code).c_str());
    }
}

// Streams multipart head + audio + tail without copying the WAV into a second buffer.
class MultipartStream : public Stream {
public:
    MultipartStream(const char* head, size_t headLen,
                    const uint8_t* data, size_t dataLen,
                    const char* tail, size_t tailLen)
        : _head(head), _headLen(headLen),
          _data(data), _dataLen(dataLen),
          _tail(tail), _tailLen(tailLen), _pos(0) {}

    int available() override {
        size_t total = _headLen + _dataLen + _tailLen;
        return _pos >= total ? 0 : (int)(total - _pos);
    }

    int read() override {
        if (available() <= 0) return -1;
        uint8_t b = 0;
        if (_pos < _headLen) {
            b = (uint8_t)_head[_pos];
        } else if (_pos < _headLen + _dataLen) {
            b = _data[_pos - _headLen];
        } else {
            b = (uint8_t)_tail[_pos - _headLen - _dataLen];
        }
        _pos++;
        return b;
    }

    int peek() override {
        if (available() <= 0) return -1;
        if (_pos < _headLen) return (uint8_t)_head[_pos];
        if (_pos < _headLen + _dataLen) return _data[_pos - _headLen];
        return (uint8_t)_tail[_pos - _headLen - _dataLen];
    }

    size_t readBytes(uint8_t* buffer, size_t length) override {
        size_t written = 0;
        while (written < length) {
            int b = read();
            if (b < 0) break;
            buffer[written++] = (uint8_t)b;
        }
        return written;
    }

    size_t write(uint8_t) override { return 0; }
    void flush() override {}

private:
    const char* _head;
    size_t _headLen;
    const uint8_t* _data;
    size_t _dataLen;
    const char* _tail;
    size_t _tailLen;
    size_t _pos;
};

class MultipartFileStream : public Stream {
public:
    MultipartFileStream(const char* head, size_t headLen,
                        File& file, size_t fileLen,
                        const char* tail, size_t tailLen)
        : _head(head), _headLen(headLen),
          _file(file), _fileLen(fileLen),
          _tail(tail), _tailLen(tailLen), _pos(0) {}

    int available() override {
        size_t total = _headLen + _fileLen + _tailLen;
        return _pos >= total ? 0 : (int)(total - _pos);
    }

    int read() override {
        uint8_t b = 0;
        return readBytes(&b, 1) ? b : -1;
    }

    int peek() override { return -1; }

    size_t readBytes(uint8_t* buffer, size_t length) override {
        size_t written = 0;
        while (written < length) {
            if (_pos < _headLen) {
                buffer[written++] = (uint8_t)_head[_pos++];
                continue;
            }
            if (_pos < _headLen + _fileLen) {
                size_t want = length - written;
                size_t left = _headLen + _fileLen - _pos;
                if (want > left) {
                    want = left;
                }
                int n = _file.read(buffer + written, want);
                if (n <= 0) {
                    break;
                }
                written += (size_t)n;
                _pos += (size_t)n;
                continue;
            }
            if (_pos < _headLen + _fileLen + _tailLen) {
                buffer[written++] = (uint8_t)_tail[_pos - _headLen - _fileLen];
                _pos++;
                continue;
            }
            break;
        }
        return written;
    }

    size_t write(uint8_t) override { return 0; }
    void flush() override {}

private:
    const char* _head;
    size_t _headLen;
    File& _file;
    size_t _fileLen;
    const char* _tail;
    size_t _tailLen;
    size_t _pos;
};

class MultipartRecordingStream : public Stream {
public:
    MultipartRecordingStream(const char* head, size_t headLen,
                             const uint8_t* wavHeader, size_t wavHeaderLen,
                             int chunkCount, size_t pcmLen,
                             const char* tail, size_t tailLen)
        : _head(head), _headLen(headLen),
          _wavHeader(wavHeader), _wavHeaderLen(wavHeaderLen),
          _chunkCount(chunkCount), _pcmLen(pcmLen),
          _tail(tail), _tailLen(tailLen),
          _pos(0), _chunkIndex(0), _fileRemaining(0) {}

    int available() override {
        size_t total = _headLen + _wavHeaderLen + _pcmLen + _tailLen;
        return _pos >= total ? 0 : (int)(total - _pos);
    }

    int read() override {
        uint8_t b = 0;
        return readBytes(&b, 1) ? b : -1;
    }

    int peek() override { return -1; }

    size_t readBytes(uint8_t* buffer, size_t length) override {
        size_t written = 0;
        while (written < length) {
            if (_pos < _headLen) {
                buffer[written++] = (uint8_t)_head[_pos++];
                continue;
            }

            size_t headerStart = _headLen;
            if (_pos < headerStart + _wavHeaderLen) {
                size_t idx = _pos - headerStart;
                buffer[written++] = _wavHeader[idx];
                _pos++;
                continue;
            }

            size_t pcmStart = headerStart + _wavHeaderLen;
            if (_pos < pcmStart + _pcmLen) {
                if (_fileRemaining == 0) {
                    if (!openNextChunk()) {
                        break;
                    }
                }
                size_t want = length - written;
                if (want > _fileRemaining) {
                    want = _fileRemaining;
                }
                int n = _file.read(buffer + written, want);
                if (n <= 0) {
                    closeChunk();
                    continue;
                }
                written += (size_t)n;
                _pos += (size_t)n;
                _fileRemaining -= (size_t)n;
                if (_fileRemaining == 0) {
                    closeChunk();
                }
                if ((_pos & 0x1FFF) == 0) {
                    yield();
                }
                continue;
            }

            if (_pos < pcmStart + _pcmLen + _tailLen) {
                size_t idx = _pos - pcmStart - _pcmLen;
                buffer[written++] = (uint8_t)_tail[idx];
                _pos++;
                continue;
            }
            break;
        }
        return written;
    }

    size_t write(uint8_t) override { return 0; }
    void flush() override {}

private:
    bool openNextChunk() {
        closeChunk();
        if (_chunkIndex >= _chunkCount) {
            return false;
        }
        char path[24];
        snprintf(path, sizeof(path), "/rec/c%04d.pcm", _chunkIndex);
        if (!storageLock()) {
            return false;
        }
        _file = LittleFS.open(path, FILE_READ);
        storageUnlock();
        _chunkIndex++;
        if (!_file) {
            return false;
        }
        _fileRemaining = _file.size();
        return _fileRemaining > 0;
    }

    void closeChunk() {
        if (_file) {
            _file.close();
        }
        _fileRemaining = 0;
    }

    const char* _head;
    size_t _headLen;
    const uint8_t* _wavHeader;
    size_t _wavHeaderLen;
    int _chunkCount;
    size_t _pcmLen;
    const char* _tail;
    size_t _tailLen;
    size_t _pos;
    int _chunkIndex;
    File _file;
    size_t _fileRemaining;
};

}  // namespace

void ApiClient::setAuth(HTTPClient& http) {
    String auth = String("Bearer ") + DEVICE_TOKEN;
    http.addHeader("Authorization", auth);
}

bool ApiClient::heartbeat() {
    WiFiClient client;
    HTTPClient http;
    if (!beginRequest(http, client, "/api/v1/device/heartbeat")) {
        Serial.println("heartbeat: begin failed");
        return false;
    }
    setAuth(http);
    int code = http.POST("");
    logHttpResult("heartbeat", code);
    http.end();
    return code == 200;
}

bool ApiClient::pollNext(NextMessage& out) {
    WiFiClient client;
    HTTPClient http;
    if (!beginRequest(http, client, "/api/v1/device/messages/next")) {
        return false;
    }
    setAuth(http);
    int code = http.GET();

    if (code != 200) {
        logHttpResult("poll", code);
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
    WiFiClient client;
    HTTPClient http;
    if (!beginRequest(http, client, "/api/v1/device/messages", true)) {
        Serial.println("upload: begin failed");
        return false;
    }
    setAuth(http);

    String boundary = "----TinyVoiceBoundary7MA4YWxk";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    size_t totalLen = head.length() + len + tail.length();

    MultipartStream body(head.c_str(), head.length(), data, len, tail.c_str(), tail.length());
    int code = http.sendRequest("POST", &body, totalLen);
    if (code != 201 && code != 200) {
        logHttpResult("upload", code);
    }
    http.end();
    return code == 201 || code == 200;
}

bool ApiClient::uploadAudioFile(const char* path) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        Serial.println("upload: recording file missing");
        return false;
    }

    size_t len = file.size();
    if (len <= 44) {
        file.close();
        Serial.println("upload: recording file empty");
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    if (!beginRequest(http, client, "/api/v1/device/messages", true)) {
        Serial.println("upload: begin failed");
        file.close();
        return false;
    }
    setAuth(http);

    String boundary = "----TinyVoiceBoundary7MA4YWxk";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    size_t totalLen = head.length() + len + tail.length();

    MultipartFileStream body(head.c_str(), head.length(), file, len, tail.c_str(), tail.length());
    int code = http.sendRequest("POST", &body, totalLen);
    file.close();
    if (code != 201 && code != 200) {
        logHttpResult("upload", code);
    }
    http.end();
    return code == 201 || code == 200;
}

bool ApiClient::uploadRecording(const uint8_t* wavHeader, size_t pcmBytes, int chunkCount) {
    if (!wavHeader || pcmBytes == 0 || chunkCount <= 0) {
        Serial.println("upload: invalid recording chunks");
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    if (!beginRequest(http, client, "/api/v1/device/messages", true)) {
        Serial.println("upload: begin failed");
        return false;
    }
    setAuth(http);

    unsigned long uploadTimeout = API_TIMEOUT_MS + (pcmBytes / 64);
    if (uploadTimeout < 60000) {
        uploadTimeout = 60000;
    }
    if (uploadTimeout > 180000) {
        uploadTimeout = 180000;
    }
    http.setTimeout(uploadTimeout);

    String boundary = "----TinyVoiceBoundary7MA4YWxk";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    size_t totalLen = head.length() + 44 + pcmBytes + tail.length();

    MultipartRecordingStream body(
        head.c_str(), head.length(),
        wavHeader, 44,
        chunkCount, pcmBytes,
        tail.c_str(), tail.length());
    int code = http.sendRequest("POST", &body, totalLen);
    if (code != 201 && code != 200) {
        logHttpResult("upload", code);
    }
    http.end();
    return code == 201 || code == 200;
}

bool ApiClient::downloadAudio(const char* messageId, uint8_t** outData, size_t* outLen) {
    WiFiClient client;
    HTTPClient http;
    String path = String("/api/v1/device/messages/") + messageId + "/audio";
    if (!beginRequest(http, client, path.c_str())) {
        return false;
    }
    setAuth(http);
    int code = http.GET();
    if (code != 200) {
        logHttpResult("download", code);
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
    WiFiClient client;
    HTTPClient http;
    String path = String("/api/v1/device/messages/") + messageId + "/played";
    if (!beginRequest(http, client, path.c_str())) {
        return false;
    }
    setAuth(http);
    int code = http.POST("");
    logHttpResult("played", code);
    http.end();
    return code == 200;
}
