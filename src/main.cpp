/**
 * Stack-chan for M5Stack Core2
 *
 * Features:
 * - Animated face with expressions and servo head movement
 * - Servo: PWM (GPIO32/33) or TTL SCS0009 (UART GPIO13/14) — selected on first boot
 * - AI conversation via Gemini or OpenAI (configured in ai_config.h)
 *
 * Button map:
 *   BtnA        — Happy expression  /  hold at boot → servo type selector
 *               LOCAL mode: tap → LLM test  /  hold ≥1.5 s → cycle wake word
 *   BtnB        — Hold to record voice → send to AI
 *   BtnC        — Sleepy expression  /  tap to clear AI conversation history
 *
 * Touch zones (screen):
 *   Top third   — Happy + nod
 *   Mid left/right — look left/right
 *   Mid centre  — Sad
 *   Bottom      — Sleepy
 */

#include <M5Unified.h>
#include <Avatar.h>
#include <WiFi.h>
#include "servo_mode.h"
#include "init_screen.h"
#include "stackchan_servo.h"
#include "stackchan_audio.h"
#include "stackchan_expression.h"
#include "ai_config.h"
#include "config_store.h"
#include "config_webserver.h"
#include "audio_recorder.h"
#include "ai_client.h"
#include "tts_client.h"
#include "conversation_ui.h"
#include "wake_word.h"
#include "esp_task_wdt.h"
#include "avatar_faces.h"

using namespace m5avatar;

// FreeRTOS stack-overflow hook — fires when a task overflows its stack canary.
// Printed to serial then hard-reset so the device recovers instead of hanging silently.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
    Serial.printf("\n[FATAL] Stack overflow in task: %s — rebooting\n", pcTaskName);
    Serial.flush();
    esp_restart();
}

