#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// OpenAI Realtime API — bidirectional audio over WebSocket
//
// Provider value: g_ai_provider == "realtime"
//
// ARCHITECTURE
// Traditional pipeline:  record → Whisper STT (2-4 s) → GPT (1-3 s) → TTS (2-5 s)  = 8-12 s
// Realtime API:          record → WSS audio → response audio streams back          < 2 s
//
// PROTOCOL (WSS wss://api.openai.com/v1/realtime?model=...)
//   1. Connect with Bearer auth + OpenAI-Beta: realtime=v1 header
//   2. Send session.update  — configure voice, instructions, audio format
//   3. Send input_audio_buffer.append (base64 PCM16 24kHz, chunked)
//   4. Send input_audio_buffer.commit
//   5. Send response.create
//   6. Receive response.audio.delta    → accumulate base64 PCM16 24kHz chunks
//      Receive response.audio_transcript.delta → accumulate text transcript
//      Receive response.done           → finish
//   7. Disconnect, play accumulated PCM with lip-sync and subtitles
//
// AUDIO FORMAT
//   Input:  PCM16, 24000 Hz, mono  (recording must be upsampled from 16 kHz)
//   Output: PCM16, 24000 Hz, mono  (played directly via M5.Speaker.playRaw)
//
// MEMORY
//   Resampled input (16→24 kHz, up to 8 s): ~384 KB PSRAM  (freed after send)
//   Response audio buffer (up to 15 s):     ~720 KB PSRAM  (freed after play)
//   Both allocations are sequential (input freed before response plays).
//
// REQUIRES
//   lib_deps: links2004/WebSockets@^2.3.6
//   build_flags: -DWEBSOCKETS_MAX_DATA_SIZE=65536
// ═══════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "ai_config.h"
#include "config_store.h"

// ─── Module-level state (used only within a single realtimeChat() call) ───────

static WebSocketsClient s_rt_ws;
static volatile bool    s_rt_connected  = false;
static volatile bool    s_rt_done       = false;
static volatile bool    s_rt_session_ok = false;   // session.created received

// Response audio accumulator (PSRAM, allocated on demand)
static int16_t* s_rt_audio      = nullptr;
static size_t   s_rt_audio_cap  = 0;
static size_t   s_rt_audio_len  = 0;   // bytes written so far

// Response transcript accumulator
static String   s_rt_transcript = "";
static String   s_rt_emotion    = "NEUTRAL";

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Append decoded base64 PCM chunk to the PSRAM audio accumulator.
static void rtAppendAudio(const char* b64, size_t b64_len) {
    if (!b64 || b64_len == 0) return;

    // Decode into a small stack temp to find output size, then copy to PSRAM buf.
    size_t out_len = 0;
    mbedtls_base64_decode(nullptr, 0, &out_len, (const uint8_t*)b64, b64_len);
    if (out_len == 0) return;

    // Grow PSRAM buffer on demand (double when full).
    if (!s_rt_audio || s_rt_audio_len + out_len > s_rt_audio_cap) {
        size_t new_cap = max(s_rt_audio_cap * 2, s_rt_audio_len + out_len + 65536);
        int16_t* grown = (int16_t*)ps_malloc(new_cap);
        if (!grown) { Serial.println("[Realtime] PSRAM alloc failed for audio"); return; }
        if (s_rt_audio) {
            memcpy(grown, s_rt_audio, s_rt_audio_len);
            free(s_rt_audio);
        }
        s_rt_audio     = grown;
        s_rt_audio_cap = new_cap;
    }

    size_t actual = 0;
    mbedtls_base64_decode((uint8_t*)s_rt_audio + s_rt_audio_len,
                           s_rt_audio_cap - s_rt_audio_len,
                           &actual,
                           (const uint8_t*)b64, b64_len);
    s_rt_audio_len += actual;
}

// Encode PCM16 bytes as base64 into a heap buffer. Caller must free().
static char* rtB64Encode(const uint8_t* data, size_t len) {
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    char* out = (char*)ps_malloc(out_len + 1);
    if (!out) return nullptr;
    mbedtls_base64_encode((uint8_t*)out, out_len, &out_len, data, len);
    out[out_len] = '\0';
    return out;
}

// Upsample PCM16 from 16 kHz → 24 kHz via linear interpolation (3:2 ratio).
// Returns PSRAM-allocated buffer. Caller must free(). out_samples set.
static int16_t* rtResample16to24(const int16_t* in, size_t in_samples,
                                  size_t* out_samples) {
    // For every 2 input samples → 3 output samples.
    // If in_samples is odd, the last sample is treated as a pair with itself.
    size_t pairs     = in_samples / 2;
    size_t remainder = in_samples % 2;
    *out_samples     = pairs * 3 + (remainder ? 2 : 0);

    int16_t* out = (int16_t*)ps_malloc((*out_samples) * sizeof(int16_t));
    if (!out) { *out_samples = 0; return nullptr; }

    size_t oi = 0;
    for (size_t i = 0; i < pairs; i++) {
        int32_t s0 = in[i * 2];
        int32_t s1 = in[i * 2 + 1];
        out[oi++] = (int16_t)s0;
        out[oi++] = (int16_t)((s0 * 2 + s1) / 3);
        out[oi++] = (int16_t)((s0     + s1 * 2) / 3);
    }
    if (remainder) {
        int32_t s0 = in[in_samples - 1];
        out[oi++] = (int16_t)s0;
        out[oi++] = (int16_t)s0;
    }
    return out;
}

