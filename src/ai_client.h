#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "ai_config.h"
#include "config_store.h"
#include "local_llm.h"      // K144 local LLM + Whisper ASR (always compiled)

// ═══════════════════════════════════════════════════════════════════════════════
// Result type
// ═══════════════════════════════════════════════════════════════════════════════

struct AIChatResult {
    String transcript;  // what the user said
    String response;    // AI response text (emotion tag already stripped)
    String emotion;     // HAPPY / SAD / ANGRY / SLEEPY / SURPRISED / NEUTRAL
    bool   success;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Conversation history
// ═══════════════════════════════════════════════════════════════════════════════

struct ConvTurn {
    char  role[12];   // "user" | "assistant" | "model"
    char* text;       // heap-allocated copy
};

static ConvTurn s_history[CONV_MAX_PAIRS * 2];
static int      s_hist_n = 0;

inline void convAdd(const char* role, const char* text) {
    if (s_hist_n >= CONV_MAX_PAIRS * 2) {
        free(s_history[0].text);
        free(s_history[1].text);
        for (int i = 2; i < s_hist_n; i++) s_history[i - 2] = s_history[i];
        s_hist_n -= 2;
    }
    strlcpy(s_history[s_hist_n].role, role, sizeof(s_history[0].role));
    s_history[s_hist_n].text = strdup(text);
    s_hist_n++;
}

inline void convClear() {
    for (int i = 0; i < s_hist_n; i++) free(s_history[i].text);
    s_hist_n = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Date / time injection
// NTP is synced in connectWiFi() (main.cpp). Once time() > epoch, we inject the
// current date and time as a one-line prefix to the system prompt so the LLM
// always knows when it is — no function-calling required.
// ═══════════════════════════════════════════════════════════════════════════════

inline const char* buildSystemPrompt() {
    static char s_full[2048];
    time_t now = time(nullptr);
    if (now > 1000000000UL) {   // valid NTP time
        struct tm* t = localtime(&now);
        char prefix[96];
        strftime(prefix, sizeof(prefix),
                 "Today is %A, %B %d %Y. Current time: %H:%M. ", t);
        snprintf(s_full, sizeof(s_full), "%s%s", prefix, AI_SYSTEM_PROMPT);
        return s_full;
    }
    return AI_SYSTEM_PROMPT;   // NTP not yet synced — no time prefix
}

// ═══════════════════════════════════════════════════════════════════════════════
// Shared helpers
// ═══════════════════════════════════════════════════════════════════════════════

inline size_t jsonEscape(const char* src, char* dst, size_t dst_max) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out < dst_max - 6; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  dst[out++] = '\\'; dst[out++] = '"';  break;
            case '\\': dst[out++] = '\\'; dst[out++] = '\\'; break;
            case '\n': dst[out++] = '\\'; dst[out++] = 'n';  break;
            case '\r': dst[out++] = '\\'; dst[out++] = 'r';  break;
            case '\t': dst[out++] = '\\'; dst[out++] = 't';  break;
            default:   if (c >= 0x20) dst[out++] = c;        break;
        }
    }
    dst[out] = '\0';
    return out;
}

inline char* b64Encode(const uint8_t* data, size_t len) {
    size_t out_len;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    char* out = (char*)ps_malloc(out_len + 1);
    if (!out) return nullptr;
    mbedtls_base64_encode((uint8_t*)out, out_len, &out_len, data, len);
    out[out_len] = '\0';
    return out;
}

// HTTPS POST. auth_bearer sets "Authorization: Bearer <token>".
// x_api_key sets "x-api-key: <key>" (used by Anthropic).
inline String httpsPost(const char* url, const char* content_type,
                        const uint8_t* body, size_t body_len,
                        const char* auth_bearer = nullptr,
                        const char* x_api_key   = nullptr) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(45000);
    http.addHeader("Content-Type", content_type);
    if (auth_bearer) http.addHeader("Authorization", auth_bearer);
    if (x_api_key) {
        http.addHeader("x-api-key",         x_api_key);
        http.addHeader("anthropic-version",  "2023-06-01");
    }

    int code = http.POST(const_cast<uint8_t*>(body), body_len);
    String resp = (code > 0) ? http.getString()
                              : String("{\"error\":\"HTTP_ERR_") + code + "\"}";
    http.end();
    return resp;
}

