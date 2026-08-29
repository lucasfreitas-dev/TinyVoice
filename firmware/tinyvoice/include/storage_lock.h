#pragma once

#include <stddef.h>

void storageLockInit();
bool storageLock();
void storageUnlock();
size_t storageFreeBytes();
void storagePruneForRecording();
void storageCleanupRecDir();
