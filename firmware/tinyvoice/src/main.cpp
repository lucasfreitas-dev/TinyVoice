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
static unsigned long s_uploadRetryAfter = 0;

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
        Serial.println("api: unreachable, backing off 60s");
    }
}

static bool apiCallsAllowed() {
    return millis() >= s_apiBackoffUntil;
}

static void requestUpload(const uint8_t* wavHeader, size_t pcmBytes, int chunkCount) {
    memcpy(s_uploadJob.wavHeader, wavHeader, 44);
    s_uploadJob.pcmBytes = pcmBytes;
    s_uploadJob.chunkCount = chunkCount;
    s_uploadRequested = true;
}

enum class NetJob : uint8_t { NONE, POLL, DOWNLOAD, MARK_PLAYED };

// A single long-lived worker owns every background TLS call. Creating a task per poll or
// per download meant a tight heap could refuse the allocation, which is what left inbound
// messages unplayable ("download: task create failed").
static TaskHandle_t s_netTask = nullptr;
static volatile NetJob s_netJob = NetJob::NONE;
static volatile bool s_netBusy = false;

static void startNetWorker();

static bool requestNetJob(NetJob job) {
    if (s_netTask == nullptr || s_netBusy) {
        return false;
    }
    s_netBusy = true;
    s_netJob = job;
    return true;
}

static void waitForNetIdle(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (s_netBusy && millis() - start < timeoutMs) {
        led.loop();
        delay(25);
    }
}

static void uploadProgressTick() {
    led.loop();
}

static void runUploadJob() {
    waitForNetIdle(30000);

    Serial.printf("upload: starting (%u pcm bytes, %d chunks)\n",
                  (unsigned)s_uploadJob.pcmBytes, s_uploadJob.chunkCount);
    Serial.flush();

    apiClient.releaseConnections();
    audioRecorder.releaseMemoryForNetwork();
    delay(200);
    Serial.printf("upload: after memory release free=%u max=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    bool ok = apiClient.uploadRecordingPcm(s_uploadJob.wavHeader, audioRecorder.takePath());

    Serial.printf("upload: %s (%u bytes)\n",
                  ok ? "ok" : "failed",
                  (unsigned)(44 + s_uploadJob.pcmBytes));
    Serial.flush();

    if (ok) {
        s_uploadRetryAfter = 0;
        stateMachine.onUploadSuccess();
        audioRecorder.cleanupRecording();
    } else {
        s_uploadRetryAfter = millis() + 30000;
        apiClient.releaseConnections();
        delay(500);
        stateMachine.onUploadFailed();
    }
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    Serial.printf("state: %s\n", stateToString(stateMachine.current()));
}

void handleUploading() {
    if (!s_uploadRequested) {
        return;
    }
    s_uploadRequested = false;
    led.update(DeviceState::UPLOADING, stateMachine.hasPendingMessage());
    runUploadJob();
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
    } else {
        stateMachine.onWiFiFailed();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi: connected, ip=%s\n", WiFi.localIP().toString().c_str());
        Serial.printf("api target: %s\n", API_BASE_URL);
        setApiProgressHook(uploadProgressTick);
        startNetWorker();
        lastPollMs = millis() - POLL_INTERVAL_MS;
        lastHeartbeatMs = millis();
    }

    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    Serial.printf("state: %s\n", stateToString(stateMachine.current()));
}