// Parse the two-line AI response format:
//   USER_SAID: <transcript>
//   [EMOTION] <response text>
inline AIChatResult parseAIResponse(const String& raw) {
    AIChatResult r;
    r.success = true;
    r.emotion = "NEUTRAL";

    String text = raw;
    text.trim();

    int us_pos = text.indexOf("USER_SAID:");
    if (us_pos >= 0) {
        int line_end = text.indexOf('\n', us_pos);
        if (line_end < 0) line_end = text.length();
        r.transcript = text.substring(us_pos + 10, line_end);
        r.transcript.trim();
        text = text.substring(line_end + 1);
        text.trim();
    }

    if (text.startsWith("[")) {
        int close = text.indexOf(']');
        if (close > 0) {
            r.emotion = text.substring(1, close);
            r.emotion.trim();        // handle "[ HAPPY ]" → "HAPPY"
            r.emotion.toUpperCase();
            text = text.substring(close + 1);
            text.trim();
        }
    }

    r.response = text;
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Whisper STT — used by OpenAI and Claude providers
// ═══════════════════════════════════════════════════════════════════════════════

inline String whisperTranscribe(const uint8_t* wav, size_t wav_len) {
    const char* B = "SCchan9b2c";
    char part_head[256], part_model[128];
    snprintf(part_head, sizeof(part_head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", B);
    snprintf(part_model, sizeof(part_model),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        OPENAI_STT_MODEL "\r\n--%s--\r\n", B, B);

    size_t   head_len = strlen(part_head), model_len = strlen(part_model);
    size_t   total    = head_len + wav_len + model_len;
    uint8_t* body     = (uint8_t*)ps_malloc(total);
    if (!body) return "";

    memcpy(body,                         part_head,  head_len);
    memcpy(body + head_len,              wav,        wav_len);
    memcpy(body + head_len + wav_len,    part_model, model_len);

    char   ct[80];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", B);
    String auth = "Bearer " + g_openai_api_key;
    String resp = httpsPost("https://api.openai.com/v1/audio/transcriptions",
                            ct, body, total, auth.c_str());
    free(body);

    JsonDocument doc;
    deserializeJson(doc, resp);
    return doc["text"] | String("");
}

// ══════���════════════════════════════════════════════════════════════════════════
// Gemini provider  (audio inline → Gemini multimodal LLM)
// ═══════════════════════════════════════════════════════════════════════════════

inline char* buildGeminiPayload(size_t* out_size,
                                const uint8_t* wav, size_t wav_len) {
    char* b64 = b64Encode(wav, wav_len);
    if (!b64) return nullptr;
    size_t b64_len = strlen(b64);

    const char* sys_prompt = buildSystemPrompt();
    size_t total = strlen(sys_prompt) * 2 + s_hist_n * 512 + b64_len + 1024;
    char*  buf   = (char*)ps_malloc(total);
    if (!buf) { free(b64); return nullptr; }

    char   esc[1024];
    size_t pos = 0;

    jsonEscape(sys_prompt, esc, sizeof(esc));
    pos += snprintf(buf + pos, total - pos,
        "{\"system_instruction\":{\"parts\":[{\"text\":\"%s\"}]},\"contents\":[", esc);

    for (int i = 0; i < s_hist_n; i++) {
        char* esc_t = (char*)ps_malloc(strlen(s_history[i].text) * 2 + 16);
        if (esc_t) {
            jsonEscape(s_history[i].text, esc_t, strlen(s_history[i].text) * 2 + 16);
            pos += snprintf(buf + pos, total - pos,
                "{\"role\":\"%s\",\"parts\":[{\"text\":\"%s\"}]},",
                s_history[i].role, esc_t);
            free(esc_t);
        }
    }

    pos += snprintf(buf + pos, total - pos,
        "{\"role\":\"user\",\"parts\":["
        "{\"text\":\"Respond as Stack-chan to what I just said.\"},"
        "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
    memcpy(buf + pos, b64, b64_len);
    pos += b64_len;
    free(b64);
    pos += snprintf(buf + pos, total - pos, "\"}}]}]}");

    *out_size = pos;
    return buf;
}

inline AIChatResult geminiChat(const uint8_t* wav, size_t wav_len) {
    AIChatResult err; err.success = false;

    size_t payload_size;
    char*  payload = buildGeminiPayload(&payload_size, wav, wav_len);
    if (!payload) { err.response = "Out of PSRAM"; return err; }

    char url[256];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
        g_chat_model.c_str(), g_gemini_api_key.c_str());

    String resp = httpsPost(url, "application/json", (uint8_t*)payload, payload_size);
    free(payload);

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { err.response = "JSON parse error"; return err; }
    if (doc["error"].is<JsonObject>()) { err.response = doc["error"]["message"].as<String>(); return err; }

    const char* raw = doc["candidates"][0]["content"]["parts"][0]["text"];
    if (!raw) { err.response = "Empty response"; return err; }

    AIChatResult r = parseAIResponse(String(raw));
    if (r.transcript.length()) convAdd("user",  r.transcript.c_str());
    convAdd("model", r.response.c_str());
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// OpenAI provider  (Whisper STT → GPT LLM)
// ═══════════════════════════════════════════════════════════════════════════════

inline String gptComplete(const String& user_text) {
    size_t est = 1024 + s_hist_n * 512 + user_text.length() * 2;
    char*  buf = (char*)ps_malloc(est);
    if (!buf) return "[ERROR] OOM";

    char   esc[1024];
    size_t pos = 0;

    jsonEscape(buildSystemPrompt(), esc, sizeof(esc));
    pos += snprintf(buf + pos, est - pos,
        "{\"model\":\"%s\","
        "\"messages\":[{\"role\":\"system\",\"content\":\"%s\"}",
        g_chat_model.c_str(), esc);

    for (int i = 0; i < s_hist_n; i++) {
        char* et = (char*)ps_malloc(strlen(s_history[i].text) * 2 + 16);
        if (et) {
            jsonEscape(s_history[i].text, et, strlen(s_history[i].text) * 2 + 16);
            pos += snprintf(buf + pos, est - pos,
                ",{\"role\":\"%s\",\"content\":\"%s\"}", s_history[i].role, et);
            free(et);
        }
    }
    char* eu = (char*)ps_malloc(user_text.length() * 2 + 16);
    if (eu) {
        jsonEscape(user_text.c_str(), eu, user_text.length() * 2 + 16);
        pos += snprintf(buf + pos, est - pos,
            ",{\"role\":\"user\",\"content\":\"%s\"}]}", eu);
        free(eu);
    }

    String auth = "Bearer " + g_openai_api_key;
    String resp = httpsPost("https://api.openai.com/v1/chat/completions",
                            "application/json", (uint8_t*)buf, pos, auth.c_str());
    free(buf);

    JsonDocument doc;
    deserializeJson(doc, resp);
    if (doc["error"].is<JsonObject>())
        return "[ERROR] " + doc["error"]["message"].as<String>();
    return doc["choices"][0]["message"]["content"] | String("[ERROR] No response");
}

inline AIChatResult openaiChat(const uint8_t* wav, size_t wav_len) {
    AIChatResult err; err.success = false;

    String transcript = whisperTranscribe(wav, wav_len);
    if (transcript.isEmpty()) { err.response = "Transcription failed"; return err; }
    Serial.println("[STT] " + transcript);

    String       raw = gptComplete(transcript);
    AIChatResult r   = parseAIResponse(raw);
    r.transcript     = transcript;

    convAdd("user",      transcript.c_str());
    convAdd("assistant", r.response.c_str());
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Claude provider  (Whisper STT → Claude LLM)
// ═══════════════════════════════════════════════════════════════════════════════

inline String claudeComplete(const String& user_text) {
    size_t est = 1024 + s_hist_n * 512 + user_text.length() * 2;
    char*  buf = (char*)ps_malloc(est);
    if (!buf) return "[ERROR] OOM";

    char   esc[1024];
    size_t pos = 0;

    jsonEscape(buildSystemPrompt(), esc, sizeof(esc));
    pos += snprintf(buf + pos, est - pos,
        "{\"model\":\"%s\","
        "\"max_tokens\":512,"
        "\"system\":\"%s\","
        "\"messages\":[",
        g_chat_model.c_str(), esc);

    bool first = true;
    for (int i = 0; i < s_hist_n; i++) {
        char* et = (char*)ps_malloc(strlen(s_history[i].text) * 2 + 16);
        if (et) {
            jsonEscape(s_history[i].text, et, strlen(s_history[i].text) * 2 + 16);
            // Claude uses "user" / "assistant" (not "model")
            const char* role = (strncmp(s_history[i].role, "user", 4) == 0)
                               ? "user" : "assistant";
            pos += snprintf(buf + pos, est - pos,
                "%s{\"role\":\"%s\",\"content\":\"%s\"}",
                first ? "" : ",", role, et);
            free(et);
            first = false;
        }
    }

    char* eu = (char*)ps_malloc(user_text.length() * 2 + 16);
    if (eu) {
        jsonEscape(user_text.c_str(), eu, user_text.length() * 2 + 16);
        pos += snprintf(buf + pos, est - pos,
            "%s{\"role\":\"user\",\"content\":\"%s\"}]}",
            first ? "" : ",", eu);
        free(eu);
    }

    // Anthropic uses x-api-key header instead of Bearer
    String resp = httpsPost("https://api.anthropic.com/v1/messages",
                            "application/json",
                            (uint8_t*)buf, pos,
                            nullptr,                          // no Bearer token
                            g_anthropic_api_key.c_str()); // x-api-key
    free(buf);

    JsonDocument doc;
    deserializeJson(doc, resp);
    if (doc["error"].is<JsonObject>())
        return "[ERROR] " + doc["error"]["message"].as<String>();
    return doc["content"][0]["text"] | String("[ERROR] No response");
}

inline AIChatResult claudeChat(const uint8_t* wav, size_t wav_len) {
    AIChatResult err; err.success = false;

    // STT via OpenAI Whisper (OPENAI_API_KEY required alongside ANTHROPIC_API_KEY)
    String transcript = whisperTranscribe(wav, wav_len);
    if (transcript.isEmpty()) { err.response = "Transcription failed"; return err; }
    Serial.println("[STT] " + transcript);

    String       raw = claudeComplete(transcript);
    AIChatResult r   = parseAIResponse(raw);
    r.transcript     = transcript;

    convAdd("user",      transcript.c_str());
    convAdd("assistant", r.response.c_str());
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Gemini STT-only  (transcription without LLM — used by LOCAL and wake-word)
// Sends WAV audio with a "transcribe only" prompt; returns plain text.
// Uses gemini-2.0-flash regardless of g_chat_model (guaranteed audio support).
// ═══════════════════════════════════════════════════════════════════════════════

inline String geminiSTTOnly(const uint8_t* wav, size_t wav_len) {
    char* b64 = b64Encode(wav, wav_len);
    if (!b64) return "";
    size_t b64_len = strlen(b64);

    size_t total = b64_len + 512;
    char*  buf   = (char*)ps_malloc(total);
    if (!buf) { free(b64); return ""; }

    size_t pos = 0;
    pos += snprintf(buf + pos, total - pos,
        "{\"contents\":[{\"role\":\"user\",\"parts\":["
        "{\"text\":\"Transcribe this audio exactly as spoken. "
        "Return only the words, no punctuation or extra text.\"},"
        "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
    memcpy(buf + pos, b64, b64_len); pos += b64_len;
    free(b64);
    pos += snprintf(buf + pos, total - pos, "\"}}]}]}");

    char url[256];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/"
        "gemini-2.0-flash:generateContent?key=%s",
        g_gemini_api_key.c_str());

    String resp = httpsPost(url, "application/json", (uint8_t*)buf, pos);
    free(buf);

    JsonDocument doc;
    if (deserializeJson(doc, resp) || doc["error"].is<JsonObject>()) return "";
    const char* text = doc["candidates"][0]["content"]["parts"][0]["text"];
    return text ? String(text) : String("");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Local LLM provider  (K144 Whisper ASR → K144 module LLM)
//
// NOTE: localChat() is NOT called from aiChat() for the LOCAL provider.
// The LOCAL provider BtnB path bypasses Core2 mic recording entirely and
// calls localWhisperCapture() + localLLMInfer() directly from handleAI()
// in main.cpp, because K144 Whisper uses the module's own mic (sys.pcm).
// This function is kept for completeness but wav/wav_len are ignored.
// ═══════════════════════════════════════════════════════════════════════════════

inline AIChatResult localChat(const uint8_t* /*wav*/, size_t /*wav_len*/) {
    AIChatResult err; err.success = false;
    // Transcript comes from K144 Whisper via localWhisperCapture() in main.cpp.
    // This code path should not be reached during normal operation.
    err.response = "Use BtnB for local mode (K144 mic)";
    return err;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Unified entry point — dispatches on g_ai_provider at runtime
// ═══════════════════════════════════════════════════════════════════════════════

#include "realtime_client.h" // OpenAI Realtime API — included here so AIChatResult is complete

inline AIChatResult aiChat(const uint8_t* wav, size_t wav_len) {
    if (g_ai_provider == "realtime") return realtimeChat(wav, wav_len);
    if (g_ai_provider == "openai")   return openaiChat(wav, wav_len);
    if (g_ai_provider == "claude")   return claudeChat(wav, wav_len);
    if (g_ai_provider == "local")    return localChat(wav, wav_len);
    return geminiChat(wav, wav_len);   // default / "gemini"
}
