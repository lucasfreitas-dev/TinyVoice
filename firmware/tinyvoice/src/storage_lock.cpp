#include "storage_lock.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_mutex = nullptr;
static const char* QUEUE_DIR = "/queue";

void storageLockInit() {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

bool storageLock() {
    storageLockInit();
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) == pdTRUE;
}

void storageUnlock() {
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

size_t storageFreeBytes() {
    if (LittleFS.totalBytes() <= LittleFS.usedBytes()) {
        return 0;
    }
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

void storagePruneForRecording() {
    if (!storageLock()) {
        return;
    }

    if (LittleFS.totalBytes() == 0) {
        storageUnlock();
        return;
    }

    if (storageFreeBytes() >= 512000) {
        storageUnlock();
        return;
    }

    Serial.println("storage: pruning old files");

    File root = LittleFS.open(QUEUE_DIR);
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                char path[48];
                snprintf(path, sizeof(path), "%s/%s", QUEUE_DIR, entry.name());
                entry.close();
                LittleFS.remove(path);
            } else {
                entry.close();
            }
            entry = root.openNextFile();
        }
        root.close();
    }

    Serial.printf("storage: free %u / %u bytes\n",
                  (unsigned)storageFreeBytes(),
                  (unsigned)LittleFS.totalBytes());
    storageUnlock();
}

void storageCleanupRecDir() {
    if (!storageLock()) {
        return;
    }

    if (LittleFS.exists("/rec/upload.wav")) {
        LittleFS.remove("/rec/upload.wav");
    }

    File root = LittleFS.open("/rec");
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                char path[32];
                snprintf(path, sizeof(path), "/rec/%s", entry.name());
                entry.close();
                LittleFS.remove(path);
            } else {
                entry.close();
            }
            entry = root.openNextFile();
        }
        root.close();
    }

    storageUnlock();
}