// ─── Web config server task (Core 0, pri 1) ───────────────────────────────────
// Keeps the HTTP config UI responsive even while loop() is busy with AI calls.
// handleConfigServer() is non-blocking; 20 ms is imperceptible for a config UI
// and saves ~175 unnecessary Core 0 context switches per second vs 5 ms.
static void webServerTask(void*) {
    for (;;) {
        handleConfigServer();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Forward declarations
void onTouchStart();
void onTouchEnd();
void handleAI();
void handleWakeWord();
void runAIPipeline(AudioRecording& rec);
void applyEmotion(const String& emotion);

// ============================================================================
// Configuration
// ============================================================================

// Servo limits — compile-time defaults; overridden at boot from g_servo_pan/tilt globals.
// The globals are loaded from /config.json by loadConfig() before setup() uses them.
#define PAN_MIN  g_servo_pan_min
#define PAN_MAX  g_servo_pan_max
#define TILT_MIN g_servo_tilt_min
#define TILT_MAX g_servo_tilt_max

constexpr uint32_t BLINK_INTERVAL_MIN  = 2000;
constexpr uint32_t BLINK_INTERVAL_MAX  = 5000;
constexpr uint32_t IDLE_MOVE_INTERVAL  = 3000;
constexpr uint32_t IDLE_BREATHE_AFTER  = 30000;  // switch to breathing after 30 s of no interaction

// ============================================================================
// Globals
// ============================================================================

Avatar             avatar;
StackchanServo     servoController;
StackchanAudio     audioController;
StackchanExpression expressionController;

bool              isInitialized   = false;
volatile bool     avatarRunning   = false;  // written by loop(), read by avatar task (pri 8)
uint32_t lastBlinkTime     = 0;
uint32_t nextBlinkInterval = 3000;
uint32_t lastIdleMove      = 0;
uint32_t lastInteraction   = 0;  // millis() of last button/touch/AI event — drives breathing

int  touchX = 0, touchY = 0;
bool isTouched = false, wasTouched = false;

// ============================================================================
// Avatar helpers
// ============================================================================

inline void startAvatar() {
    if (!avatarRunning) {
        if      (g_face_style == "cat")      avatar.setFace(new CatFace());
        else if (g_face_style == "capybara") avatar.setFace(new CapybaraFace());
        // else: library default face
        avatar.init(8);
        avatarRunning = true;
    }
}

inline void stopAvatar() {
    if (avatarRunning) {
        avatar.stop();
        delay(150);    // wait for avatar task to finish its current frame and release the SPI mutex
        avatarRunning = false;
    }
}

// Map the emotion string returned by the AI to an Avatar Expression.
void applyEmotion(const String& emotion) {
    Expression expr = Expression::Neutral;
    if      (emotion == "HAPPY")     { expr = Expression::Happy;    servoController.nod(); }
    else if (emotion == "SAD")       { expr = Expression::Sad;      servoController.tiltDown(); }
    else if (emotion == "ANGRY")     { expr = Expression::Angry;    servoController.shake(); }
    else if (emotion == "SLEEPY")    { expr = Expression::Sleepy;   servoController.tiltDown(); }
    else if (emotion == "SURPRISED") { expr = Expression::Doubt;    servoController.lookUp(); }
    else                             {                               servoController.center(); }
    expressionController.setExpression(expr);
}

// ============================================================================
// WiFi
// ============================================================================

void connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s\n", g_wifi_ssid.c_str());
    showWiFiConnecting();
    WiFi.begin(g_wifi_ssid.c_str(), g_wifi_pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300);
    }

    bool ok = WiFi.status() == WL_CONNECTED;
    if (ok) {
        // Disable DTIM power-save: without this the radio sleeps between beacon
        // intervals (~100 ms each), adding latency to every API packet exchange.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        // NTP sync — gives the AI accurate date/time context in the system prompt.
        // configTime(UTC offset s, DST offset s, NTP server)
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("[NTP] Time sync started (pool.ntp.org)");
    } else {
        // WiFi failed — start AP so the user can reach the config UI to fix credentials.
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);
        WiFi.softAP("stackchan-config");
        Serial.println("[WiFi] AP mode: SSID=stackchan-config  IP=192.168.4.1");
    }
    showWiFiStatus(ok);
    Serial.printf("[WiFi] %s\n", ok ? WiFi.localIP().toString().c_str() : "AP 192.168.4.1");
    delay(800);

    // Start config web server on either STA or AP network
    startConfigServer();

    // Show the IP/AP address briefly on screen so the user knows where to connect
    {
        auto& d = M5.Display;
        d.setTextColor(TFT_CYAN);
        d.setTextSize(1);
        d.setCursor(4, 145);
        if (ok) {
            d.printf("Config: http://%s", WiFi.localIP().toString().c_str());
        } else {
            d.print("Config: WiFi 'stackchan-config' -> 192.168.4.1");
        }
        delay(2000);
    }
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    loadConfig();   // reads /config.json from LittleFS; falls back to ai_config.h

    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_spk    = true;
    cfg.internal_mic    = true;   // enable mic for AI voice input
    cfg.external_spk    = false;
    M5.begin(cfg);

    // Bluetooth is never used — shut the stack down immediately to reclaim
    // ~40 KB heap and eliminate its background service tasks.
    btStop();

    Serial.println("Stack-chan initializing...");

    M5.Display.setRotation(1);
    M5.Display.setBrightness(g_brightness);
    M5.Display.fillScreen(TFT_BLACK);

    // ── Servo mode selection (first boot or hold BtnA) ────────────────────────
    M5.update();
    ServoMode servoMode = loadServoMode();
    if (servoMode == ServoMode::UNSET || M5.BtnA.isPressed()) {
        servoMode = showServoSelectScreen();
    }
    // LLM Module uses Port C (GPIO 13/14) at 115200 baud — same pins as TTL SCS servo.
    // Force PWM servo mode at runtime when the LOCAL provider is selected.
    if (g_ai_provider == "local" && servoMode == ServoMode::TTL_SCS) {
        Serial.println("[LocalLLM] Port C conflict: overriding TTL SCS → PWM servo mode");
        servoMode = ServoMode::PWM;
    }
    Serial.printf("[Servo] mode: %s\n", servoMode == ServoMode::PWM ? "PWM" : "TTL SCS");

    // ── Startup splash ────────────────────────────────────────────────────────
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(60, 90);
    M5.Display.println("Stack-chan");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.setCursor(68, 118);
    M5.Display.println("AI Companion");

    // ── WiFi ──────────────────────────────────────────────────────────────────
    connectWiFi();

    // ── Web config server task (Core 0) ───────────────────────────────────────
    // Started here — after startConfigServer() runs inside connectWiFi().
    // Keeps the HTTP UI alive throughout the session, including during AI calls.
    xTaskCreatePinnedToCore(webServerTask, "webcfg", 8192, nullptr, 1, nullptr, 0);

    // ── Local LLM Module (K144) ───────────────────────────────────────────────
    // Initialises LLM + ASR units only when LOCAL provider is selected.
    // localLLMStartTasks() pins inference + TTS to Core 0 so loop() stays free.
    if (g_ai_provider == "local" && localLLMBegin()) localLLMStartTasks();

    // ── Servos ───────────────────────────────────────────────────────────────
    servoController.begin(servoMode);
    servoController.setLimits(PAN_MIN, PAN_MAX, TILT_MIN, TILT_MAX);
    servoController.center();

    // ── Audio (speaker) ───────────────────────────────────────────────────────
    audioController.begin();

    // ── Avatar ───────────────────────────────────────────────────────────────
    startAvatar();
    // Scale=1.0 — SubtitleOverlay renders text below the mouth (sprite y=193–240)
    // in the same pixel space as the face, no scaling distortion.
    avatar.setScale(1.0);
    avatar.setPosition(0, 0);
    avatar.getFace()->setMouth(new SubtitleOverlay(avatar.getFace()->getMouth()));
    expressionController.begin(&avatar);
    expressionController.setThemeDefault();   // black background — avatar default palette is NOT black
    expressionController.setExpression(Expression::Neutral);

    // Enable lip-sync: avatar stays running during TTS, mouth driven by PCM amplitude
    setLipSyncAvatar(&avatar);
    // Give tts_client.h a way to stop/start the avatar around direct display writes
    // (e.g. showReaderUI when SAM produces 0 samples).
    setAvatarLifecycle([]{ stopAvatar(); }, []{ startAvatar(); });

    audioController.playStartup();

    // ── Spoken greeting ───────────────────────────────────────────────────────
    expressionController.setExpression(Expression::Happy);
    servoController.nod();
    delay(300);
    // Play greeting: MeloTTS when LOCAL provider is active and TTS initialised,
    // otherwise SAM. MeloTTS gives the same voice as all subsequent responses.
    if (g_ai_provider == "local" && s_melotts_ready) {
        localK144Speak(g_greeting.c_str());
    } else {
        samSpeak(g_greeting.c_str());
    }
    expressionController.setExpression(Expression::Neutral);
    servoController.center();

    lastInteraction  = millis();
    isInitialized    = true;
    nextBlinkInterval = random(BLINK_INTERVAL_MIN, BLINK_INTERVAL_MAX);
    Serial.println("[Stack-chan] Ready!");
}

