#include "state_machine.h"
#include <string.h>

const char* stateToString(DeviceState state) {
    switch (state) {
        case DeviceState::BOOT: return "BOOT";
        case DeviceState::CONNECTING_WIFI: return "CONNECTING_WIFI";
        case DeviceState::IDLE: return "IDLE";
        case DeviceState::RECORDING: return "RECORDING";
        case DeviceState::UPLOADING: return "UPLOADING";
        case DeviceState::CHECKING_MESSAGES: return "CHECKING_MESSAGES";
        case DeviceState::DOWNLOADING: return "DOWNLOADING";
        case DeviceState::PLAYING: return "PLAYING";
        case DeviceState::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

StateMachine::StateMachine()
    : _state(DeviceState::BOOT), _hasPendingMessage(false) {
    _lastError[0] = '\0';
}

DeviceState StateMachine::current() const { return _state; }
bool StateMachine::hasPendingMessage() const { return _hasPendingMessage; }
const char* StateMachine::lastError() const { return _lastError; }

void StateMachine::transition(DeviceState next) {
    _state = next;
}

void StateMachine::setError(const char* reason) {
    strncpy(_lastError, reason, sizeof(_lastError) - 1);
    _lastError[sizeof(_lastError) - 1] = '\0';
    transition(DeviceState::ERROR);
}

void StateMachine::onBootComplete() {
    transition(DeviceState::CONNECTING_WIFI);
}

void StateMachine::onWiFiConnected() {
    transition(DeviceState::IDLE);
}

void StateMachine::onWiFiFailed() {
    setError("wifi_failed");
}

void StateMachine::onButtonHoldStart() {
    if (_state == DeviceState::IDLE && !_hasPendingMessage) {
        transition(DeviceState::RECORDING);
    }
}

void StateMachine::onButtonRelease() {
    if (_state == DeviceState::RECORDING) {
        transition(DeviceState::UPLOADING);
    }
}

void StateMachine::onButtonShortPress() {
    if (_hasPendingMessage && (_state == DeviceState::IDLE || _state == DeviceState::CHECKING_MESSAGES)) {
        transition(DeviceState::DOWNLOADING);
    }
}

void StateMachine::onPollStart() {
    if (_state == DeviceState::IDLE) {
        transition(DeviceState::CHECKING_MESSAGES);
    }
}

void StateMachine::onPollComplete(bool messageAvailable) {
    _hasPendingMessage = messageAvailable;
    if (_state == DeviceState::CHECKING_MESSAGES) {
        transition(DeviceState::IDLE);
    }
}

void StateMachine::onUploadSuccess() {
    transition(DeviceState::IDLE);
}

void StateMachine::onUploadFailed() {
    transition(DeviceState::IDLE);
}

void StateMachine::onDownloadComplete() {
    transition(DeviceState::PLAYING);
}

void StateMachine::onDownloadFailed() {
    transition(DeviceState::IDLE);
}

void StateMachine::onPlaybackComplete() {
    _hasPendingMessage = false;
    transition(DeviceState::IDLE);
}

void StateMachine::onRecordingCancelled() {
    transition(DeviceState::IDLE);
}

void StateMachine::onError(const char* reason) {
    setError(reason);
}
