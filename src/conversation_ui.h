#pragma once

#include <M5Unified.h>

// ─── Colour palette ───────────────────────────────────────────────────────────
static const uint16_t COL_BG       = 0x1082;  // very dark grey
static const uint16_t COL_USER_BG  = 0x2124;  // dark blue-grey
static const uint16_t COL_AI_TEXT  = 0xFFFF;  // white
static const uint16_t COL_HINT     = 0x632C;  // dim grey

// ─── Subtitle text — written by loop() / pipeline task, read by SubtitleOverlay ──
// SubtitleOverlay (in avatar_faces.h) reads this on every avatar frame and renders
// word-wrapped text in the bottom of the face sprite. Use setSubtitleText() to update.
static String g_subtitle_text = "";

inline void setSubtitleText(const char* t) {
    g_subtitle_text = (t && *t) ? t : "";
}

// ─── Recording overlay ────────────────────────────────────────────────────────
// Call from the recording loop to update the on-screen indicator.

inline void showRecordingUI(uint32_t elapsed_ms) {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);

    // Pulsing red dot
    bool blink = (elapsed_ms / 500) % 2 == 0;
    d.setTextColor(blink ? TFT_RED : 0x4000);
    d.setTextSize(2);
    d.setCursor(80, 70);
    d.print("  RECORDING");

    // Elapsed seconds bar
    int secs = elapsed_ms / 1000;
    d.setTextColor(TFT_WHITE);
    d.setTextSize(3);
    d.setCursor(130, 110);
    d.printf("%ds", secs);

    // Instruction
    d.setTextColor(COL_HINT);
    d.setTextSize(1);
    d.setCursor(75, 175);
    d.print("Release BtnB to send");
}

// ─── Thinking overlay ─────────────────────────────────────────────────────────

inline void showThinkingUI() {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_CYAN);
    d.setTextSize(2);
    d.setCursor(70, 105);
    d.print("Thinking...");
}

// ─── Response display ─────────────────────────────────────────────────────────
// Shows transcript (user) and AI response. Stays on screen for display_ms.

inline void showResponseUI(const String& transcript, const String& response,
                           const String& emotion, uint32_t display_ms = 4000) {
    auto& d = M5.Display;
    const int W = d.width();   // 320
    const int H = d.height();  // 240

    d.fillScreen(COL_BG);

    // Emotion pill at top
    d.fillRoundRect(W/2 - 40, 6, 80, 18, 6, TFT_DARKGREY);
    d.setTextColor(TFT_YELLOW);
    d.setTextSize(1);
    int ex = W/2 - (emotion.length() * 6) / 2;
    d.setCursor(ex, 11);
    d.print(emotion);

    // User bubble (right-aligned)
    if (transcript.length()) {
        String t = transcript.length() > 38 ? transcript.substring(0, 38) + "..." : transcript;
        int bw = min((int)t.length() * 6 + 10, W - 20);
        int bx = W - bw - 8;
        d.fillRoundRect(bx, 30, bw, 20, 5, COL_USER_BG);
        d.setTextColor(0xAD75);   // light blue
        d.setTextSize(1);
        d.setCursor(bx + 5, 37);
        d.print(t);
    }

    // AI response text (word-wrapped)
    d.setTextColor(COL_AI_TEXT);
    d.setTextSize(1);
    const int LINE_H  = 14;
    const int MARGIN  = 8;
    const int MAX_COL = (W - MARGIN * 2) / 6;  // ~51 chars per line
    int x = MARGIN, y = 60;

    String words = response;
    while (words.length() > 0 && y < H - LINE_H) {
        int sp = words.indexOf(' ');
        if (sp < 0) sp = words.length();
        String word = words.substring(0, sp);
        words = (sp < (int)words.length()) ? words.substring(sp + 1) : "";

        if (x + (int)word.length() * 6 > W - MARGIN) {
            x = MARGIN; y += LINE_H;
        }
        d.setCursor(x, y);
        d.print(word + " ");
        x += (word.length() + 1) * 6;
    }

    // Hint at bottom
    d.setTextColor(COL_HINT);
    d.setTextSize(1);
    d.setCursor(55, H - 14);
    d.print("BtnB to talk  BtnC to forget");

    delay(display_ms);
}

// ─── Reader display (TTS quota fallback) ─────────────────────────────────────
// Shows the full AI response as readable text. Display time scales with length.

