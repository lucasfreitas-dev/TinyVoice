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
#include "storage_lock.h"
#include <WiFi.h>

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
bool littlefsReady = false;

struct UploadJob {
    uint8_t wavHeader[44];
    size_t pcmBytes;
    int chunkCount;
};

static UploadJob s_uploadJob;
static volatile bool s_uploadRequested = false;
static volatile bool s_uploadFinished = false;
static volatile bool s_uploadSuccess = false;
static TaskHandle_t s_uploadTask = nullptr;

static unsigned long s_apiBackoffUntil = 0;
static int s_apiFailStreak = 0;

static void noteApiSuccess() {
    s_apiFailStreak = 0;
    s_apiBackoffUntil = 0;
}

static void noteApiFailure() {
    s_apiFailStreak++;
    if (s_apiFailStreak >= 3) {
        s_apiBackoffUntil = millis() + 60000;
        s_apiFailStreak = 0;
        Serial.println("api: offline, backing off 60s (start Docker on the host)");
    }
}

static bool apiCallsAllowed() {
    return millis() >= s_apiBackoffUntil;
}

static void uploadTaskEntry(void* arg) {
    (void)arg;
    UploadJob job = s_uploadJob;
    s_uploadSuccess = apiClient.uploadRecording(job.wavHeader, job.pcmBytes, job.chunkCount);
    s_uploadFinished = true;
    s_uploadTask = nullptr;
    vTaskDelete(nullptr);
}

static void requestUpload(const uint8_t* wavHeader, size_t pcmBytes, int chunkCount) {
    memcpy(s_uploadJob.wavHeader, wavHeader, 44);
    s_uploadJob.pcmBytes = pcmBytes;
    s_uploadJob.chunkCount = chunkCount;
    s_uploadFinished = false;
    s_uploadSuccess = false;
    s_uploadRequested = true;
}

void handleUploading() {
    if (s_uploadRequested && s_uploadTask == nullptr) {
        s_uploadRequested = false;
        Serial.printf("upload: starting (%u pcm bytes, %d chunks)\n",
                      (unsigned)s_uploadJob.pcmBytes, s_uploadJob.chunkCount);
        Serial.flush();
        xTaskCreatePinnedToCore(
            uploadTaskEntry,
            "upload",
            12288,
            nullptr,
            5,
            &s_uploadTask,
            0
        );
    }

    if (s_uploadFinished && s_uploadTask == nullptr) {
        s_uploadFinished = false;
        Serial.printf("upload: %s (%u bytes)\n",
                      s_uploadSuccess ? "ok" : "failed",
                      (unsigned)(44 + s_uploadJob.pcmBytes));
        Serial.flush();
        if (s_uploadSuccess) {
            stateMachine.onUploadSuccess();
            audioRecorder.cleanupRecording();
        } else {
            stateMachine.onUploadFailed();
        }
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        Serial.printf("state: %s\n", stateToString(stateMachine.current()));
    }
}

bool mountLittleFS() {
    if (littlefsReady) {
        return true;
    }

    if (LittleFS.begin(false)) {
        littlefsReady = true;
        storageLockInit();
        Serial.printf("LittleFS: %u / %u bytes free\n",
                      (unsigned)storageFreeBytes(),
                      (unsigned)LittleFS.totalBytes());
        return true;
    }

    Serial.println("LittleFS mount failed, formatting...");
    if (!LittleFS.format()) {
        Serial.println("LittleFS format failed");
        return false;
    }

    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed after format");
        return false;
    }

    littlefsReady = true;
    storageLockInit();
    Serial.printf("LittleFS: %u / %u bytes free\n",
                  (unsigned)storageFreeBytes(),
                  (unsigned)LittleFS.totalBytes());
    return true;
}

void ensureStorageDirs() {
    if (!mountLittleFS()) {
        return;
    }
    if (!LittleFS.exists(QUEUE_DIR)) {
        LittleFS.mkdir(QUEUE_DIR);
    }
    if (!LittleFS.exists("/rec")) {
        LittleFS.mkdir("/rec");
    }
}

