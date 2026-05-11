#pragma once

#include <Preferences.h>

enum class ServoMode : uint8_t {
    UNSET   = 0,
    PWM     = 1,
    TTL_SCS = 2
};

inline ServoMode loadServoMode() {
    Preferences prefs;
    prefs.begin("stackchan", true);
    uint8_t v = prefs.getUChar("servo_mode", (uint8_t)ServoMode::UNSET);
    prefs.end();
    return (ServoMode)v;
}

inline void saveServoMode(ServoMode mode) {
    Preferences prefs;
    prefs.begin("stackchan", false);
    prefs.putUChar("servo_mode", (uint8_t)mode);
    prefs.end();
}

inline void clearServoMode() {
    Preferences prefs;
    prefs.begin("stackchan", false);
    prefs.remove("servo_mode");
    prefs.end();
}
