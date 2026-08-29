#include <Arduino.h>
#include <LittleFS.h>
#include <FS.h>

#include "state_machine.h"
#include "wifi_manager.h"
#include "api_client.h"
#include "audio_recorder.h"
#include "audio_player.h"
#include "button.h"
#include "led.h"
#include "config.h"

StateMachine stateMachine;
WiFiManager wifiManager;
ApiClient apiClient;
AudioRecorder audioRecorder;
AudioPlayer audioPlayer;
Button button;
Led led;

unsigned long lastPollMs = 0;
unsigned long lastHeartbeatMs = 0;
char pendingMessageId[40] = {0};

// Local upload queue (simple filesystem queue)
const char* QUEUE_DIR = "/queue";

void ensureQueueDir() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed");
        return;
    }
    if (!LittleFS.exists(QUEUE_DIR)) {
        LittleFS.mkdir(QUEUE_DIR);
    }
}

bool queueUpload(const uint8_t* data, size_t len) {
    ensureQueueDir();
    char path[32];
    snprintf(path, sizeof(path), "%s/%lu.wav", QUEUE_DIR, millis());
    File f = LittleFS.open(path, FILE_WRITE);
    if (!f) return false;
    f.write(data, len);
    f.close();
    Serial.printf("queued upload: %s\n", path);
    return true;
}

void processQueue() {
    ensureQueueDir();
    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) return;

    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String path = entry.name();
            size_t len = entry.size();
            uint8_t* buf = (uint8_t*)malloc(len);
            if (buf) {
                entry.read(buf, len);
                entry.close();
                if (apiClient.uploadAudio(buf, len)) {
                    LittleFS.remove(path);
                    Serial.println("queued upload sent");
                }
                free(buf);
                root.close();
                return;
            }
        } else {
            entry.close();
        }
        entry = root.openNextFile();
    }
    root.close();
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("TinyVoice boot");

    button.begin();
    led.begin();
    ensureQueueDir();

    stateMachine.onBootComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());

    wifiManager.begin();
    if (wifiManager.connect()) {
        stateMachine.onWiFiConnected();
        apiClient.heartbeat();
    } else {
        stateMachine.onWiFiFailed();
    }

    audioRecorder.begin();
    audioPlayer.begin();

    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    Serial.printf("state: %s\n", stateToString(stateMachine.current()));
}

void handleRecording() {
    audioRecorder.loop();

    if (button.wasRelease() || millis() - lastPollMs > (unsigned long)MAX_RECORDING_SECONDS * 1000UL) {
        uint8_t* wav = nullptr;
        size_t wavLen = 0;
        size_t pcmBytes = audioRecorder.stop(&wav, &wavLen);

        if (pcmBytes > 0 && wavLen > 0) {
            unsigned long durationMs = (pcmBytes / 2) * 1000UL / 16000UL;
            if (durationMs >= MIN_RECORDING_MS) {
                stateMachine.onButtonRelease();
                led.update(stateMachine.current(), stateMachine.hasPendingMessage());

                bool ok = apiClient.uploadAudio(wav, wavLen);
                if (ok) {
                    stateMachine.onUploadSuccess();
                } else {
                    queueUpload(wav, wavLen);
                    stateMachine.onUploadFailed();
                }
            } else {
                stateMachine.onRecordingCancelled();
            }
        } else {
            stateMachine.onRecordingCancelled();
        }

        if (wav) free(wav);
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    }
}

void handleDownloadAndPlay() {
    uint8_t* audio = nullptr;
    size_t audioLen = 0;

    if (!apiClient.downloadAudio(pendingMessageId, &audio, &audioLen)) {
        stateMachine.onDownloadFailed();
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        return;
    }

    stateMachine.onDownloadComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());

    audioPlayer.play(audio, audioLen);
    free(audio);

    apiClient.markPlayed(pendingMessageId);
    pendingMessageId[0] = '\0';
    stateMachine.onPlaybackComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
}

void loop() {
    wifiManager.loop();
    button.loop();
    led.loop();

    if (!wifiManager.isConnected() && stateMachine.current() != DeviceState::ERROR) {
        stateMachine.onWiFiFailed();
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        delay(1000);
        wifiManager.connect();
        if (wifiManager.isConnected()) {
            stateMachine.onWiFiConnected();
        }
        return;
    }

    // Button: hold to record
    if (button.isHeld() && stateMachine.current() == DeviceState::IDLE) {
        stateMachine.onButtonHoldStart();
        if (stateMachine.current() == DeviceState::RECORDING) {
            audioRecorder.start();
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }

    // Button: short press to play
    if (button.wasShortPress() && stateMachine.hasPendingMessage()) {
        stateMachine.onButtonShortPress();
        if (stateMachine.current() == DeviceState::DOWNLOADING) {
            handleDownloadAndPlay();
        }
    }

    if (stateMachine.current() == DeviceState::RECORDING) {
        handleRecording();
    }

    // Heartbeat every 60s
    if (millis() - lastHeartbeatMs > 60000) {
        lastHeartbeatMs = millis();
        apiClient.heartbeat();
    }

    // Poll for messages
    if (millis() - lastPollMs > POLL_INTERVAL_MS) {
        lastPollMs = millis();
        if (stateMachine.current() == DeviceState::IDLE) {
            stateMachine.onPollStart();
            NextMessage msg;
            if (apiClient.pollNext(msg)) {
                if (msg.available) {
                    strlcpy(pendingMessageId, msg.id, sizeof(pendingMessageId));
                }
                stateMachine.onPollComplete(msg.available);
            } else {
                stateMachine.onPollComplete(stateMachine.hasPendingMessage());
            }
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }

    // Process offline queue when idle
    if (stateMachine.current() == DeviceState::IDLE && wifiManager.isConnected()) {
        processQueue();
    }

    delay(10);
}