bool queueUploadFile(const char* path) {
    ensureStorageDirs();
    if (!storageLock()) {
        return false;
    }
    File src = LittleFS.open(path, FILE_READ);
    if (!src) {
        storageUnlock();
        return false;
    }

    char dest[32];
    snprintf(dest, sizeof(dest), "%s/%lu.wav", QUEUE_DIR, millis());
    File dst = LittleFS.open(dest, FILE_WRITE);
    if (!dst) {
        src.close();
        storageUnlock();
        return false;
    }

    uint8_t buf[512];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        if (n == 0) break;
        dst.write(buf, n);
    }
    src.close();
    dst.close();
    storageUnlock();
    Serial.printf("queued upload: %s\n", dest);
    return true;
}

void processQueue() {
    ensureStorageDirs();
    File root = LittleFS.open(QUEUE_DIR);
    if (!root || !root.isDirectory()) return;

    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            char path[48];
            snprintf(path, sizeof(path), "%s/%s", QUEUE_DIR, entry.name());
            entry.close();
            if (apiClient.uploadAudioFile(path)) {
                LittleFS.remove(path);
                Serial.println("queued upload sent");
            }
            root.close();
            return;
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
    ensureStorageDirs();
    storageCleanupRecDir();

    stateMachine.onBootComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());

    // Grab the recording buffer before Wi-Fi consumes contiguous heap.
    if (!audioRecorder.begin()) {
        Serial.println("audio init failed");
        stateMachine.onError("audio_init_failed");
    }
    if (audioPlayer.begin()) {
        audioPlayer.playBootChime();
    }

    wifiManager.begin();
    if (wifiManager.connect()) {
        stateMachine.onWiFiConnected();
        if (apiClient.heartbeat()) {
            noteApiSuccess();
        } else {
            noteApiFailure();
        }
    } else {
        stateMachine.onWiFiFailed();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi: connected, ip=%s\n", WiFi.localIP().toString().c_str());
        Serial.printf("api target: %s\n", API_BASE_URL);
        if (!apiCallsAllowed()) {
            Serial.println("api: waiting for backoff to expire or Docker to start");
        }
    }

    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    Serial.printf("state: %s\n", stateToString(stateMachine.current()));
}

bool uploadPendingRecording() {
    if (s_uploadTask != nullptr || s_uploadRequested || s_uploadFinished) {
        return true;
    }
    if (!audioRecorder.hasPendingRecording()) {
        return false;
    }

    size_t pcmBytes = audioRecorder.recordedPcmBytes();
    uint8_t wavHeader[44];
    audioRecorder.buildWavHeader(wavHeader, pcmBytes);
    requestUpload(wavHeader, pcmBytes, audioRecorder.chunkCount());
    stateMachine.onUploadStart();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    return true;
}

void handleRecording() {
    audioRecorder.loop();

    if (button.wasRelease() || audioRecorder.maxDurationReached() || audioRecorder.diskFull()) {
        if (audioRecorder.diskFull()) {
            Serial.println("recording stopped: flash full");
        }
        size_t fileLen = 0;
        size_t pcmBytes = audioRecorder.stop(&fileLen);

        if (pcmBytes > 0 && fileLen > 0) {
            unsigned long durationMs = (pcmBytes / 2) * 1000UL / 16000UL;
            if (durationMs >= MIN_RECORDING_MS) {
                uint8_t wavHeader[44];
                audioRecorder.buildWavHeader(wavHeader, pcmBytes);
                stateMachine.onButtonRelease();
                requestUpload(wavHeader, pcmBytes, audioRecorder.chunkCount());
                led.update(stateMachine.current(), stateMachine.hasPendingMessage());
                Serial.printf("state: %s\n", stateToString(stateMachine.current()));
                Serial.flush();
            } else {
                Serial.println("recording too short, cancelled");
                audioRecorder.cleanupRecording();
                stateMachine.onRecordingCancelled();
            }
        } else {
            Serial.println("recording empty, cancelled");
            audioRecorder.cleanupRecording();
            stateMachine.onRecordingCancelled();
        }

        if (stateMachine.current() != DeviceState::UPLOADING) {
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }
}

static const char* INBOUND_PLAY_PATH = "/play.wav";

void handleDownloadAndPlay() {
    if (!apiClient.downloadAudioToFile(pendingMessageId, INBOUND_PLAY_PATH)) {
        Serial.println("download: failed");
        stateMachine.onDownloadFailed();
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        return;
    }

    stateMachine.onDownloadComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());

    if (!audioPlayer.playFile(INBOUND_PLAY_PATH)) {
        Serial.println("audio: playback failed");
    }
    LittleFS.remove(INBOUND_PLAY_PATH);

    apiClient.markPlayed(pendingMessageId);
    pendingMessageId[0] = '\0';
    stateMachine.onPlaybackComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
}

