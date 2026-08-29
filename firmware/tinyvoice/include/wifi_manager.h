#pragma once

#include <WiFi.h>

class WiFiManager {
public:
    void begin();
    bool connect();
    bool isConnected();
    void reconnect();
    void loop();
};
