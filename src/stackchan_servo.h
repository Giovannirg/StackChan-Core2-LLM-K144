/**
 * Stack-chan Servo Controller
 * Supports PWM servos (ESP32Servo) and TTL SCS serial servos (SCSCL / SCS0009).
 */

#ifndef STACKCHAN_SERVO_H
#define STACKCHAN_SERVO_H

#include <Arduino.h>
#include "servo_mode.h"

// ── PWM mode ─────────────────────────────────────────────────────────────────
#include <ESP32Servo.h>

// ── TTL SCS mode ─────────────────────────────────────────────────────────────
#include <SCSCL.h>

// SCS servo IDs as set on the physical servos (default Feetech IDs)
#define SCS_PAN_ID   1
#define SCS_TILT_ID  2

// UART wired through the MAX3485 on the Stack-chan board (Core2 Port C)
// GPIO 16/17 are reserved for PSRAM CS/CLK — do NOT use them.
// Port C (J4 connector) routes to GPIO 13 (RX) / GPIO 14 (TX) on the ESP32.
#define SCS_SERIAL   Serial2
#define SCS_RX_PIN   14
#define SCS_TX_PIN   13
#define SCS_BAUD     1000000   // SCS0009 default baud

// SCS0009 position range: 0–1023, 300° full range, 512 = centre (90°).
// Adjust SCS_CENTRE if the servo's physical centre differs.
#define SCS_CENTRE   512

inline int degToSCS(float deg) {
    int pos = (int)(SCS_CENTRE + (deg - 90.0f) * (1024.0f / 300.0f));
    return constrain(pos, 0, 1023);
}

// ─────────────────────────────────────────────────────────────────────────────

class StackchanServo {
public:
    StackchanServo()
        : mode(ServoMode::UNSET)
        , panMin(45), panMax(135)
        , tiltMin(70), tiltMax(110)
        , currentPan(90), currentTilt(90)
        , targetPan(90), targetTilt(90)
        , moveSpeed(2.0f)
        , isMoving(false)
        , nodState(0), nodStartTime(0)
        , shakeState(0), shakeStartTime(0)
    {}

    // PWM mode: panPin=33, tiltPin=32 for Stack-chan board Core2.
    // TTL mode: pins are fixed (SCS_RX_PIN / SCS_TX_PIN) — arguments ignored.
    void begin(ServoMode servoMode, uint8_t panPin = 33, uint8_t tiltPin = 32) {
        mode = servoMode;

        if (mode == ServoMode::PWM) {
            ESP32PWM::allocateTimer(0);
            ESP32PWM::allocateTimer(1);
            ESP32PWM::allocateTimer(2);
            ESP32PWM::allocateTimer(3);
            panServo.setPeriodHertz(50);
            tiltServo.setPeriodHertz(50);
            panServo.attach(panPin, 500, 2500);
            tiltServo.attach(tiltPin, 500, 2500);
            Serial.printf("PWM servos: Pan=GPIO%d  Tilt=GPIO%d\n", panPin, tiltPin);

        } else if (mode == ServoMode::TTL_SCS) {
            SCS_SERIAL.begin(SCS_BAUD, SERIAL_8N1, SCS_RX_PIN, SCS_TX_PIN);
            sc.pSerial = &SCS_SERIAL;
            Serial.printf("TTL SCS: Serial2  RX=GPIO%d  TX=GPIO%d  IDs: pan=%d tilt=%d\n",
                          SCS_RX_PIN, SCS_TX_PIN, SCS_PAN_ID, SCS_TILT_ID);
        }
    }

    void setLimits(int panMinDeg, int panMaxDeg, int tiltMinDeg, int tiltMaxDeg) {
        panMin  = panMinDeg;  panMax  = panMaxDeg;
        tiltMin = tiltMinDeg; tiltMax = tiltMaxDeg;
    }

    void setSpeed(float speed) { moveSpeed = constrain(speed, 0.1f, 10.0f); }

    void center() {
        targetPan  = (panMin  + panMax)  / 2;
        targetTilt = (tiltMin + tiltMax) / 2;
        isMoving = true;
    }

    void moveTo(int pan, int tilt) {
        targetPan  = constrain(pan,  panMin,  panMax);
        targetTilt = constrain(tilt, tiltMin, tiltMax);
        isMoving = true;
    }

    void moveRelative(int deltaPan, int deltaTilt) {
        targetPan  = constrain(currentPan  + deltaPan,  panMin,  panMax);
        targetTilt = constrain(currentTilt + deltaTilt, tiltMin, tiltMax);
        isMoving = true;
    }

    void lookLeft()  { targetPan = panMin + 20;   isMoving = true; }
    void lookRight() { targetPan = panMax - 20;   isMoving = true; }
    void lookUp()    { targetTilt = tiltMin + 10; isMoving = true; }
    void tiltDown()  { targetTilt = tiltMax - 10; isMoving = true; }