// ============================================================================
// Touch handling
// ============================================================================

void handleTouch() {
    wasTouched = isTouched;
    auto touch = M5.Touch.getDetail();
    isTouched  = touch.isPressed();
    if (isTouched) { touchX = touch.x; touchY = touch.y; }

    if (isTouched  && !wasTouched) onTouchStart();
    if (!isTouched &&  wasTouched) onTouchEnd();
}

void onTouchStart() {
    lastInteraction = millis();
    int W = M5.Display.width(), H = M5.Display.height();
    if (touchY < H / 3) {
        expressionController.setExpression(Expression::Happy);
        audioController.playHappy();
        servoController.nod();
    } else if (touchY < (H * 2) / 3) {
        if (touchX < W / 3) {
            expressionController.setExpression(Expression::Doubt);
            servoController.lookLeft();
        } else if (touchX > (W * 2) / 3) {
            expressionController.setExpression(Expression::Doubt);
            servoController.lookRight();
        } else {
            expressionController.setExpression(Expression::Sad);
            audioController.playSurprise();
        }
    } else {
        expressionController.setExpression(Expression::Sleepy);
        audioController.playSleepy();
        servoController.tiltDown();
    }
}

void onTouchEnd() {
    delay(500);
    expressionController.setExpression(Expression::Neutral);
    servoController.center();
}

// ============================================================================
// AI conversation  (BtnB hold-to-record)
// ============================================================================

// ─── Shared AI pipeline ───────────────────────────────────────────────────────
// Called by both handleAI() (BtnB) and handleWakeWord() with a ready recording.
// Takes ownership of rec — always calls freeRecording() before returning.

