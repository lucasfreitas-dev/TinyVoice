#include "api_client.h"
#include "config.h"
#include "storage_lock.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <cstring>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>

namespace {

ApiProgressFn s_progressFn = nullptr;

void apiProgressTick() {
    if (s_progressFn) {
        s_progressFn();
    }
    yield();
}

SemaphoreHandle_t s_apiMutex = nullptr;

class ApiLock {
public:
    ApiLock() : _held(false) {
        if (!s_apiMutex) {
            s_apiMutex = xSemaphoreCreateMutex();
        }
        _held = xSemaphoreTake(s_apiMutex, pdMS_TO_TICKS(90000)) == pdTRUE;
    }
    ~ApiLock() {
        if (_held) {
            xSemaphoreGive(s_apiMutex);
        }
    }
    bool held() const { return _held; }

private:
    bool _held;
};

struct ApiEndpoint {
    String host;
    uint16_t port;
    String uri;
    bool tls;
};

WiFiClient s_plainClient;
WiFiClientSecure s_tlsClient;
bool s_tlsConfigured = false;

bool apiUsesTls() {
    return String(API_BASE_URL).startsWith("https://");
}

void releaseClients() {
    s_plainClient.stop();
    s_tlsClient.stop();
}

WiFiClient& plainClient() {
    return s_plainClient;
}

WiFiClientSecure& tlsClient() {
    if (!s_tlsConfigured) {
        s_tlsClient.setInsecure();
        s_tlsConfigured = true;
    }
    return s_tlsClient;
}

ApiEndpoint parseEndpoint(const char* path) {
    ApiEndpoint ep;
    ep.port = 80;
    ep.tls = false;

    String base = API_BASE_URL;
    if (base.startsWith("http://")) {
        base.remove(0, 7);
    } else if (base.startsWith("https://")) {
        base.remove(0, 8);
        ep.port = 443;
        ep.tls = true;
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

void configureHttpDownload(HTTPClient& http) {
    http.setConnectTimeout(5000);
    http.setTimeout(30000);
    http.setReuse(false);
}

void configureHttpUpload(HTTPClient& http) {
    http.setConnectTimeout(5000);
    http.setTimeout(API_TIMEOUT_MS);
    http.setReuse(false);
}

bool beginDownloadRequest(HTTPClient& http, const char* path) {
    ApiEndpoint ep = parseEndpoint(path);
    configureHttpDownload(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (apiUsesTls()) {
        return http.begin(tlsClient(), ep.host, ep.port, ep.uri);
    }
    return http.begin(plainClient(), ep.host, ep.port, ep.uri);
}

bool beginRequest(HTTPClient& http, const char* path, bool forUpload) {
    ApiEndpoint ep = parseEndpoint(path);
    if (forUpload) {
        configureHttpUpload(http);
    } else {
        configureHttp(http);
    }
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (apiUsesTls()) {
        return http.begin(tlsClient(), ep.host, ep.port, ep.uri);
    }
    return http.begin(plainClient(), ep.host, ep.port, ep.uri);
}

bool beginRequest(HTTPClient& http, const char* path) {
    return beginRequest(http, path, false);
}

void logHttpResult(const char* op, int code) {
    if (code > 0) {
        Serial.printf("%s http code: %d\n", op, code);
    } else {
        Serial.printf("%s http error: %d (%s)\n", op, code, HTTPClient::errorToString(code).c_str());
    }
}

const char* MULTIPART_BOUNDARY = "----TinyVoiceBoundary7MA4YWxk";

uint8_t s_bodyBuf[1024];

// ssl_client sets O_NONBLOCK for its connect timeout and never clears it. That is required
// for available()/connected() to stay cheap, but during a long send it turns a full LWIP
// buffer into an EWOULDBLOCK that mbedtls_ssl_write can only spin on until it times out.
// So switch to blocking sends for the body, then switch back before reading the response.
void setSocketBlocking(WiFiClientSecure& tls, bool blocking) {
    int fd = tls.fd();
    if (fd < 0) {
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}

// mbedtls_ssl_write may accept only part of the buffer, and HTTPClient gives up after
// retrying once. Keep pushing until every byte is accepted.
bool tlsWriteAll(WiFiClientSecure& tls, const uint8_t* data, size_t len) {
    size_t sent = 0;
    unsigned long lastMoved = millis();

    while (sent < len) {
        size_t n = tls.write(data + sent, len - sent);
        if (n > 0) {
            sent += n;
            lastMoved = millis();
            apiProgressTick();
            continue;
        }

        // write() returns 0 for a stalled socket and for a fatal TLS error alike, so only
        // probe the connection here (connected() can block while the socket is blocking).
        if (!tls.connected()) {
            Serial.printf("upload: tls closed after %u/%u bytes (errno=%d)\n",
                          (unsigned)sent, (unsigned)len, errno);
            return false;
        }
        if (millis() - lastMoved > 8000) {
            Serial.printf("upload: tls write stalled at %u/%u bytes "
                          "(errno=%d free=%u max=%u)\n",
                          (unsigned)sent, (unsigned)len, errno,
                          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            return false;
        }
        apiProgressTick();
        delay(5);
    }
    return true;
}

int readHttpStatus(WiFiClientSecure& tls, unsigned long timeoutMs) {
    unsigned long start = millis();
    char line[64];
    size_t n = 0;

    while (millis() - start < timeoutMs) {
        int c = tls.read();
        if (c < 0) {
            if (!tls.connected() && tls.available() <= 0) {
                break;
            }
            apiProgressTick();
            delay(5);
            continue;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r' && n + 1 < sizeof(line)) {
            line[n++] = (char)c;
        }
    }
    line[n] = '\0';

    const char* space = strchr(line, ' ');
    if (!space) {
        Serial.printf("upload: no status line (got \"%s\")\n", line);
        return -1;
    }
    return atoi(space + 1);
}

bool writeBodyFromFile(WiFiClientSecure& tls, File& file, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t want = len - sent;
        if (want > sizeof(s_bodyBuf)) {
            want = sizeof(s_bodyBuf);
        }
        int n = file.read(s_bodyBuf, want);
        if (n <= 0) {
            Serial.printf("upload: file read failed at %u/%u\n", (unsigned)sent, (unsigned)len);
            return false;
        }
        if (!tlsWriteAll(tls, s_bodyBuf, (size_t)n)) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

bool writeBodyFromChunks(WiFiClientSecure& tls, const uint8_t* wavHeader,
                         size_t pcmBytes, int chunkCount) {
    if (!tlsWriteAll(tls, wavHeader, 44)) {
        return false;
    }

    size_t sent = 0;
    for (int i = 0; i < chunkCount && sent < pcmBytes; i++) {
        char path[24];
        snprintf(path, sizeof(path), "/rec/c%04d.pcm", i);

        if (!storageLock()) {
            Serial.println("upload: storage lock timeout");
            return false;
        }
        File in = LittleFS.open(path, FILE_READ);
        storageUnlock();
        if (!in) {
            Serial.printf("upload: chunk missing %s\n", path);
            return false;
        }

        size_t remaining = in.size();
        while (remaining > 0) {
            size_t want = remaining > sizeof(s_bodyBuf) ? sizeof(s_bodyBuf) : remaining;
            int n = in.read(s_bodyBuf, want);
            if (n <= 0) {
                Serial.printf("upload: chunk read failed %s\n", path);
                in.close();
                return false;
            }
            if (!tlsWriteAll(tls, s_bodyBuf, (size_t)n)) {
                in.close();
                return false;
            }
            remaining -= (size_t)n;
            sent += (size_t)n;
        }
        in.close();
    }

    if (sent != pcmBytes) {
        Serial.printf("upload: chunk total %u != declared %u\n",
                      (unsigned)sent, (unsigned)pcmBytes);
        return false;
    }
    return true;
}

// Posts the recording as multipart/form-data straight over TLS. Body comes either from
// an assembled WAV (filePath) or from the raw PCM chunks plus a synthesized WAV header.
int postRecording(WiFiClientSecure& tls, const char* filePath,
                  const uint8_t* wavHeader, size_t pcmBytes, int chunkCount) {
    ApiEndpoint ep = parseEndpoint("/api/v1/device/messages");

    File file;
    size_t bodyLen;
    if (filePath) {
        file = LittleFS.open(filePath, FILE_READ);
        if (!file) {
            Serial.println("upload: recording file missing");
            return -1;
        }
        bodyLen = file.size();
        if (bodyLen <= 44) {
            file.close();
            Serial.println("upload: recording file empty");
            return -1;
        }
    } else {
        if (!wavHeader || pcmBytes == 0 || chunkCount <= 0) {
            return -1;
        }
        bodyLen = 44 + pcmBytes;
    }

    char head[192];
    int headLen = snprintf(head, sizeof(head),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           MULTIPART_BOUNDARY);
    char tail[64];
    int tailLen = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", MULTIPART_BOUNDARY);

    char req[512];
    int reqLen = snprintf(req, sizeof(req),
                          "POST %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "Authorization: Bearer %s\r\n"
                          "Content-Type: multipart/form-data; boundary=%s\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          ep.uri.c_str(), ep.host.c_str(), DEVICE_TOKEN, MULTIPART_BOUNDARY,
                          (unsigned)((size_t)headLen + bodyLen + (size_t)tailLen));

    if (headLen <= 0 || tailLen <= 0 || reqLen <= 0 || reqLen >= (int)sizeof(req)) {
        Serial.println("upload: request header too long");
        if (file) {
            file.close();
        }
        return -1;
    }

    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    tls.setTimeout(10);
    if (!tls.connect(ep.host.c_str(), ep.port)) {
        Serial.println("upload: tls connect failed");
        if (file) {
            file.close();
        }
        return -1;
    }

    setSocketBlocking(tls, true);

    Serial.printf("upload: tls up fd=%d (free=%u max=%u)\n",
                  tls.fd(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    bool ok = tlsWriteAll(tls, (const uint8_t*)req, (size_t)reqLen) &&
              tlsWriteAll(tls, (const uint8_t*)head, (size_t)headLen);

    if (ok) {
        ok = filePath ? writeBodyFromFile(tls, file, bodyLen)
                      : writeBodyFromChunks(tls, wavHeader, pcmBytes, chunkCount);
    }
    if (ok) {
        ok = tlsWriteAll(tls, (const uint8_t*)tail, (size_t)tailLen);
    }

    if (file) {
        file.close();
    }
    setSocketBlocking(tls, false);

    if (!ok) {
        return -1;
    }

    Serial.printf("upload: body sent (%u bytes), waiting for response\n",
                  (unsigned)((size_t)headLen + bodyLen + (size_t)tailLen));
    return readHttpStatus(tls, 60000);
}

int postRecordingWithRetry(const char* filePath, const uint8_t* wavHeader,
                           size_t pcmBytes, int chunkCount) {
    int code = -1;

    for (int attempt = 1; attempt <= 2; attempt++) {
        releaseClients();
        delay(attempt == 1 ? 200 : 1500);

        WiFiClientSecure tls;
        tls.setInsecure();
        code = postRecording(tls, filePath, wavHeader, pcmBytes, chunkCount);
        tls.stop();

        if (code == 200 || code == 201) {
            return code;
        }
        Serial.printf("upload: attempt %d failed (status %d)\n", attempt, code);
    }

    return code;
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
        if (_pos >= _totalLen()) {
            return 0;
        }
        return (int)(_totalLen() - _pos);
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
                if ((_pos & 0xFFF) == 0) {
                    apiProgressTick();
                }
                continue;
            }
            if (_pos < _headLen + _fileLen + _tailLen) {
                buffer[written++] = (uint8_t)_tail[_pos - _headLen - _fileLen];
                _pos++;
                if ((_pos & 0xFFF) == 0) {
                    apiProgressTick();
                }
                continue;
            }
            break;
        }
        return written;
    }

    size_t write(uint8_t) override { return 0; }
    void flush() override {}

private:
    size_t _totalLen() const {
        return _headLen + _fileLen + _tailLen;
    }

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
                        Serial.printf("upload: stream stalled at byte %u (chunk %d/%d)\n",
                                      (unsigned)(_pos - pcmStart),
                                      _chunkIndex, _chunkCount);
                        break;
                    }
                }
                size_t want = length - written;
                if (want > _fileRemaining) {
                    want = _fileRemaining;
                }
                int n = _file.read(buffer + written, want);
                if (n <= 0) {
                    Serial.printf("upload: chunk read error at byte %u\n",
                                  (unsigned)(_pos - pcmStart));
                    closeChunk();
                    break;
                }
                written += (size_t)n;
                _pos += (size_t)n;
                _fileRemaining -= (size_t)n;
                if (_fileRemaining == 0) {
                    closeChunk();
                }
                if ((_pos & 0xFFF) == 0) {
                    apiProgressTick();
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
        if (!_file) {
            Serial.printf("upload: chunk missing %s\n", path);
            return false;
        }
        _chunkIndex++;
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

bool uploadMultipartFile(WiFiClientSecure& tls, const char* path) {
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

    HTTPClient http;
    ApiEndpoint ep = parseEndpoint("/api/v1/device/messages");
    configureHttpUpload(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (!http.begin(tls, ep.host, ep.port, ep.uri)) {
        Serial.println("upload: begin failed");
        file.close();
        return false;
    }

    String auth = String("Bearer ") + DEVICE_TOKEN;
    http.addHeader("Authorization", auth);

    unsigned long uploadTimeout = 60000 + (len / 32);
    if (uploadTimeout > 180000) {
        uploadTimeout = 180000;
    }
    http.setTimeout(uploadTimeout);

    static const char* boundary = "----TinyVoiceBoundary7MA4YWxk";
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);

    static const char* headPrefix = "--";
    String head = String(headPrefix) + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = String("\r\n--") + boundary + "--\r\n";
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

bool uploadMultipartRecording(WiFiClientSecure& tls, const uint8_t* wavHeader,
                            size_t pcmBytes, int chunkCount) {
    if (!wavHeader || pcmBytes == 0 || chunkCount <= 0) {
        return false;
    }

    HTTPClient http;
    ApiEndpoint ep = parseEndpoint("/api/v1/device/messages");
    configureHttpUpload(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (!http.begin(tls, ep.host, ep.port, ep.uri)) {
        Serial.println("upload: begin failed");
        return false;
    }

    String auth = String("Bearer ") + DEVICE_TOKEN;
    http.addHeader("Authorization", auth);

    unsigned long uploadTimeout = 60000 + (pcmBytes / 32);
    if (uploadTimeout > 300000) {
        uploadTimeout = 300000;
    }
    http.setTimeout(uploadTimeout);

    static const char* boundary = "----TinyVoiceBoundary7MA4YWxk";
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);

    String head = String("--") + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = String("\r\n--") + boundary + "--\r\n";
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

}  // namespace

void setApiProgressHook(ApiProgressFn fn) {
    s_progressFn = fn;
}

void ApiClient::setAuth(HTTPClient& http) {
    String auth = String("Bearer ") + DEVICE_TOKEN;
    http.addHeader("Authorization", auth);
}

void ApiClient::releaseConnections() {
    releaseClients();
}

bool ApiClient::heartbeat() {
    ApiLock lock;
    if (!lock.held()) {
        return false;
    }
    releaseConnections();
    delay(50);

    WiFiClientSecure tls;
    tls.setInsecure();

    HTTPClient http;
    ApiEndpoint ep = parseEndpoint("/api/v1/device/heartbeat");
    configureHttp(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (!http.begin(tls, ep.host, ep.port, ep.uri)) {
        Serial.println("heartbeat: begin failed");
        return false;
    }
    setAuth(http);
    int code = http.POST("");
    logHttpResult("heartbeat", code);
    http.end();
    tls.stop();
    return code == 200;
}

bool ApiClient::pollNext(NextMessage& out) {
    ApiLock lock;
    if (!lock.held()) {
        return false;
    }
    releaseConnections();
    delay(50);

    WiFiClientSecure tls;
    tls.setInsecure();

    HTTPClient http;
    ApiEndpoint ep = parseEndpoint("/api/v1/device/messages/next");
    configureHttp(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (!http.begin(tls, ep.host, ep.port, ep.uri)) {
        return false;
    }
    setAuth(http);
    int code = http.GET();

    if (code != 200) {
        logHttpResult("poll", code);
        http.end();
        tls.stop();
        return false;
    }

    String body = http.getString();
    http.end();
    tls.stop();

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
    if (!beginRequest(http, "/api/v1/device/messages", true)) {
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
    releaseClients();
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

    HTTPClient http;
    if (!beginRequest(http, "/api/v1/device/messages", true)) {
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
    releaseClients();
    return code == 201 || code == 200;
}

bool ApiClient::uploadRecordingFile(const char* path) {
    ApiLock lock;
    if (!lock.held()) {
        return false;
    }
    Serial.printf("upload: heap free=%u min=%u max=%u\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());

    int code = postRecordingWithRetry(path, nullptr, 0, 0);
    releaseConnections();

    if (code != 200 && code != 201) {
        Serial.printf("upload: rejected (status %d)\n", code);
        return false;
    }
    return true;
}

bool ApiClient::uploadRecordingStream(const uint8_t* wavHeader, size_t pcmBytes, int chunkCount) {
    ApiLock lock;
    if (!lock.held()) {
        return false;
    }
    Serial.printf("upload: streaming %u pcm bytes, heap max=%u\n",
                  (unsigned)pcmBytes, ESP.getMaxAllocHeap());

    int code = postRecordingWithRetry(nullptr, wavHeader, pcmBytes, chunkCount);
    releaseConnections();

    if (code != 200 && code != 201) {
        Serial.printf("upload: rejected (status %d)\n", code);
        return false;
    }
    return true;
}

bool ApiClient::downloadAudioToFile(const char* messageId, const char* path) {
    ApiLock lock;
    if (!lock.held()) {
        Serial.println("download: api lock timeout");
        return false;
    }
    releaseConnections();
    delay(250);

    Serial.printf("download: heap free=%u max=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    WiFiClientSecure downloadTls;
    downloadTls.setInsecure();

    HTTPClient http;
    String apiPath = String("/api/v1/device/messages/") + messageId + "/audio";
    ApiEndpoint ep = parseEndpoint(apiPath.c_str());
    configureHttpDownload(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (!http.begin(downloadTls, ep.host, ep.port, ep.uri)) {
        Serial.println("download: begin failed");
        return false;
    }
    setAuth(http);
    int code = http.GET();
    if (code != 200) {
        logHttpResult("download", code);
        http.end();
        downloadTls.stop();
        return false;
    }

    int len = http.getSize();
    Serial.printf("download: expected=%d heap=%u\n", len, ESP.getFreeHeap());

    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
    }
    File out = LittleFS.open(path, FILE_WRITE);
    if (!out) {
        Serial.println("download: open failed");
        http.end();
        downloadTls.stop();
        return false;
    }

    // Read straight off the TLS client. Stream::readBytes would go byte-at-a-time (neither
    // WiFiClient nor WiFiClientSecure overrides it) and hides why a transfer died; read()
    // hands back the raw mbedTLS code instead. Reuses the upload body buffer: capture and
    // networking never overlap, so nothing here allocates.
    WiFiClient* stream = http.getStreamPtr();
    size_t received = 0;
    unsigned long lastData = millis();
    int lastFault = 0;
    size_t nextLog = 32768;
    const char* reason = "complete";

    while (len <= 0 || received < (size_t)len) {
        int n = stream->read(s_bodyBuf, sizeof(s_bodyBuf));

        if (n > 0) {
            if (out.write(s_bodyBuf, (size_t)n) != (size_t)n) {
                reason = "fs write failed";
                break;
            }
            received += (size_t)n;
            lastData = millis();
            if (received >= nextLog) {
                Serial.printf("download: %u/%d bytes (free=%u)\n",
                              (unsigned)received, len, ESP.getFreeHeap());
                nextLog += 32768;
            }
            continue;
        }

        // -1 just means nothing is buffered yet; anything lower is a real mbedTLS fault.
        if (n < -1 && n != lastFault) {
            lastFault = n;
            Serial.printf("download: mbedtls %d (-0x%04X) at %u bytes\n",
                          n, (unsigned)(-n), (unsigned)received);
        }
        if (stream->available() <= 0 && !stream->connected()) {
            reason = "peer closed";
            break;
        }
        if (millis() - lastData > 15000) {
            reason = "stalled";
            break;
        }
        apiProgressTick();
        delay(2);
    }

    out.close();
    http.end();
    downloadTls.stop();

    if (len > 0 && received != (size_t)len) {
        Serial.printf("download: %s at %u/%d bytes (mbedtls=%d errno=%d free=%u)\n",
                      reason, (unsigned)received, len, lastFault, errno, ESP.getFreeHeap());
        LittleFS.remove(path);
        return false;
    }

    Serial.printf("download: saved %u bytes\n", (unsigned)received);
    return received > 44;
}

bool ApiClient::markPlayed(const char* messageId) {
    ApiLock lock;
    if (!lock.held()) {
        return false;
    }
    releaseConnections();
    delay(200);

    WiFiClientSecure tls;
    tls.setInsecure();

    HTTPClient http;
    String path = String("/api/v1/device/messages/") + messageId + "/played";
    ApiEndpoint ep = parseEndpoint(path.c_str());
    configureHttp(http);
    Serial.printf("api: %s:%u%s\n", ep.host.c_str(), ep.port, ep.uri.c_str());
    if (!http.begin(tls, ep.host, ep.port, ep.uri)) {
        Serial.println("played: begin failed");
        return false;
    }
    setAuth(http);
    int code = http.POST("");
    logHttpResult("played", code);
    http.end();
    tls.stop();
    return code == 200;
}
