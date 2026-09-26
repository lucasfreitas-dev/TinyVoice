#pragma once

#include <stddef.h>
#include <stdint.h>

enum class LogLevel : uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

struct RemoteLogEntry {
    unsigned long tsMs;
    LogLevel level;
    char msg[160];
};

void remoteLog(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int remoteLogPendingCount();
bool remoteLogAlmostFull();
int remoteLogCopyPending(RemoteLogEntry* out, int max);
void remoteLogConsume(int n);
const char* logLevelName(LogLevel level);

#define TV_LOG(...) remoteLog(LogLevel::Info, __VA_ARGS__)
#define TV_WARN(...) remoteLog(LogLevel::Warn, __VA_ARGS__)
#define TV_ERROR(...) remoteLog(LogLevel::Error, __VA_ARGS__)
