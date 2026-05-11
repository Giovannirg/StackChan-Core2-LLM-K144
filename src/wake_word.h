#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Wake-word detection — "Oye chispita"
//
// Flow (called every loop iteration when not busy):
//   1. Cooldown gate  — skip if called < WAKE_COOLDOWN_MS ago
//   2. VAD probe      — record 512 samples, compute RMS; skip if silent
//   3. Full recording — capture WAKE_RECORD_SECS seconds
//   4. STT            — Gemini transcription-only or Whisper
//   5. Keyword check  — case-insensitive search for WAKE_KEYWORD in transcript
//
// Returns true when the wake phrase is detected.  Caller (main.cpp) then plays
// an ack beep, records the command with recordTimedSeconds(), and runs the full
// AI pipeline.
//
// Must be #include'd AFTER ai_client.h (needs b64Encode, httpsPost,
// whisperTranscribe, g_gemini_api_key).
// ═══════════════════════════════════════════════════════════════════════════════

#include <M5Unified.h>
#include "audio_recorder.h"
#include "ai_config.h"
#include "config_store.h"

// ─── Tuning ───────────────────────────────────────────────────────────────────

#define WAKE_VAD_THRESHOLD  600     // RMS below this → silence, skip STT
#define WAKE_VAD_SAMPLES    512     // ~32 ms @ 16 kHz for the energy probe
#define WAKE_RECORD_SECS    3.0f    // duration of the phrase capture window
#define WAKE_KEYWORD        "chisp" // matches "chispita", "chispa", etc.
#define WAKE_COOLDOWN_MS    1500    // minimum ms between consecutive checks

// ─── STT: Gemini transcription-only ──────────────────────────────────────────
// Sends audio directly to Gemini with a minimal "transcribe only" prompt.
// No conversation history, no system prompt — faster and cheaper than aiChat().

inline String geminiTranscribe(const uint8_t* wav, size_t wav_len) {
    char* b64 = b64Encode(wav, wav_len);
    if (!b64) return "";
    size_t b64_len = strlen(b64);

    // JSON frame is ~180 chars; add b64 payload + headroom
    size_t total = b64_len + 512;
    char*  buf   = (char*)ps_malloc(total);
    if (!buf) { free(b64); return ""; }

    size_t pos = 0;
    pos += snprintf(buf + pos, total - pos,
        "{\"contents\":[{\"role\":\"user\",\"parts\":["
        "{\"text\":\"Transcribe this audio exactly as spoken. "
        "Output ONLY the transcription, nothing else.\"},"
        "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
    memcpy(buf + pos, b64, b64_len); pos += b64_len;
    free(b64);
    pos += snprintf(buf + pos, total - pos, "\"}}]}]}");

    char url[256];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
        g_chat_model.c_str(), g_gemini_api_key.c_str());

    String resp = httpsPost(url, "application/json", (uint8_t*)buf, pos);
    free(buf);

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return "";
    const char* text = doc["candidates"][0]["content"]["parts"][0]["text"];
    return text ? String(text) : String("");
}

// ─── Unified transcribe — runtime dispatch ────────────────────────────────────

inline String wakeTranscribe(const uint8_t* wav, size_t wav_len) {
    // OpenAI / Claude use Whisper for wake-word STT.
    // All other providers (Gemini, Local) use Gemini transcription-only.
    // Note: Local provider wake-word still uses Gemini STT for the Core2-mic
    // VAD+record path; the K144 Whisper ASR is used only for the BtnB flow.
    String t;
    if (g_ai_provider == "openai" || g_ai_provider == "claude")
        t = whisperTranscribe(wav, wav_len);
    else
        t = geminiTranscribe(wav, wav_len);
    t.toLowerCase();
    return t;
}

// ─── VAD probe ────────────────────────────────────────────────────────────────
// Starts the mic, grabs WAKE_VAD_SAMPLES, computes RMS, then re-inits speaker.

inline uint32_t vadProbe() {
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate      = RECORD_SAMPLE_RATE;
    mic_cfg.task_pinned_core = 0;
    mic_cfg.task_priority    = 2;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
    delay(60);  // let DMA ring buffer fill

    int16_t buf[WAKE_VAD_SAMPLES];
    M5.Mic.record(buf, WAKE_VAD_SAMPLES, RECORD_SAMPLE_RATE, false);
    // Wait one full chunk period so DMA delivers fresh samples
    delay((WAKE_VAD_SAMPLES * 1000UL) / RECORD_SAMPLE_RATE + 10);

    M5.Mic.end();
    speakerReinitAfterMic();

    int64_t sum = 0;
    for (int i = 0; i < WAKE_VAD_SAMPLES; i++)
        sum += (int64_t)buf[i] * buf[i];
    return (uint32_t)sqrt((double)sum / WAKE_VAD_SAMPLES);
}

// ─── Public API ───────────────────────────────────────────────────────────────
// Returns true when the wake phrase "oye chispita" (or any utterance containing
// WAKE_KEYWORD) is detected.  Non-blocking except during the ~4 s VAD+record
// window that only opens when the voice energy gate passes.

inline bool wakeWordCheck() {
    static uint32_t last_check_ms = 0;
    uint32_t now = millis();
    if (now - last_check_ms < WAKE_COOLDOWN_MS) return false;
    last_check_ms = now;

    // ── Step 1: energy gate ───────────────────────────────────────────────────
    uint32_t rms = vadProbe();
    Serial.printf("[WAKE] VAD rms=%u\n", rms);
    if (rms < WAKE_VAD_THRESHOLD) return false;

    // ── Step 2: record full phrase window ─────────────────────────────────────
    AudioRecording rec = recordTimedSeconds(WAKE_RECORD_SECS);
    if (!rec.valid) { freeRecording(rec); return false; }

    // ── Step 3: transcribe ────────────────────────────────────────────────────
    size_t   wav_size;
    uint8_t* wav = buildWav(rec, &wav_size);
    freeRecording(rec);
    if (!wav) return false;

    String transcript = wakeTranscribe(wav, wav_size);
    free(wav);

    Serial.printf("[WAKE] transcript: \"%s\"\n", transcript.c_str());

    // ── Step 4: keyword match ─────────────────────────────────────────────────
    return transcript.indexOf(WAKE_KEYWORD) >= 0;
}