void runAIPipeline(AudioRecording& rec) {
    lastInteraction = millis();
    Serial.printf("[MIC] Recorded %u samples (%.1f s)\n",
                  rec.count, rec.count / (float)RECORD_SAMPLE_RATE);

    // ── Build WAV ─────────────────────────────────────────────────────────────
    setSubtitleText("...");
    size_t   wav_size;
    uint8_t* wav = buildWav(rec, &wav_size);
    freeRecording(rec);

    if (!wav) {
        setSubtitleText("");
        stopAvatar();
        showErrorUI("WAV alloc failed");
        startAvatar();
        return;
    }

    // ── Send to AI ────────────────────────────────────────────────────────────
    AIChatResult result = aiChat(wav, wav_size);
    free(wav);
    setSubtitleText("");

    Serial.printf("[AI] emotion=%s transcript=%s\n",
                  result.emotion.c_str(), result.transcript.c_str());
    Serial.printf("[AI] response=%s\n", result.response.c_str());

    if (!result.success) {
        stopAvatar();
        showErrorUI(result.response);
        startAvatar();
        return;
    }

    // ── Show emotion and speak — avatar stays running for lip-sync ───────────
    applyEmotion(result.emotion);
    delay(400);
    // Realtime API plays response audio internally in realtimeChat() — skip TTS.
    if (g_ai_provider != "realtime") speakText(result.response.c_str());
    applyEmotion(result.emotion);
    delay(500);
    servoController.center();
}

void handleAI() {
    if (!M5.BtnB.isPressed()) return;

    // ── LOCAL provider: pipeline is autonomous (KWS→ASR→LLM→TTS on K144) ────
    // The K144 always listens for "HELLO". After wake word, ASR captures
    // speech and feeds it to LLM, which feeds to MeloTTS on K144 speaker.
    // Core2 monitors LLM output in localLLMUpdate() and handles emotion/TTS.
    // BtnB just shows the current status — no recording from Core2 mic.
    if (g_ai_provider == "local") {
        setSubtitleText("Say 'Hello' to wake K144");
        delay(1500);
        setSubtitleText("");
        return;
    }

    // ── Cloud providers: Core2 mic → cloud STT → cloud LLM ───────────────────
    if (WiFi.status() != WL_CONNECTED) {
        stopAvatar();
        showErrorUI("No WiFi — reconnecting...");
        connectWiFi();
        startAvatar();
        return;
    }

    // Keep avatar running — setSpeechText() is drawn by the avatar task itself.
    setSubtitleText("Listening...");
    AudioRecording rec = recordWhilePressed();
    setSubtitleText("");

    if (!rec.valid) {
        freeRecording(rec);
        return;
    }

    runAIPipeline(rec);
}

// ─── Wake-word handler ────────────────────────────────────────────────────────
// Polls for "Oye chispita" (or any utterance containing "chisp").
// BtnB takes priority — skip probe if the button is currently held.

void handleWakeWord() {
    if (M5.BtnB.isPressed()) return;  // BtnB path has priority
    if (WiFi.status() != WL_CONNECTED) return;

    if (!wakeWordCheck()) return;

    // ── Wake phrase confirmed ─────────────────────────────────────────────────
    Serial.println("[WAKE] Keyword detected — listening for command");

    // Acknowledge with a happy chirp and expression
    audioController.playHappy();
    expressionController.setExpression(Expression::Happy);
    servoController.nod();

    // Record the command (5 s window)
    setSubtitleText("Listening...");
    AudioRecording rec = recordTimedSeconds(5.0f);
    setSubtitleText("");

    if (!rec.valid) {
        freeRecording(rec);
        expressionController.setExpression(Expression::Neutral);
        servoController.center();
        return;
    }

    runAIPipeline(rec);
}

// ============================================================================
// Button handling
// ============================================================================

// ============================================================================
// Servo test  (BtnC long-hold)
//
// Walks through each axis extreme so both PWM and TTL SCS servos can be
// visually verified and calibrated.  The avatar speech bubble announces
// each step with the target angle — avatar stays running so setSpeechText()
// is used (safe from loop() without stopping the avatar task).
// ============================================================================

