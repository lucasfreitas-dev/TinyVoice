#pragma once

// Copy this file to config.h and fill in your values.
// config.h is gitignored — never commit credentials.

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define API_BASE_URL "https://tinyvoice.example.com"
#define DEVICE_ID "00000000-0000-0000-0000-000000000000"
#define DEVICE_TOKEN "your-device-token"

// Recording limits
#define MIN_RECORDING_MS 500
#define MAX_RECORDING_SECONDS 60

// Polling interval for new messages (ms)
#define POLL_INTERVAL_MS 5000

// Button timing
#define HOLD_THRESHOLD_MS 300
#define DEBOUNCE_MS 50

// API
#define API_TIMEOUT_MS 30000