    void nod()     { nodState = 1;   nodStartTime = millis(); }
    void shake()   { shakeState = 1; shakeStartTime = millis(); }

    // Gentle idle breathing — slow sine-wave on tilt, subtle pan drift.
    // Call every loop() iteration when idle. Internally tracks phase with millis().
    void breathe() {
        float t    = millis() / 1000.0f;
        float tilt = ((tiltMin + tiltMax) / 2.0f) + sinf(t * 0.8f) * 4.0f;
        float pan  = ((panMin  + panMax)  / 2.0f) + sinf(t * 0.3f) * 6.0f;
        targetPan  = constrain((int)pan,  panMin,  panMax);
        targetTilt = constrain((int)tilt, tiltMin, tiltMax);
        isMoving   = true;
    }

    // Map a display pixel coordinate (0–319, 0–239) to servo angles and look there.
    // x=0 → look left (panMax), x=319 → look right (panMin).
    // y=0 → look up (tiltMin),  y=239 → look down (tiltMax).
    void lookAtScreenCoord(int x, int y) {
        const int W = 320, H = 240;
        // Invert x: left side of screen → pan right (from robot's perspective)
        float nx  = 1.0f - constrain(x, 0, W - 1) / (float)(W - 1);
        float ny  = constrain(y, 0, H - 1)         / (float)(H - 1);
        targetPan  = (int)(panMin  + nx * (panMax  - panMin));
        targetTilt = (int)(tiltMin + ny * (tiltMax - tiltMin));
        isMoving   = true;
    }

    void update() {
        handleNod();
        handleShake();

        if (!isMoving) return;

        if (mode == ServoMode::PWM) {
            bool panDone  = smoothMove(currentPan,  targetPan);
            bool tiltDone = smoothMove(currentTilt, targetTilt);
            panServo.write((int)currentPan);
            tiltServo.write((int)currentTilt);
            if (panDone && tiltDone) isMoving = false;

        } else if (mode == ServoMode::TTL_SCS) {
            // Let the servo's internal controller handle smooth motion.
            // Speed: 300 steps/sec ≈ comfortable movement.
            sc.WritePos(SCS_PAN_ID,  degToSCS(targetPan),  0, 300);
            sc.WritePos(SCS_TILT_ID, degToSCS(targetTilt), 0, 300);
            currentPan  = targetPan;
            currentTilt = targetTilt;
            isMoving = false;
        }
    }

    int  getPan()       const { return (int)currentPan; }
    int  getTilt()      const { return (int)currentTilt; }
    bool isAnimating()  const { return isMoving || nodState > 0 || shakeState > 0; }
    ServoMode getMode() const { return mode; }

private:
    ServoMode mode;
    Servo     panServo, tiltServo;   // PWM mode
    SCSCL     sc;                    // TTL SCS mode

    int   panMin, panMax, tiltMin, tiltMax;
    float currentPan, currentTilt;
    float targetPan,  targetTilt;
    float moveSpeed;
    bool  isMoving;

    int      nodState,   shakeState;
    uint32_t nodStartTime, shakeStartTime;

    bool smoothMove(float& current, float target) {
        float diff = target - current;
        if (fabsf(diff) < 0.5f) { current = target; return true; }
        current += (diff > 0 ? moveSpeed : -moveSpeed);
        return false;
    }

    void handleNod() {
        if (nodState == 0) return;
        uint32_t elapsed = millis() - nodStartTime;
        int ct = (tiltMin + tiltMax) / 2;
        switch (nodState) {
            case 1: targetTilt = ct + 15; if (elapsed > 150) { nodState = 2; nodStartTime = millis(); } break;
            case 2: targetTilt = ct - 10; if (elapsed > 150) { nodState = 3; nodStartTime = millis(); } break;
            case 3: targetTilt = ct;      if (elapsed > 100) { nodState = 0; } break;
        }
        isMoving = true;
    }

    void handleShake() {
        if (shakeState == 0) return;
        uint32_t elapsed = millis() - shakeStartTime;
        int cp = (panMin + panMax) / 2;
        switch (shakeState) {
            case 1: targetPan = cp - 20; if (elapsed > 100) { shakeState = 2; shakeStartTime = millis(); } break;
            case 2: targetPan = cp + 20; if (elapsed > 100) { shakeState = 3; shakeStartTime = millis(); } break;
            case 3: targetPan = cp - 15; if (elapsed > 100) { shakeState = 4; shakeStartTime = millis(); } break;
            case 4: targetPan = cp;      if (elapsed > 100) { shakeState = 0; } break;
        }
        isMoving = true;
    }
};

#endif // STACKCHAN_SERVO_H
