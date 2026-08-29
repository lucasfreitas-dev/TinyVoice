#pragma once

#include <ArduinoJson.h>
#include <HTTPClient.h>

struct NextMessage {
    bool available;
    char id[40];
    int durationMs;
    long sizeBytes;
};

class ApiClient {
public:
    bool heartbeat();
    bool pollNext(NextMessage& out);
    bool uploadAudio(const uint8_t* data, size_t len);
    bool downloadAudio(const char* messageId, uint8_t** outData, size_t* outLen);
    bool markPlayed(const char* messageId);

private:
    void setAuth(HTTPClient& http);
    String apiUrl(const char* path);
};