void loop() {
    wifiManager.loop();
    button.loop();
    led.setWiFiConnected(wifiManager.isConnected());
    led.loop();

    DeviceState state = stateMachine.current();

    // Recover from ERROR once Wi-Fi is back
    if (state == DeviceState::ERROR && wifiManager.isConnected()) {
        Serial.println("wifi recovered, back to IDLE");
        stateMachine.onWiFiConnected();
        state = stateMachine.current();
        led.update(state, stateMachine.hasPendingMessage());
    }

    if (!wifiManager.isConnected()) {
        if (state != DeviceState::ERROR) {
            stateMachine.onWiFiFailed();
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }

    bool buttonActive = button.isPressed() || button.isHeld();

    // Light button LED whenever the switch is pressed (except during playback)
    if (state != DeviceState::PLAYING && state != DeviceState::RECORDING) {
        led.setPressedHint(button.isPressed());
    } else {
        led.setPressedHint(false);
    }

    // Button: hold to record (works whenever Wi-Fi is up and not busy uploading/playing)
    if (button.wasJustHeld() &&
        wifiManager.isConnected() &&
        (state == DeviceState::IDLE ||
         state == DeviceState::CHECKING_MESSAGES ||
         state == DeviceState::ERROR)) {
        stateMachine.onButtonHoldStart();
        if (stateMachine.current() == DeviceState::RECORDING) {
            if (!audioRecorder.start()) {
                Serial.println("recording start failed");
                stateMachine.onRecordingCancelled();
            } else {
                Serial.println("recording started");
            }
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
        audioRecorder.loop();
    }

    if (stateMachine.current() == DeviceState::UPLOADING) {
        handleUploading();
    }

    // Heartbeat every 60s
    if (apiCallsAllowed() &&
        wifiManager.isConnected() &&
        millis() - lastHeartbeatMs > 60000) {
        lastHeartbeatMs = millis();
        if (apiClient.heartbeat()) {
            noteApiSuccess();
        } else {
            noteApiFailure();
        }
    }

    // Poll for messages (skip while button is in use so HTTP does not block input)
    if (apiCallsAllowed() &&
        !buttonActive &&
        wifiManager.isConnected() &&
        millis() - lastPollMs > POLL_INTERVAL_MS &&
        stateMachine.current() == DeviceState::IDLE) {
        lastPollMs = millis();
        stateMachine.onPollStart();
        NextMessage msg;
        if (apiClient.pollNext(msg)) {
            noteApiSuccess();
            if (msg.available) {
                strlcpy(pendingMessageId, msg.id, sizeof(pendingMessageId));
            }
            stateMachine.onPollComplete(msg.available);
        } else {
            noteApiFailure();
            stateMachine.onPollComplete(stateMachine.hasPendingMessage());
        }
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    }

    // Retry pending chunk recording or legacy queue uploads when idle
    if (!buttonActive &&
        stateMachine.current() == DeviceState::IDLE &&
        wifiManager.isConnected() &&
        s_uploadTask == nullptr &&
        !s_uploadRequested &&
        !s_uploadFinished) {
        if (!uploadPendingRecording()) {
            processQueue();
        }
    }

    if (stateMachine.current() != DeviceState::RECORDING) {
        delay(10);
    }
}
