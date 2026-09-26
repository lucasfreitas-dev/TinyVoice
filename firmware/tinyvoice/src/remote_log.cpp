#include "remote_log.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int kRingSize = 24;
constexpr int kAlmostFull = 18;

RemoteLogEntry s_ring[kRingSize];
int s_head = 0;
int s_count = 0;

void push(LogLevel level, const char* msg) {
    int idx = (s_head + s_count) % kRingSize;
    if (s_count == kRingSize) {
        s_head = (s_head + 1) % kRingSize;
        idx = (s_head + s_count - 1) % kRingSize;
    } else {
        s_count++;
    }
    s_ring[idx].tsMs = millis();
    s_ring[idx].level = level;
    strlcpy(s_ring[idx].msg, msg, sizeof(s_ring[idx].msg));
}

}  // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
        default: return "info";
    }
}

void remoteLog(LogLevel level, const char* fmt, ...) {
    char buf[sizeof(RemoteLogEntry::msg)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.printf("%s\n", buf);
    push(level, buf);
}

int remoteLogPendingCount() {
    return s_count;
}

bool remoteLogAlmostFull() {
    return s_count >= kAlmostFull;
}

int remoteLogCopyPending(RemoteLogEntry* out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_ring[(s_head + i) % kRingSize];
    }
    return n;
}

void remoteLogConsume(int n) {
    if (n <= 0) {
        return;
    }
    if (n > s_count) {
        n = s_count;
    }
    s_head = (s_head + n) % kRingSize;
    s_count -= n;
}
