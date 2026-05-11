#pragma once

#include <M5Unified.h>
#include "servo_mode.h"

// Blocks until the user selects a servo mode, saves it to NVS, and returns it.
inline ServoMode showServoSelectScreen() {
    auto& d = M5.Display;
    const int W = d.width();   // 320
    const int H = d.height();  // 240

    // ── Background ──────────────────────────────────────────────────────────
    d.fillScreen(TFT_BLACK);

    // Title
    d.setTextColor(TFT_WHITE);
    d.setTextSize(2);
    d.setCursor((W - 220) / 2, 14);
    d.print("Select Servo Type");

    d.setTextSize(1);
    d.setTextColor(0x7BEF); // light grey
    d.setCursor(30, 38);
    d.print("Touch a button to choose. Saved until changed.");

    // ── PWM button (left) ────────────────────────────────────────────────────
    const int BTN_Y = 58, BTN_H = 130, BTN_W = 140, BTN_GAP = 10;
    const int LEFT_X  = BTN_GAP;
    const int RIGHT_X = W / 2 + BTN_GAP / 2;

    d.fillRoundRect(LEFT_X, BTN_Y, BTN_W, BTN_H, 10, 0x1A5F);  // deep blue
    d.setTextColor(TFT_WHITE);
    d.setTextSize(3);
    d.setCursor(LEFT_X + 22, BTN_Y + 28);
    d.print("PWM");
    d.setTextSize(1);
    d.setTextColor(0xC618);  // light grey
    d.setCursor(LEFT_X + 10, BTN_Y + 70);
    d.print("Analog servos");
    d.setCursor(LEFT_X + 10, BTN_Y + 85);
    d.print("e.g. SG90");
    d.setCursor(LEFT_X + 10, BTN_Y + 100);
    d.print("GPIO 33 / 32");

    // ── TTL SCS button (right) ───────────────────────────────────────────────
    d.fillRoundRect(RIGHT_X, BTN_Y, BTN_W, BTN_H, 10, 0x0480);  // deep green
    d.setTextColor(TFT_WHITE);
    d.setTextSize(2);
    d.setCursor(RIGHT_X + 10, BTN_Y + 28);
    d.print("TTL SCS");
    d.setTextSize(1);
    d.setTextColor(0xC618);
    d.setCursor(RIGHT_X + 10, BTN_Y + 70);
    d.print("Serial smart");
    d.setCursor(RIGHT_X + 10, BTN_Y + 85);
    d.print("e.g. SCS0009");
    d.setCursor(RIGHT_X + 10, BTN_Y + 100);
    d.print("UART GPIO 13/14");

    // ── Hint ─────────────────────────────────────────────────────────────────
    d.setTextColor(0x4228);  // dim grey
    d.setTextSize(1);
    d.setCursor(28, H - 22);
    d.print("Hold BtnA at boot to show this screen again.");

    // ── Wait for touch ───────────────────────────────────────────────────────
    while (true) {
        M5.update();
        auto t = M5.Touch.getDetail();
        if (t.wasPressed() && t.y >= BTN_Y && t.y <= BTN_Y + BTN_H) {
            ServoMode chosen = (t.x < W / 2) ? ServoMode::PWM : ServoMode::TTL_SCS;

            // ── Confirmation flash ────────────────────────────────────────────
            d.fillScreen(TFT_BLACK);
            d.setTextColor(TFT_GREEN);
            d.setTextSize(2);
            const char* label = (chosen == ServoMode::PWM) ? "PWM selected" : "TTL SCS selected";
            d.setCursor((W - (int)strlen(label) * 12) / 2, H / 2 - 10);
            d.print(label);
            delay(800);

            saveServoMode(chosen);
            return chosen;
        }
        delay(10);
    }
}