inline void showReaderUI(const String& text) {
    auto& d = M5.Display;
    const int W = d.width();   // 320
    const int H = d.height();  // 240

    d.fillScreen(0x1082);  // dark background

    // Small "no audio" badge top-right
    d.fillRoundRect(W - 62, 4, 58, 14, 4, 0x4208);
    d.setTextColor(0x8410);
    d.setTextSize(1);
    d.setCursor(W - 58, 8);
    d.print("No audio");

    // Render word-wrapped text, size 1 (6×8 px per char, ~51 cols)
    d.setTextColor(TFT_WHITE);
    d.setTextSize(1);
    const int LINE_H = 12;
    const int MARGIN = 8;
    const int MAX_COLS = (W - MARGIN * 2) / 6;
    int x = MARGIN, y = 24;

    String remaining = text;
    int word_count = 0;
    while (remaining.length() > 0 && y < H - LINE_H) {
        int sp = remaining.indexOf(' ');
        if (sp < 0) sp = remaining.length();
        String word = remaining.substring(0, sp);
        remaining = (sp < (int)remaining.length()) ? remaining.substring(sp + 1) : "";
        word_count++;

        if (x + (int)word.length() * 6 > W - MARGIN) {
            x = MARGIN; y += LINE_H;
            if (y >= H - LINE_H) break;
        }
        d.setCursor(x, y);
        d.print(word + " ");
        x += (word.length() + 1) * 6;
    }

    // 300 ms per word, min 2 s, max 12 s
    uint32_t display_ms = constrain((uint32_t)word_count * 300u, 2000u, 12000u);
    delay(display_ms);
}

// ─── Error display ────────────────────────────────────────────────────────────

inline void showErrorUI(const String& msg) {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_RED);
    d.setTextSize(1);
    d.setCursor(10, 108);
    d.print(msg.substring(0, 50));
    delay(2000);
}

// ─── Subtitle bar (overlaid on avatar face) ───────────────────────────────────
// Drawn at the bottom 52 px. Avatar redraws over it every ~66-100ms,
// so callers must redraw on a tight loop while audio plays.

static const int SUB_H = 52;

inline void drawSubtitleBar(const String& text) {
    auto& d  = M5.Display;
    const int W  = d.width();
    const int H  = d.height();
    const int BY = H - SUB_H;

    d.fillRect(0, BY, W, SUB_H, 0x0841);   // dark blue-tinted bar

    if (!text.length()) return;

    // Size-2 font: each char ~12 px wide, 16 px tall. Max ~26 chars/line.
    const int MAX_LINE = 26;
    d.setTextSize(2);

    String line1 = text, line2 = "";
    if ((int)text.length() > MAX_LINE) {
        int br = text.lastIndexOf(' ', MAX_LINE);
        if (br < 1) br = MAX_LINE;
        line1 = text.substring(0, br);
        line2 = text.substring(br + (text[br] == ' ' ? 1 : 0));
        if ((int)line2.length() > MAX_LINE) line2 = line2.substring(0, MAX_LINE - 1) + "~";
    }

    d.setTextColor(TFT_WHITE, 0x0841);

    if (!line2.length()) {
        int x = max(2, (W - (int)line1.length() * 12) / 2);
        d.setCursor(x, BY + (SUB_H - 16) / 2);
        d.print(line1);
    } else {
        int x1 = max(2, (W - (int)line1.length() * 12) / 2);
        int x2 = max(2, (W - (int)line2.length() * 12) / 2);
        d.setCursor(x1, BY + 4);
        d.print(line1);
        d.setCursor(x2, BY + 4 + 20);
        d.print(line2);
    }
}

inline void clearSubtitleBar() {
    M5.Display.fillRect(0, M5.Display.height() - SUB_H,
                        M5.Display.width(), SUB_H, TFT_BLACK);
}

// ─── Boot progress ────────────────────────────────────────────────────────────
// Call during long init operations (K144 setup, tokenizer wait).
// step: current step string, e.g. "LLM loading... (attempt 2/3)"
// progress_0_to_100: -1 = indeterminate (animated dots), 0-100 = bar fill

static uint8_t s_boot_dot_phase = 0;

inline void showBootProgress(const char* step, int progress_0_to_100 = -1) {
    auto& d = M5.Display;
    const int W = d.width();

    // Only redraw the lower portion — preserve the splash text above
    d.fillRect(0, 140, W, 100, TFT_BLACK);

    // Step label
    d.setTextColor(TFT_CYAN);
    d.setTextSize(1);
    int tx = max(2, (W - (int)strlen(step) * 6) / 2);
    d.setCursor(tx, 148);
    d.print(step);

    if (progress_0_to_100 >= 0) {
        // Determinate progress bar
        int bw = W - 40;
        d.drawRoundRect(20, 165, bw, 12, 4, TFT_DARKGREY);
        int fill = (bw - 4) * progress_0_to_100 / 100;
        if (fill > 0) d.fillRoundRect(22, 167, fill, 8, 3, TFT_CYAN);
    } else {
        // Indeterminate — three animated dots
        s_boot_dot_phase = (s_boot_dot_phase + 1) % 4;
        char dots[5] = "    ";
        for (int i = 0; i < (int)s_boot_dot_phase; i++) dots[i] = '.';
        d.setTextColor(TFT_DARKGREY);
        d.setCursor(W/2 - 12, 165);
        d.print(dots);
    }
}

// ─── WiFi status ─────────────────────────────────────────────────────────────

inline void showWiFiConnecting() {
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(80, 160);
    M5.Display.print("Connecting WiFi...");
}

inline void showWiFiStatus(bool connected) {
    M5.Display.fillRect(0, 155, 320, 12, TFT_BLACK);
    M5.Display.setTextSize(1);
    if (connected) {
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.setCursor(100, 158);
        M5.Display.print("WiFi OK");
    } else {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(85, 158);
        M5.Display.print("WiFi FAILED");
    }
}
