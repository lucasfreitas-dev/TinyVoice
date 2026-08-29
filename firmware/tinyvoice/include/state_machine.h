#pragma once

enum class DeviceState {
    BOOT,
    CONNECTING_WIFI,
    IDLE,
    RECORDING,
    UPLOADING,
    CHECKING_MESSAGES,
    DOWNLOADING,
    PLAYING,
    ERROR
};

const char* stateToString(DeviceState state);

class StateMachine {
public:
    StateMachine();

    DeviceState current() const;
    bool hasPendingMessage() const;

    void onBootComplete();
    void onWiFiConnected();
    void onWiFiFailed();
    void onButtonHoldStart();
    void onButtonRelease();
    void onButtonShortPress();
    void onPollStart();
    void onPollComplete(bool messageAvailable);
    void onUploadSuccess();
    void onUploadFailed();
    void onDownloadComplete();
    void onDownloadFailed();
    void onPlaybackComplete();
    void onRecordingCancelled();
    void onError(const char* reason);

    const char* lastError() const;

private:
    DeviceState _state;
    bool _hasPendingMessage;
    char _lastError[64];

    void transition(DeviceState next);
    void setError(const char* reason);
};