// ─── WebSocket event handler ─────────────────────────────────────────────────

static void rtOnEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

    case WStype_CONNECTED:
        s_rt_connected = true;
        Serial.println("[Realtime] WS connected");
        break;

    case WStype_DISCONNECTED:
        s_rt_connected = false;
        if (!s_rt_done) {
            Serial.println("[Realtime] WS disconnected unexpectedly");
            s_rt_done = true;   // unblock the polling loop
        }
        break;

    case WStype_TEXT: {
        // Parse event type quickly without deserialising the whole document.
        // Most messages are audio deltas — avoid full JSON parse on every one.
        const char* p = (const char*)payload;

        // Quick scan for "type":"..." field
        const char* type_tag = strstr(p, "\"type\":\"");
        if (!type_tag) break;
        type_tag += 8;  // past "type":"

        if (strncmp(type_tag, "session.created", 15) == 0 ||
            strncmp(type_tag, "session.updated", 15) == 0) {
            s_rt_session_ok = true;
            Serial.println("[Realtime] Session ready");

        } else if (strncmp(type_tag, "response.audio.delta", 20) == 0) {
            // Extract the base64 "delta" value and accumulate PCM.
            const char* d = strstr(p, "\"delta\":\"");
            if (d) {
                d += 9;
                const char* end = strchr(d, '"');
                if (end) rtAppendAudio(d, (size_t)(end - d));
            }

        } else if (strncmp(type_tag, "response.audio_transcript.delta", 31) == 0) {
            const char* d = strstr(p, "\"delta\":\"");
            if (d) {
                d += 9;
                const char* end = strchr(d, '"');
                if (end) s_rt_transcript += String(d).substring(0, (int)(end - d));
            }

        } else if (strncmp(type_tag, "response.done", 13) == 0) {
            Serial.printf("[Realtime] Done — audio %u bytes, transcript: %s\n",
                          s_rt_audio_len, s_rt_transcript.c_str());
            // Try to extract emotion tag from transcript using the same format
            // the system prompt requests: [EMOTION] text
            String tr = s_rt_transcript;
            tr.trim();
            if (tr.startsWith("[")) {
                int close = tr.indexOf(']');
                if (close > 0) {
                    s_rt_emotion = tr.substring(1, close);
                    s_rt_emotion.trim();
                    s_rt_emotion.toUpperCase();
                }
            }
            s_rt_done = true;

        } else if (strncmp(type_tag, "error", 5) == 0) {
            Serial.printf("[Realtime] Server error: %.*s\n", (int)min(length, (size_t)200), p);
            s_rt_done = true;
        }
        break;
    }

    default: break;
    }
}

// ─── Send helpers ─────────────────────────────────────────────────────────────

// Send a JSON string as a WebSocket text frame.
// Takes by value — sendTXT() requires a non-const String& reference.
static inline void rtSend(String json) {
    s_rt_ws.sendTXT(json);
}

