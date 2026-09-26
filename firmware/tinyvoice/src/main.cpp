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
#include "remote_log.h"
#include "version.h"
#include <WiFi.h>
#include <esp_system.h>

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
char announcedMessageId[40] = {0};
char playedMessageId[40] = {0};

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
    // Transient TLS aborts are common on this radio; a 60s blackout after 3
    // misses left the box mute while the API was still up.
    if (s_apiFailStreak >= 6) {
        s_apiBackoffUntil = millis() + 15000;
        s_apiFailStreak = 0;
        Serial.println("api: tls failing, backing off 15s");
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

enum class NetJob : uint8_t { NONE, POLL, DOWNLOAD, MARK_PLAYED, HEARTBEAT, LOGS, OTA };

static RemoteLogEntry s_logBatch[24];
static int s_logBatchCount = 0;
static FirmwareOffer s_firmwareOffer = {};
static bool s_otaPending = false;
static unsigned long s_otaRetryAfter = 0;
static volatile bool s_otaTaskDone = false;
static volatile bool s_otaOk = false;
static volatile bool s_logsTaskDone = false;
static volatile bool s_logsOk = false;
static FirmwareOffer s_heartbeatOffer = {};

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
    while (!apiCallsAllowed()) {
        led.loop();
        delay(50);
    }

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
        TV_LOG("upload: ok (%u bytes)", (unsigned)(44 + s_uploadJob.pcmBytes));
        stateMachine.onUploadSuccess();
        audioRecorder.cleanupRecording();
    } else {
        s_uploadRetryAfter = millis() + 30000;
        TV_ERROR("upload: failed (%u bytes)", (unsigned)(44 + s_uploadJob.pcmBytes));
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

static const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "other";
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    TV_LOG("TinyVoice boot version=%s reset=%s heap=%u",
           FIRMWARE_VERSION, resetReasonName(esp_reset_reason()), ESP.getFreeHeap());

    button.begin();
    led.begin();
    ensureStorageDirs();
    storageCleanupRecDir();
    // A crash mid-download leaves /play.wav filling the partition. exists() logs an
    // error when the file is missing, so this must not run from the idle queue loop.
    if (LittleFS.exists("/play.wav")) {
        LittleFS.remove("/play.wav");
        Serial.printf("storage: dropped leftover inbound file, free %u / %u\n",
                      (unsigned)storageFreeBytes(),
                      (unsigned)LittleFS.totalBytes());
    }

    stateMachine.onBootComplete();
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());

    // Grab the recording buffer before Wi-Fi consumes contiguous heap.
    if (!audioRecorder.begin()) {
        Serial.println("audio init failed");
        stateMachine.onError("audio_init_failed");
    }
    audioRecorder.setProgressTick(uploadProgressTick);
    if (audioPlayer.begin()) {
        audioPlayer.setProgressTick(uploadProgressTick);
        audioPlayer.playBootChime();
    }

    wifiManager.begin();
    if (wifiManager.connect()) {
        stateMachine.onWiFiConnected();
    } else {
        TV_ERROR("wifi: connect failed");
        stateMachine.onWiFiFailed();
    }

    if (WiFi.status() == WL_CONNECTED) {
        TV_LOG("wifi: connected ip=%s rssi=%d api=%s",
               WiFi.localIP().toString().c_str(), WiFi.RSSI(), API_BASE_URL);
        setApiProgressHook(uploadProgressTick);
        startNetWorker();
        lastPollMs = millis() - POLL_INTERVAL_MS;
        lastHeartbeatMs = millis() - 60000;
    }

    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    Serial.printf("state: %s\n", stateToString(stateMachine.current()));
}

