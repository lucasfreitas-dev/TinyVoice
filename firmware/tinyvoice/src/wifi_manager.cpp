#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>

static unsigned long s_lastReconnectMs = 0;

void WiFiManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
}

bool WiFiManager::connect() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void WiFiManager::reconnect() {
    if (isConnected()) {
        return;
    }
    unsigned long now = millis();
    if (now - s_lastReconnectMs < 5000) {
        return;
    }
    s_lastReconnectMs = now;
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void WiFiManager::loop() {
    reconnect();
}