void runServoTest() {
    struct Step { int pan; int tilt; };
    const Step steps[] = {
        {                          90,                           90 },  // center
        {                     PAN_MIN,                           90 },  // pan left
        {                     PAN_MAX,                           90 },  // pan right
        {                          90,                           90 },  // center
        {                          90,                      TILT_MIN},  // tilt up
        {                          90,                      TILT_MAX},  // tilt down
        {                          90,                           90 },  // center
    };
    const char* names[] = {
        "Center",
        "Pan LEFT",
        "Pan RIGHT",
        "Center",
        "Tilt MIN",
        "Tilt MAX",
        "Done!",
    };
    const int N = sizeof(steps) / sizeof(steps[0]);

    Serial.println("[Servo] Test started");
    for (int i = 0; i < N; i++) {
        char label[24];
        // Show name + target angles — useful for PWM pulse-width calibration
        if (steps[i].pan != 90 || steps[i].tilt != 90) {
            snprintf(label, sizeof(label), "%s %d/%d",
                     names[i], steps[i].pan, steps[i].tilt);
        } else {
            snprintf(label, sizeof(label), "%s", names[i]);
        }

        setSubtitleText(label);
        servoController.moveTo(steps[i].pan, steps[i].tilt);
        Serial.printf("[Servo] %s  pan=%d tilt=%d\n",
                      names[i], steps[i].pan, steps[i].tilt);

        // Hold each position for 1.5 s — enough time to observe and measure.
        // Keep calling servoController.update() so PWM smooth-move completes.
        uint32_t t0 = millis();
        while (millis() - t0 < 1500) {
            M5.update();
            servoController.update();
            delay(10);
        }
    }
    setSubtitleText("");
    Serial.println("[Servo] Test done");
}

// ─── Wake word cycle (BtnA long-hold, LOCAL mode only) ───────────────────────
// Cycles through a preset list of GigaSpeech KWS-supported words, restarts the
// KWS service, and saves the new word to config.
// The list covers the most reliably recognised short English words on this model.
void handleWakeWordChange() {
    static const char* const WORDS[] = {
        "HELLO", "HEY", "OKAY", "STOP", "GO"
    };
    static const int N_WORDS = sizeof(WORDS) / sizeof(WORDS[0]);

    // Find current index and advance to the next word.
    int cur = 0;
    for (int i = 0; i < N_WORDS; i++) {
        if (g_kws_word.equalsIgnoreCase(WORDS[i])) { cur = i; break; }
    }
    int next = (cur + 1) % N_WORDS;
    const char* word = WORDS[next];

    // Show the new word in the speech bubble while KWS restarts.
    char label[24];
    snprintf(label, sizeof(label), "Wake: %s", word);
    setSubtitleText(label);

    if (localKWSRestart(String(word))) {
        Serial.printf("[KWS] Wake word changed to: %s\n", word);
        audioController.playHappy();
    } else {
        setSubtitleText("KWS fail");
    }
    delay(1500);
    setSubtitleText("");
}

void handleButtons() {
    // BtnA — short tap: Happy / LOCAL test  |  long-hold (≥1.5 s, LOCAL only): cycle wake word
    static uint32_t btnA_downAt = 0;
    static bool     btnA_used   = false;

    if (M5.BtnA.wasPressed()) {
        btnA_downAt = millis();
        btnA_used   = false;
        lastInteraction = millis();
    }
    if (M5.BtnA.isPressed() && !btnA_used &&
        g_ai_provider == "local" &&
        millis() - btnA_downAt >= 1500) {
        btnA_used = true;
        handleWakeWordChange();
    }
    if (M5.BtnA.wasReleased() && !btnA_used) {
        if (g_ai_provider == "local") {
            if (!s_pipeline_busy) {
                Serial.println("[Test] Dispatching greeting to local LLM...");
                if (localLLMDispatch(g_greeting)) setSubtitleText("...");
            }
        } else {
            expressionController.setExpression(Expression::Happy);
            audioController.playHappy();
            servoController.nod();
        }
    }

    // BtnB — handled by handleAI() (hold-to-record).
    // wasPressed() here would fire AFTER the hold ends; skip to avoid double action.

    // BtnC — short tap: clear AI history  /  hold ≥ 1.5 s: servo test
    static uint32_t btnC_downAt = 0;
    static bool     btnC_used   = false;

    if (M5.BtnC.wasPressed()) {
        btnC_downAt = millis();
        btnC_used   = false;
        lastInteraction = millis();
    }
    // Long-hold threshold: 1500 ms
    if (M5.BtnC.isPressed() && !btnC_used &&
        millis() - btnC_downAt >= 1500) {
        btnC_used = true;
        runServoTest();
    }
    if (M5.BtnC.wasReleased() && !btnC_used) {
        // Short tap — clear conversation history
        convClear();
        Serial.println("[Conv] History cleared");
        expressionController.setExpression(Expression::Sleepy);
        audioController.playSleepy();
        servoController.tiltDown();
    }
}

// ============================================================================
// Idle animations
// ============================================================================

