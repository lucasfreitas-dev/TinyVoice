#pragma once

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "remote_log.h"

struct NextMessage {
    bool available;
    char id[40];
    int durationMs;
    long sizeBytes;
};

struct FirmwareOffer {
    bool updateAvailable;
    char version[24];
    long sizeBytes;
    char sha256[65];
};

typedef void (*ApiProgressFn)();

void setApiProgressHook(ApiProgressFn fn);

class ApiClient {
public:
    bool heartbeat(const RemoteLogEntry* logs, int logCount, FirmwareOffer& offer);
    bool uploadLogs(const RemoteLogEntry* logs, int logCount);
    bool downloadAndApplyFirmware(const FirmwareOffer& offer);
    bool pollNext(NextMessage& out);
    bool uploadAudio(const uint8_t* data, size_t len);
    bool uploadAudioFile(const char* path);
    // Sends the 44-byte WAV header followed by the raw PCM take, so nothing has to be
    // concatenated into a second file first.
    bool uploadRecordingPcm(const uint8_t* wavHeader, const char* pcmPath);
    bool downloadAudioToFile(const char* messageId, const char* path, bool* trimmed);
    bool markPlayed(const char* messageId);
    void releaseConnections();

private:
    void setAuth(HTTPClient& http);
};