bool uploadPendingRecording() {
    if (!apiCallsAllowed() || millis() < s_uploadRetryAfter) {
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

    if ((button.wasRelease() && !button.isPressed()) ||
        audioRecorder.maxDurationReached() ||
        audioRecorder.flashLimitReached()) {
        if (audioRecorder.flashLimitReached()) {
            Serial.println("recording stopped: flash limit");
        } else if (audioRecorder.maxDurationReached()) {
            Serial.println("recording stopped: max safe length");
        }
        // Fast pulse while stop() drains the ring and flushes LittleFS.
        stateMachine.onButtonRelease();
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        Serial.printf("state: %s\n", stateToString(stateMachine.current()));
        size_t fileLen = 0;
        size_t pcmBytes = audioRecorder.stop(&fileLen);

        if (pcmBytes > 0 && fileLen > 0) {
            unsigned long durationMs = (pcmBytes / 2) * 1000UL / 16000UL;
            if (durationMs >= MIN_RECORDING_MS) {
                uint8_t wavHeader[44];
                audioRecorder.buildWavHeader(wavHeader, pcmBytes);
                requestUpload(wavHeader, pcmBytes, audioRecorder.chunkCount());
                stateMachine.onUploadStart();
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

        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    }
}

static const char* INBOUND_PLAY_PATH = "/play.wav";

static volatile bool s_downloadTaskDone = false;
static volatile bool s_inboundReady = false;
static volatile bool s_inboundTrimmed = false;
static char s_downloadMessageId[40] = {0};

static volatile bool s_pollTaskDone = false;
static bool s_pollOk = false;
static NextMessage s_pollMsg = {};
static volatile bool s_heartbeatTaskDone = false;
static bool s_heartbeatOk = false;

static void runDownloadJob() {
    Serial.println("download: started");

    apiClient.releaseConnections();
    audioRecorder.releaseMemoryForNetwork();
    delay(200);
    Serial.printf("download: after memory release free=%u max=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    bool trimmed = false;
    bool ok = apiClient.downloadAudioToFile(s_downloadMessageId, INBOUND_PLAY_PATH, &trimmed);
    s_inboundTrimmed = trimmed;

    if (ok) {
        stateMachine.onDownloadComplete();
        s_inboundReady = true;
        TV_LOG("download: ready id=%s", s_downloadMessageId);
    } else {
        TV_ERROR("download: failed id=%s", s_downloadMessageId);
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
        } else if (job == NetJob::HEARTBEAT) {
            s_logBatchCount = remoteLogCopyPending(s_logBatch, 24);
            s_heartbeatOffer = {};
            s_heartbeatOk = apiClient.heartbeat(s_logBatch, s_logBatchCount, s_heartbeatOffer);
            s_heartbeatTaskDone = true;
        } else if (job == NetJob::MARK_PLAYED) {
            if (apiClient.markPlayed(s_downloadMessageId)) {
                s_downloadMessageId[0] = '\0';
            }
        } else if (job == NetJob::LOGS) {
            s_logsOk = apiClient.uploadLogs(s_logBatch, s_logBatchCount);
            s_logsTaskDone = true;
        } else if (job == NetJob::OTA) {
            s_otaOk = apiClient.downloadAndApplyFirmware(s_firmwareOffer);
            s_otaTaskDone = true;
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
        16384,
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

    led.setTrimHint(s_inboundTrimmed);
    led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    if (s_inboundTrimmed) {
        Serial.println("playback: message was trimmed to fit flash");
    }

    if (!audioPlayer.playFile(INBOUND_PLAY_PATH)) {
        Serial.println("audio: playback failed");
    } else if (s_downloadMessageId[0] != '\0') {
        strlcpy(playedMessageId, s_downloadMessageId, sizeof(playedMessageId));
        strlcpy(announcedMessageId, s_downloadMessageId, sizeof(announcedMessageId));
    }
    LittleFS.remove(INBOUND_PLAY_PATH);
    pendingMessageId[0] = '\0';
    s_inboundTrimmed = false;
    led.setTrimHint(false);
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
    audioPlayer.loop();
    led.setWiFiConnected(wifiManager.isConnected());
    led.loop();

    DeviceState state = stateMachine.current();

    // Recover from ERROR once Wi-Fi is back
    if (state == DeviceState::ERROR && wifiManager.isConnected()) {
        TV_LOG("wifi recovered, back to IDLE");
        stateMachine.onWiFiConnected();
        state = stateMachine.current();
        led.update(state, stateMachine.hasPendingMessage());
    }

    if (!wifiManager.isConnected()) {
        if (state != DeviceState::ERROR && state != DeviceState::UPDATING) {
            stateMachine.onWiFiFailed();
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
        }
    }

    bool buttonActive = button.isPressed() || button.isHeld();

    // Light button LED whenever the switch is pressed (except during owned states)
    if (state != DeviceState::PLAYING &&
        state != DeviceState::RECORDING &&
        state != DeviceState::PROCESSING &&
        state != DeviceState::UPLOADING &&
        state != DeviceState::DOWNLOADING &&
        state != DeviceState::UPDATING) {
        led.setPressedHint(button.isPressed());
    } else {
        led.setPressedHint(false);
    }

    // Capture into the ring as soon as the button is down so the hold threshold
    // and file setup cannot clip the first words. Discarded on a short press.
    const bool canArmRecord =
        wifiManager.isConnected() &&
        !stateMachine.hasPendingMessage() &&
        !audioRecorder.isRecording() &&
        (state == DeviceState::IDLE ||
         state == DeviceState::CHECKING_MESSAGES ||
         state == DeviceState::ERROR);
    if (button.isPressed() && canArmRecord) {
        audioRecorder.arm();
    } else if (!audioRecorder.isRecording()) {
        audioRecorder.disarm();
    }

    // Pending inbound: tap or hold plays. A hold must not record instead.
    if (stateMachine.hasPendingMessage() &&
        (button.wasShortPress() || button.wasJustHeld())) {
        DeviceState playState = stateMachine.current();
        if (playState == DeviceState::DOWNLOADING ||
            playState == DeviceState::PLAYING ||
            s_inboundReady) {
            Serial.println("button: play ignored, already playing");
        } else {
            Serial.println("button: play requested");
            stateMachine.onButtonShortPress();
            if (stateMachine.current() == DeviceState::DOWNLOADING) {
                handleDownloadAndPlay();
                led.update(stateMachine.current(), stateMachine.hasPendingMessage());
            }
        }
    }

    // Button: hold to record (only when no pending message to play).
    // Do not waitForNetIdle here: the ring is only 1 s, and a blocking poll
    // wait wraps it, which is what clipped the first words of the take.
    if (button.wasJustHeld() && canArmRecord) {
        stateMachine.onButtonHoldStart();
        if (stateMachine.current() == DeviceState::RECORDING) {
            if (!audioRecorder.start()) {
                Serial.println("recording start failed");
                audioRecorder.disarm();
                stateMachine.onRecordingCancelled();
            } else {
                TV_LOG("recording started");
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

    if (s_logsTaskDone) {
        s_logsTaskDone = false;
        if (s_logsOk) {
            remoteLogConsume(s_logBatchCount);
            noteApiSuccess();
        } else {
            noteApiFailure();
        }
        s_logBatchCount = 0;
    }

    if (s_otaTaskDone) {
        s_otaTaskDone = false;
        if (!s_otaOk) {
            TV_ERROR("ota: failed version=%s", s_firmwareOffer.version);
            s_otaPending = false;
            s_otaRetryAfter = millis() + 900000;
            stateMachine.onUpdateFailed();
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
            noteApiFailure();
        }
    }

    if (s_pollTaskDone) {
        s_pollTaskDone = false;
        if (s_pollOk) {
            noteApiSuccess();
            if (s_pollMsg.available) {
                if (playedMessageId[0] != '\0' &&
                    strcmp(playedMessageId, s_pollMsg.id) == 0) {
                    Serial.printf("poll: already played id=%s, retrying ack\n",
                                  s_pollMsg.id);
                    if (s_downloadMessageId[0] == '\0') {
                        strlcpy(s_downloadMessageId, s_pollMsg.id, sizeof(s_downloadMessageId));
                    }
                    requestNetJob(NetJob::MARK_PLAYED);
                    stateMachine.onPollComplete(false);
                } else {
                    bool isNew = strcmp(announcedMessageId, s_pollMsg.id) != 0;
                    strlcpy(pendingMessageId, s_pollMsg.id, sizeof(pendingMessageId));
                    Serial.printf("poll: message available id=%s\n", pendingMessageId);
                    if (isNew) {
                        strlcpy(announcedMessageId, s_pollMsg.id, sizeof(announcedMessageId));
                        audioPlayer.playRingtone();
                    } else {
                        Serial.printf("poll: still waiting to play id=%s\n", pendingMessageId);
                    }
                    stateMachine.onPollComplete(true);
                }
            } else {
                Serial.printf("poll: no message (free=%u max=%u)\n",
                              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
                stateMachine.onPollComplete(false);
            }
        } else {
            TV_WARN("poll: failed");
            noteApiFailure();
            stateMachine.onPollComplete(stateMachine.hasPendingMessage());
        }
        led.update(stateMachine.current(), stateMachine.hasPendingMessage());
    }

    if (s_heartbeatTaskDone) {
        s_heartbeatTaskDone = false;
        if (s_heartbeatOk) {
            if (s_logBatchCount > 0) {
                remoteLogConsume(s_logBatchCount);
            }
            noteApiSuccess();
            if (s_heartbeatOffer.updateAvailable) {
                s_firmwareOffer = s_heartbeatOffer;
                s_otaPending = true;
                TV_LOG("ota: offered %s (%ld bytes)",
                       s_heartbeatOffer.version, s_heartbeatOffer.sizeBytes);
            } else {
                s_otaPending = false;
            }
        } else {
            noteApiFailure();
        }
        s_logBatchCount = 0;
    }

    // Heartbeat every 60s — same worker as poll so TLS never overlaps.
    if (apiCallsAllowed() &&
        wifiManager.isConnected() &&
        stateMachine.current() != DeviceState::UPLOADING &&
        stateMachine.current() != DeviceState::DOWNLOADING &&
        stateMachine.current() != DeviceState::PLAYING &&
        stateMachine.current() != DeviceState::RECORDING &&
        stateMachine.current() != DeviceState::PROCESSING &&
        stateMachine.current() != DeviceState::UPDATING &&
        !buttonActive &&
        !s_netBusy &&
        millis() - lastHeartbeatMs > 60000) {
        lastHeartbeatMs = millis();
        s_heartbeatTaskDone = false;
        requestNetJob(NetJob::HEARTBEAT);
    }

    if (apiCallsAllowed() &&
        wifiManager.isConnected() &&
        stateMachine.current() == DeviceState::IDLE &&
        !buttonActive &&
        !s_netBusy &&
        !s_uploadRequested &&
        remoteLogAlmostFull() &&
        millis() >= s_uploadRetryAfter) {
        s_logBatchCount = remoteLogCopyPending(s_logBatch, 24);
        s_logsTaskDone = false;
        if (s_logBatchCount > 0 && !requestNetJob(NetJob::LOGS)) {
            s_logBatchCount = 0;
        }
    }

    if (s_otaPending &&
        apiCallsAllowed() &&
        wifiManager.isConnected() &&
        stateMachine.current() == DeviceState::IDLE &&
        !stateMachine.hasPendingMessage() &&
        !audioRecorder.hasPendingRecording() &&
        !buttonActive &&
        !s_netBusy &&
        !s_uploadRequested &&
        !s_inboundReady &&
        millis() >= s_otaRetryAfter &&
        millis() >= s_uploadRetryAfter) {
        stateMachine.onUpdateStart();
        if (stateMachine.current() == DeviceState::UPDATING) {
            TV_LOG("ota: starting %s", s_firmwareOffer.version);
            led.update(stateMachine.current(), stateMachine.hasPendingMessage());
            waitForNetIdle(30000);
            apiClient.releaseConnections();
            audioRecorder.releaseMemoryForNetwork();
            delay(200);
            s_otaTaskDone = false;
            if (!requestNetJob(NetJob::OTA)) {
                TV_ERROR("ota: worker busy");
                stateMachine.onUpdateFailed();
                led.update(stateMachine.current(), stateMachine.hasPendingMessage());
            }
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

    if (stateMachine.current() != DeviceState::RECORDING &&
        stateMachine.current() != DeviceState::PROCESSING) {
        delay(10);
    }
}