void handleIdleAnimations() {
    uint32_t now = millis();

    if (now - lastBlinkTime > nextBlinkInterval) {
        lastBlinkTime     = now;
        nextBlinkInterval = random(BLINK_INTERVAL_MIN, BLINK_INTERVAL_MAX);
    }

    // Persistent WiFi-down badge via speech bubble (safe — avatar-task API)
    static uint32_t wifiCheckLast = 0;
    static bool     wifiBadgeShown = false;
    if (now - wifiCheckLast > 5000) {
        wifiCheckLast = now;
        bool wifiOk = (WiFi.status() == WL_CONNECTED || g_ai_provider == "local");
        if (!wifiOk && !wifiBadgeShown) {
            setSubtitleText("No WiFi");
            wifiBadgeShown = true;
        } else if (wifiOk && wifiBadgeShown) {
            setSubtitleText("");
            wifiBadgeShown = false;
        }
    }

    bool idle = (now - lastInteraction > IDLE_BREATHE_AFTER);

    if (idle) {
        // Breathing mode — smooth sine-wave servo motion, no random jitter
        servoController.breathe();
    } else {
        // Active mode — occasional random glances
        if (now - lastIdleMove > IDLE_MOVE_INTERVAL) {
            servoController.moveRelative(random(-15, 16), random(-5, 6));
            float gx = random(-100, 101) / 100.0f;
            avatar.setRotation(gx * 10);
            lastIdleMove = now;
        }
    }
}

// ============================================================================
// Main loop
// ============================================================================