// Send audio in chunks of CHUNK_SAMPLES PCM samples (base64-encoded per chunk).
// This avoids building one giant base64 string in memory.
static void rtSendAudio(const int16_t* pcm, size_t samples) {
    const size_t CHUNK_SAMPLES = 3072;   // 6144 bytes → ~8192 bytes base64 per frame
    size_t sent = 0;
    while (sent < samples) {
        size_t n      = min(CHUNK_SAMPLES, samples - sent);
        char*  b64    = rtB64Encode((const uint8_t*)(pcm + sent), n * sizeof(int16_t));
        if (!b64) break;
        String frame  = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
        frame        += b64;
        frame        += "\"}";
        free(b64);
        s_rt_ws.sendTXT(frame);
        sent += n;
        s_rt_ws.loop();   // keep connection alive while sending large audio
        yield();
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

#include "tts_client.h"   // playWithSubtitles, samSpeak

// AIChatResult is defined in ai_client.h, which includes this file after defining the struct.

// Full Realtime API conversation turn: WAV recording → WSS → response audio + text.
// Returns an AIChatResult with the transcript and emotion.
// TTS is handled internally (response audio played directly here).
inline AIChatResult realtimeChat(const uint8_t* wav, size_t wav_len) {
    extern AIChatResult parseAIResponse(const String&);  // from ai_client.h

    AIChatResult err;
    err.success = false;
    err.emotion = "NEUTRAL";

    if (g_openai_api_key.isEmpty() || g_openai_api_key == "YOUR_OPENAI_API_KEY") {
        err.response = "OpenAI API key not set";
        return err;
    }

    // ── Reset per-call state ──────────────────────────────────────────────────
    s_rt_connected  = false;
    s_rt_done       = false;
    s_rt_session_ok = false;
    s_rt_transcript = "";
    s_rt_emotion    = "NEUTRAL";
    s_rt_audio      = nullptr;
    s_rt_audio_cap  = 0;
    s_rt_audio_len  = 0;

    // ── Resample recording 16 kHz → 24 kHz ───────────────────────────────────
    // WAV header is 44 bytes; skip it to get raw PCM samples.
    const int16_t* raw_pcm   = (const int16_t*)(wav + 44);
    size_t         raw_samp  = (wav_len - 44) / sizeof(int16_t);
    size_t         up_samp   = 0;
    int16_t*       up_pcm    = rtResample16to24(raw_pcm, raw_samp, &up_samp);
    if (!up_pcm) {
        err.response = "Resample OOM";
        return err;
    }
    Serial.printf("[Realtime] Resampled %u → %u samples (16→24 kHz)\n", raw_samp, up_samp);

    // ── Connect WebSocket ─────────────────────────────────────────────────────
    s_rt_ws.onEvent(rtOnEvent);
    String path = "/v1/realtime?model=" + g_realtime_model;
    String hdrs = "Authorization: Bearer " + g_openai_api_key + "\r\n"
                  "OpenAI-Beta: realtime=v1";
    s_rt_ws.beginSSL("api.openai.com", 443, path.c_str());
    s_rt_ws.setExtraHeaders(hdrs.c_str());
    s_rt_ws.setReconnectInterval(0);   // no auto-reconnect — we drive this manually

    // Wait for connection (up to 8 s)
    uint32_t t0 = millis();
    while (!s_rt_connected && millis() - t0 < 8000) { s_rt_ws.loop(); delay(5); }
    if (!s_rt_connected) {
        free(up_pcm);
        s_rt_ws.disconnect();
        err.response = "WebSocket connect failed";
        return err;
    }

    // Wait for session.created (up to 4 s)
    t0 = millis();
    while (!s_rt_session_ok && millis() - t0 < 4000) { s_rt_ws.loop(); delay(5); }

    // ── Configure session ─────────────────────────────────────────────────────
    // Voice: alloy | echo | fable | onyx | nova | shimmer (default: alloy)
    // We reuse g_openai_tts_voice so the web-UI voice picker applies here too.
    String voice = g_openai_tts_voice.isEmpty() ? "alloy" : g_openai_tts_voice;

    // Escape system prompt for JSON
    char esc[1024];
    extern size_t jsonEscape(const char*, char*, size_t);
    extern const char* buildSystemPrompt();
    jsonEscape(buildSystemPrompt(), esc, sizeof(esc));

    String session_update =
        String("{\"type\":\"session.update\",\"session\":{"
               "\"modalities\":[\"text\",\"audio\"],"
               "\"instructions\":\"") + esc + "\","
               "\"voice\":\"" + voice + "\","
               "\"input_audio_format\":\"pcm16\","
               "\"output_audio_format\":\"pcm16\","
               "\"turn_detection\":null"
               "}}";
    rtSend(session_update);
    s_rt_ws.loop(); delay(200);

    // ── Send recording ────────────────────────────────────────────────────────
    Serial.printf("[Realtime] Sending %u samples of audio...\n", up_samp);
    rtSendAudio(up_pcm, up_samp);
    free(up_pcm);

    rtSend("{\"type\":\"input_audio_buffer.commit\"}");
    s_rt_ws.loop(); delay(50);
    rtSend("{\"type\":\"response.create\"}");

    // ── Wait for response.done (up to 30 s) ──────────────────────────────────
    t0 = millis();
    while (!s_rt_done && millis() - t0 < 30000) {
        s_rt_ws.loop();
        delay(5);
    }
    s_rt_ws.disconnect();

    if (!s_rt_done) {
        if (s_rt_audio) { free(s_rt_audio); s_rt_audio = nullptr; }
        err.response = "Realtime timeout";
        return err;
    }

    // ── Play response audio with subtitles ────────────────────────────────────
    size_t samples = s_rt_audio_len / sizeof(int16_t);
    Serial.printf("[Realtime] Playing %u samples (%.1f s)\n",
                  samples, samples / 24000.0f);

    if (samples > 0) {
        playWithSubtitles(s_rt_transcript.c_str(),
                          s_rt_audio, samples, 24000);
    } else {
        samSpeak(s_rt_transcript.isEmpty()
                 ? "I heard you but had no audio response."
                 : s_rt_transcript.c_str());
    }

    if (s_rt_audio) { free(s_rt_audio); s_rt_audio = nullptr; }

    // ── Build result ──────────────────────────────────────────────────────────
    AIChatResult r;
    r.success    = true;
    r.emotion    = s_rt_emotion;
    r.transcript = "";   // Realtime API handles STT internally
    r.response   = s_rt_transcript;
    return r;
}
