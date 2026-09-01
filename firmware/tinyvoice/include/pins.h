#pragma once

// Central GPIO map for TinyVoice prototype (ESP32-WROOM-32 dev board).
// See docs/hardware.md for full wiring diagram.

struct AudioPins {
    int micSck        = 14;
    int micWs         = 15;
    int micSd         = 32;
    int speakerBclk   = 27;
    int speakerLrc    = 26;
    int speakerDin    = 25;
};

constexpr int BUTTON_PIN    = 4;
constexpr int LED_GREEN_PIN = 17;  // arcade button built-in LED (+ 220Ω)
constexpr int LED_RED_PIN   = 16;  // optional external RGB
constexpr int LED_BLUE_PIN  = 18;  // optional external RGB
constexpr int VOLUME_POT_PIN = 34;  // ADC1_CH6 — analog volume pot (input-only)