void loop() {
    if (!isInitialized) { delay(100); return; }

    M5.update();

    handleAI();           // check BtnB first (blocks during recording)
    // handleWakeWord();  // wake-word disabled — re-enable when VAD threshold is tuned
    handleTouch();
    handleButtons();
    // handleConfigServer() removed — running in webServerTask() on Core 0

    // localLLMUpdate() reads Serial2 for KWS/ASR events.
    // Skipped while the pipeline task is running inference (it owns Serial2 then).
    if (!s_pipeline_busy) localLLMUpdate();

    // ── K144 ASR ready → dispatch to pipeline task ────────────────────────────
    // localLLMUpdate() sets s_local_asr_ready when a transcript arrives.
    // Dispatch to pipeline task on Core 0; loop() remains free during inference.
    if (g_ai_provider == "local" && s_local_asr_ready && !s_pipeline_busy) {
        s_local_asr_ready = false;
        if (localLLMDispatch(s_local_last_asr)) {
            s_local_kws_fired = false;
            // Keep s_asr_accum text visible so the user can see what was recognised
            // while the LLM thinks. Cleared when the pipeline result arrives below.
        }
    }

    // ── Pipeline task result handler ──────────────────────────────────────────
    // s_pipeline_done goes true after inference + TTS complete on Core 0.
    // While pipeline is busy: show recognised transcript until TTS starts.
    // Once TTS is running, fireTTS() updates g_subtitle_text each sentence
    // via setSubtitleText() — no explicit update needed here.
    if (g_ai_provider == "local" && s_pipeline_busy) {
        if (g_subtitle_text.isEmpty() && s_asr_accum.length()) {
            setSubtitleText(s_asr_accum.c_str());
        }
    }

    if (g_ai_provider == "local" && s_pipeline_done) {
        s_pipeline_done = false;
        portMEMORY_BARRIER();  // ensure s_pipeline_raw read happens after done flag
        lastInteraction = millis();
        s_asr_accum = "";            // clear recognised text now that response has arrived
        setSubtitleText("");
        AIChatResult result = parseAIResponse(s_pipeline_raw);
        result.transcript   = s_pipeline_input;
        result.success      = true;
        convAdd("user",  s_pipeline_input.c_str());
        convAdd("model", result.response.c_str());
        applyEmotion(result.emotion);
        // Dispatch function call action from LLM (set by pipeline task via [ACT:xxx] tag)
        if (s_pending_action_valid) {
            s_pending_action_valid = false;
            String act = String(s_pending_action);
            Serial.printf("[Main] Function call: %s\n", act.c_str());
            if      (act == "nod")   servoController.nod();
            else if (act == "shake") servoController.shake();
            else if (act == "left")  servoController.lookLeft();
            else if (act == "right") servoController.lookRight();
            else if (act == "up")    servoController.lookUp();
        }
        delay(500);
        servoController.center();

        // Auto-wake hint: if the response STARTS with "hello" / "hola", prompt
        // the user to speak without repeating the wake word.
        // Whisper processes 30-second audio chunks — any ambient noise during
        // TTS playback ends up in the next chunk and gets mis-transcribed.
        // Auto-rearming Whisper is therefore unreliable and disabled.
        // For Sherpa (real-time KWS-gated) we just show a prompt bubble.
        if (!s_asr_is_whisper) {
            String lower = result.response;
            lower.toLowerCase();
            lower.trim();
            if (lower.startsWith("hello") || lower.startsWith("hola")) {
                setSubtitleText("Say HELLO to reply");
                Serial.println("[LocalLLM] Auto-wake hint: say HELLO to continue");
            }
        }
    }

    // Show "Listening..." while KWS has fired and we're waiting for speech.
    // Whisper processes 30-second audio chunks — transcript arrives up to 30 s after
    // KWS fires. Use 40 s for Whisper, 15 s for sherpa-ncnn (streams immediately).
    if (g_ai_provider == "local" && s_local_kws_fired && !s_pipeline_busy) {
        uint32_t asr_timeout_ms = s_asr_is_whisper ? 40000 : 8000;   // 8 s for sherpa (was 15)
        if (millis() - s_kws_fired_at > asr_timeout_ms) {
            Serial.printf("[LocalLLM] ASR timeout — no transcript in %lu s, resetting\n",
                          asr_timeout_ms / 1000);
            s_local_kws_fired = false;
            s_asr_accum = "";
            setSubtitleText("");
        } else if (!s_asr_accum.length()) {
            // No streaming text yet — show "Listening..."
            setSubtitleText("Listening...");
        }
        // If s_asr_accum has content, the block above already set it
    }

    // ── Auto-listen: re-arm Whisper after a question response ─────────────────
    // s_auto_listen_at is set by the pipeline task when the response ends with '?'.
    // We wait until the mute window clears before activating so no TTS audio
    // bleeds into the next Whisper chunk.
    if (g_ai_provider == "local" && s_asr_is_whisper &&
        s_auto_listen_at > 0 && millis() >= s_auto_listen_at &&
        !s_pipeline_busy && !s_local_kws_fired && millis() >= s_tts_mute_until) {
        s_auto_listen_at  = 0;
        s_local_kws_fired = true;
        s_kws_fired_at    = millis();
        setSubtitleText("Listening...");
        Serial.println("[LocalLLM] Auto-listen: Whisper armed after question response");
    }

    // ── K144 error recovery ───────────────────────────────────────────────────
    // If Serial2 timed out during inference, attempt re-init after 30 s.
    // Shows "K144 error" in the speech bubble while waiting.
    if (g_ai_provider == "local" && s_k144_error && !s_pipeline_busy) {
        static uint32_t k144ErrorAt = 0;
        if (k144ErrorAt == 0) {
            k144ErrorAt = millis();
            setSubtitleText("K144 error");
            Serial.println("[LocalLLM] Scheduling K144 re-init in 30 s...");
        }
        if (millis() - k144ErrorAt >= 30000) {
            Serial.println("[LocalLLM] Attempting K144 re-init...");
            s_k144_error = false;
            k144ErrorAt  = 0;
            s_llm_ready  = false;
            // localLLMBegin() calls showBootProgress() which writes directly to
            // the display — must stop the avatar first (SPI mutex constraint #1).
            stopAvatar();
            bool ok = localLLMBegin();
            startAvatar();
            // localLLMStartTasks() is a no-op if the llm_pipe task is already
            // running — it re-uses the existing task which loops back to
            // xSemaphoreTake() and picks up the new LLM context via globals.
            if (ok) {
                localLLMStartTasks();
                setSubtitleText("");
                Serial.println("[LocalLLM] Re-init OK");
            } else {
                setSubtitleText("K144: check cable");
                Serial.println("[LocalLLM] Re-init failed — check cable");
            }
        }
    }

    if (!isTouched) handleIdleAnimations();

    servoController.update();

    // Yield to the avatar task (same core, same priority).
    // When fully idle, sleep 20 ms — gives the avatar task 100% of its time-slice
    // without spinning. When work is in progress (pipeline running, KWS active,
    // or Serial2 data pending), keep the 10 ms cadence for responsive polling.
    bool busy = s_pipeline_busy || s_local_kws_fired || Serial2.available();
    vTaskDelay(pdMS_TO_TICKS(busy ? 10 : 20));
}
