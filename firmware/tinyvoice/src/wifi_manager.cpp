#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>

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
    if (!isConnected()) {
        connect();
    }
}

void WiFiManager::loop() {
    reconnect();
}