bool uploadPendingRecording() {
    if (millis() < s_uploadRetryAfter) {
        return false;
    }
    if (s_uploadRequested) {
        return false;
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

    if (button.wasRelease() || audioRecorder.maxDurationReached() || audioRecorder.flashLimitReached()) {
        if (audioRecorder.flashLimitReached()) {
            Serial.println("recording stopped: flash limit (upload safe)");
        } else if (audioRecorder.maxDurationReached()) {
            Serial.println("recording stopped: max safe length");
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

static volatile bool s_downloadTaskDone = false;
static volatile bool s_inboundReady = false;
static char s_downloadMessageId[40] = {0};

static volatile bool s_pollTaskDone = false;
static bool s_pollOk = false;
static NextMessage s_pollMsg = {};

static void runDownloadJob() {
    Serial.println("download: started");

    apiClient.releaseConnections();
    audioRecorder.releaseMemoryForNetwork();
    delay(200);
    Serial.printf("download: after memory release free=%u max=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    bool ok = apiClient.downloadAudioToFile(s_downloadMessageId, INBOUND_PLAY_PATH);

    if (ok) {
        stateMachine.onDownloadComplete();
        s_inboundReady = true;
        Serial.println("download: file ready");
    } else {
        Serial.println("download: failed");
        stateMachine.onDownloadFailed();
    }

    s_downloadTaskDone = true;
}

static void netTaskEntry(void* arg) {
    (void)arg;
    for (;;) {
        NetJob job = s_netJob;
        if (job == NetJob::NONE) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (job == NetJob::POLL) {
            s_pollOk = apiClient.pollNext(s_pollMsg);
            s_pollTaskDone = true;
        } else if (job == NetJob::MARK_PLAYED) {
            apiClient.markPlayed(s_downloadMessageId);
            s_downloadMessageId[0] = '\0';
        } else {
            runDownloadJob();
        }

        s_netJob = NetJob::NONE;
        s_netBusy = false;
    }
}

static void startNetWorker() {
    static unsigned long retryAfter = 0;
    if (s_netTask != nullptr || millis() < retryAfter) {
        return;
    }
    retryAfter = millis() + 5000;
    BaseType_t created = xTaskCreatePinnedToCore(
        netTaskEntry,
        "net",
        8192,
        nullptr,
        2,
        &s_netTask,
        0
    );
    if (created != pdPASS) {
        Serial.println("net: worker create failed");
        s_netTask = nullptr;
    }
}

static void handleInboundPlayback() {
    if (!s_inboundReady) {
        return;
    }
    s_inboundReady = false;

    led.update(stateMachine.current(), stateMachine.hasPendingMessage());

    if (!audioPlayer.playFile(INBOUND_PLAY_PATH)) {
        Serial.println("audio: playback failed");
    }
    LittleFS.remove(INBOUND_PLAY_PATH);
    pendingMessageId[0] = '\0';
    stateMachine.onPlaybackComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    Serial.println("playback: complete");

    // Acking runs on the worker: a blocking TLS call here would freeze the state machine.
    if (!requestNetJob(NetJob::MARK_PLAYED)) {
        Serial.println("played: worker busy, will retry on next poll");
    }
}

static void startDownloadAndPlay() {
    if (pendingMessageId[0] == '\0') {
        Serial.println("download: no message id");
        stateMachine.onDownloadFailed();
        return;
    }
    strlcpy(s_downloadMessageId, pendingMessageId, sizeof(s_downloadMessageId));

    // A poll may be in flight; it finishes in a few seconds and both share the worker.
    waitForNetIdle(30000);
    s_downloadTaskDone = false;
    if (!requestNetJob(NetJob::DOWNLOAD)) {
        Serial.println("download: worker busy");
        stateMachine.onDownloadFailed();
    }
}

void handleDownloadAndPlay() {
    startDownloadAndPlay();
}

void loop() {
    wifiManager.loop();
    if (wifiManager.isConnected()) {
        startNetWorker();
    }
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

    // Button: short press to play pending message (quick tap < HOLD_THRESHOLD_MS)
    if (button.wasShortPress() && stateMachine.hasPendingMessage()) {
        Serial.println("button: play requested");
        stateMachine.onButtonShortPress();
        if (stateMachine.current() == DeviceState::DOWNLOADING) {
            handleDownloadAndPlay();
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }

    // Button: hold to record (only when no pending message to play)
    if (button.wasJustHeld() &&
        wifiManager.isConnected() &&
        !stateMachine.hasPendingMessage() &&
        (state == DeviceState::IDLE ||
         state == DeviceState::CHECKING_MESSAGES ||
         state == DeviceState::ERROR)) {
        stateMachine.onButtonHoldStart();
        if (stateMachine.current() == DeviceState::RECORDING) {
            waitForNetIdle(5000);
            if (!audioRecorder.start()) {
                Serial.println("recording start failed");
                stateMachine.onRecordingCancelled();
            } else {
                Serial.println("recording started");
            }
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }

    if (stateMachine.current() == DeviceState::RECORDING) {
        handleRecording();
        audioRecorder.loop();
    }

    if (stateMachine.current() == DeviceState::UPLOADING) {
        handleUploading();
    }

    if (s_downloadTaskDone) {
        s_downloadTaskDone = false;
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    }

    handleInboundPlayback();

    if (s_pollTaskDone) {
        s_pollTaskDone = false;
        if (s_pollOk) {
            noteApiSuccess();
            if (s_pollMsg.available) {
                strlcpy(pendingMessageId, s_pollMsg.id, sizeof(pendingMessageId));
                Serial.printf("poll: message available id=%s\n", pendingMessageId);
            } else {
                Serial.printf("poll: no message (free=%u max=%u)\n",
                              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            }
            stateMachine.onPollComplete(s_pollMsg.available);
        } else {
            noteApiFailure();
            stateMachine.onPollComplete(stateMachine.hasPendingMessage());
        }
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    }

    // Heartbeat every 60s (skip while the worker holds a TLS session)
    if (apiCallsAllowed() &&
        wifiManager.isConnected() &&
        stateMachine.current() != DeviceState::UPLOADING &&
        stateMachine.current() != DeviceState::DOWNLOADING &&
        stateMachine.current() != DeviceState::PLAYING &&
        stateMachine.current() != DeviceState::RECORDING &&
        !s_netBusy &&
        millis() - lastHeartbeatMs > 60000) {
        lastHeartbeatMs = millis();
        if (apiClient.heartbeat()) {
            noteApiSuccess();
        } else {
            noteApiFailure();
        }
    }

    // Poll for messages in background so button stays responsive
    if (apiCallsAllowed() &&
        !buttonActive &&
        wifiManager.isConnected() &&
        stateMachine.current() == DeviceState::IDLE &&
        !s_uploadRequested &&
        !s_netBusy &&
        !s_inboundReady &&
        millis() - lastPollMs > POLL_INTERVAL_MS &&
        millis() >= s_uploadRetryAfter) {
        lastPollMs = millis();
        s_pollTaskDone = false;
        requestNetJob(NetJob::POLL);
    }

    // Retry pending chunk recording or legacy queue uploads when idle
    if (!buttonActive &&
        stateMachine.current() == DeviceState::IDLE &&
        wifiManager.isConnected() &&
        !s_uploadRequested &&
        !s_netBusy) {
        if (!uploadPendingRecording()) {
            processQueue();
        }
    }

    if (stateMachine.current() != DeviceState::RECORDING) {
        delay(10);
    }
}
