#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Runtime config — loaded from /config.json on LittleFS at boot.
//
// Flash the filesystem once:   pio run -t uploadfs
// Flash firmware normally:     pio run -t upload
// Edit via web UI:             http://<device-ip>  (or http://192.168.4.1 in AP mode)
//
// Sensitive fields (API keys, WiFi password) are stored encrypted with AES-128-CTR.
// The encryption key is derived from the device's WiFi MAC address — device-bound.
// This prevents casual plaintext inspection of the filesystem image.
//
// Plain fields (SSID, voice, greeting) remain readable for easy manual editing.
// ═══════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "ai_config.h"

// ─── Runtime config globals ───────────────────────────────────────────────────

static String g_wifi_ssid         = WIFI_SSID;
static String g_wifi_pass         = WIFI_PASSWORD;
// Default provider from compile-time setting; overridden by config.json at runtime.
#if   AI_PROVIDER == AI_PROVIDER_GEMINI
static String g_ai_provider = "gemini";
#elif AI_PROVIDER == AI_PROVIDER_OPENAI
static String g_ai_provider = "openai";
#elif AI_PROVIDER == AI_PROVIDER_CLAUDE
static String g_ai_provider = "claude";
#elif AI_PROVIDER == AI_PROVIDER_LOCAL
static String g_ai_provider = "local";
#else
static String g_ai_provider = "gemini";
#endif
static String g_gemini_api_key    = GEMINI_API_KEY;
static String g_gemini_tts_voice  = GEMINI_TTS_VOICE;
static String g_openai_api_key    = OPENAI_API_KEY;
static String g_anthropic_api_key = ANTHROPIC_API_KEY;
static String g_greeting          = AI_GREETING;

// Runtime-editable model names (web UI / config.json override compile-time defaults)
#if   AI_PROVIDER == AI_PROVIDER_OPENAI
static String g_chat_model        = OPENAI_LLM_MODEL;
#elif AI_PROVIDER == AI_PROVIDER_CLAUDE
static String g_chat_model        = CLAUDE_MODEL;
#else
static String g_chat_model        = GEMINI_MODEL;
#endif
static String g_gemini_tts_model  = GEMINI_TTS_MODEL;
static uint8_t g_brightness       = 200;   // display brightness 0–255
static uint8_t g_volume           = 80;    // Core2 speaker volume 0–255
static uint8_t g_k144_volume      = 0;     // K144 module speaker 0–100 (→ 0.0–1.0). Default 0: mutes
                                            // the K144 speaker amp to prevent brownout when KWS fires.
static String g_face_style        = "default";  // "default" | "cat" | "capybara"
static String g_openai_tts_voice  = OPENAI_TTS_VOICE;  // alloy|echo|fable|onyx|nova|shimmer
static String g_kws_word          = "HELLO";           // K144 wake word (model-dependent)
static String g_local_llm_model   = LOCAL_LLM_MODEL;   // K144 LLM model — runtime selectable via web UI
static String g_elevenlabs_api_key  = ELEVENLABS_API_KEY;   // ElevenLabs TTS API key (encrypted)
static String g_elevenlabs_voice_id = ELEVENLABS_VOICE_ID;  // ElevenLabs voice ID (plain)
static String g_tts_provider        = "auto";               // "auto" | "elevenlabs"
static String g_realtime_model      = OPENAI_REALTIME_MODEL; // OpenAI Realtime model
static String g_local_asr_model     = "sherpa-ncnn";        // K144 ASR: "sherpa-ncnn"|"whisper-tiny"|"whisper-small"
static String g_local_tts_model   = "melotts-en-default"; // K144 TTS: "melotts-en-default"|"melotts-en-us"|"melotts-es-es"|"single-speaker-english-fast"
static uint16_t g_local_max_tokens = 80;               // K144 LLM max output tokens (60–300); 80 ≈ 1-2 sentences
static int8_t  g_servo_pan_min  =  45;   // servo pan  lower limit (degrees)
static int8_t  g_servo_pan_max  = 135;   // servo pan  upper limit (degrees)
static int8_t  g_servo_tilt_min =  70;   // servo tilt lower limit (degrees)
static int8_t  g_servo_tilt_max = 110;   // servo tilt upper limit (degrees)

// ─── Encryption helpers ───────────────────────────────────────────────────────
// AES-128-CTR, device-bound key derived from WiFi station MAC via HMAC-SHA256.
// Stored format:  "<ENC>" + base64(16-byte nonce || ciphertext)
// Plain fields:   stored as-is (no prefix)

static const char ENC_PREFIX[] = "<ENC>";

static void cfgDeriveKey(uint8_t key[16]) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // HMAC-SHA256(key="stackchan-cfg-v1", msg=mac) → take first 16 bytes as AES key
    static const uint8_t SALT[] = "stackchan-cfg-v1";
    uint8_t digest[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    SALT, sizeof(SALT) - 1,
                    mac,  sizeof(mac),
                    digest);
    memcpy(key, digest, 16);
}

// Encode bytes → base64 String
static String cfgB64Enc(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    char* buf = (char*)malloc(olen + 1);
    if (!buf) return "";
    mbedtls_base64_encode((uint8_t*)buf, olen + 1, &olen, data, len);
    buf[olen] = '\0';
    String s(buf);
    free(buf);
    return s;
}

// Decode base64 String → bytes. Returns byte count, 0 on error.
static size_t cfgB64Dec(const String& s, uint8_t* out, size_t max_out) {
    size_t olen = 0;
    if (mbedtls_base64_decode(out, max_out, &olen,
                              (const uint8_t*)s.c_str(), s.length()) != 0) return 0;
    return olen;
}

// Encrypt a plain-text String → "<ENC>base64(nonce||ciphertext)"
static String cfgEncrypt(const String& plain) {
    if (plain.isEmpty()) return plain;

    uint8_t key[16];
    cfgDeriveKey(key);

    // 16-byte random nonce from hardware RNG
    uint8_t nonce[16];
    uint32_t* n32 = (uint32_t*)nonce;
    for (int i = 0; i < 4; i++) n32[i] = esp_random();

    size_t   len    = plain.length();
    uint8_t* cipher = (uint8_t*)malloc(len);
    if (!cipher) return plain;  // OOM — store plain as fallback

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);  // CTR always uses _enc
    size_t nc_off = 0;
    uint8_t stream_block[16] = {};
    uint8_t nonce_ctr[16];
    memcpy(nonce_ctr, nonce, 16);
    mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_ctr, stream_block,
                          (const uint8_t*)plain.c_str(), cipher);
    mbedtls_aes_free(&aes);

    // Pack nonce||cipher and base64-encode
    uint8_t* combined = (uint8_t*)malloc(16 + len);
    if (!combined) { free(cipher); return plain; }
    memcpy(combined,      nonce,  16);
    memcpy(combined + 16, cipher, len);
    free(cipher);

    String result = String(ENC_PREFIX) + cfgB64Enc(combined, 16 + len);
    free(combined);
    return result;
}

// Decrypt "<ENC>base64..." → plain text. Returns "" on error.
static String cfgDecrypt(const String& stored) {
    if (!stored.startsWith(ENC_PREFIX)) return stored;  // not encrypted

    String b64      = stored.substring(sizeof(ENC_PREFIX) - 1);
    size_t b64_max  = b64.length();
    uint8_t* combined = (uint8_t*)malloc(b64_max);
    if (!combined) return "";

    size_t combined_len = cfgB64Dec(b64, combined, b64_max);
    if (combined_len < 17) { free(combined); return ""; }  // need nonce + ≥1 byte

    uint8_t key[16];
    cfgDeriveKey(key);

    uint8_t nonce_ctr[16];
    memcpy(nonce_ctr, combined, 16);

    size_t   cipher_len = combined_len - 16;
    uint8_t* plain      = (uint8_t*)malloc(cipher_len + 1);
    if (!plain) { free(combined); return ""; }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {};
    mbedtls_aes_crypt_ctr(&aes, cipher_len, &nc_off, nonce_ctr, stream_block,
                          combined + 16, plain);
    mbedtls_aes_free(&aes);
    free(combined);

    plain[cipher_len] = '\0';
    String result((char*)plain);
    free(plain);
    return result;
}

// ─── Save ─────────────────────────────────────────────────────────────────────
// Writes current globals to /config.json. Sensitive fields are AES-128-CTR
// encrypted; plain fields are stored as-is for easy manual inspection.

inline void saveConfig() {
    JsonDocument doc;
    // Plain fields (not secret)
    doc["wifi_ssid"]        = g_wifi_ssid;
    doc["ai_provider"]      = g_ai_provider;
    doc["gemini_tts_voice"] = g_gemini_tts_voice;
    doc["greeting"]         = g_greeting;
    doc["chat_model"]       = g_chat_model;
    doc["gemini_tts_model"] = g_gemini_tts_model;
    doc["brightness"]       = g_brightness;
    doc["volume"]           = g_volume;
    doc["k144_volume"]      = g_k144_volume;
    doc["face_style"]       = g_face_style;
    doc["openai_tts_voice"] = g_openai_tts_voice;
    doc["kws_word"]         = g_kws_word;
    doc["local_llm_model"]  = g_local_llm_model;
    doc["elevenlabs_voice_id"] = g_elevenlabs_voice_id;
    doc["tts_provider"]        = g_tts_provider;
    doc["realtime_model"]      = g_realtime_model;
    doc["local_asr_model"]     = g_local_asr_model;
    doc["local_tts_model"]     = g_local_tts_model;
    // Encrypted ElevenLabs key
    doc["elevenlabs_api_key"]  = cfgEncrypt(g_elevenlabs_api_key);
    doc["local_max_tokens"] = g_local_max_tokens;
    doc["servo_pan_min"]  = g_servo_pan_min;
    doc["servo_pan_max"]  = g_servo_pan_max;
    doc["servo_tilt_min"] = g_servo_tilt_min;
    doc["servo_tilt_max"] = g_servo_tilt_max;
    // Encrypted fields (sensitive)
    doc["wifi_password"]    = cfgEncrypt(g_wifi_pass);
    doc["gemini_api_key"]   = cfgEncrypt(g_gemini_api_key);
    doc["openai_api_key"]   = cfgEncrypt(g_openai_api_key);
    doc["anthropic_api_key"]= cfgEncrypt(g_anthropic_api_key);

    if (!LittleFS.begin(false)) LittleFS.begin(true);  // format on first use
    File f = LittleFS.open("/config.json", "w");
    if (f) {
        serializeJsonPretty(doc, f);
        f.close();
        Serial.println("[Config] Saved to /config.json (sensitive fields encrypted)");
    } else {
        Serial.println("[Config] ERROR: could not write /config.json");
    }
    LittleFS.end();
}

// ─── Load ─────────────────────────────────────────────────────────────────────

inline void loadConfig() {
    if (!LittleFS.begin(false)) {
        Serial.println("[Config] LittleFS mount failed — trying format...");
        if (!LittleFS.begin(true)) {
            Serial.println("[Config] LittleFS unavailable — using compiled defaults");
            return;
        }
    }

    if (!LittleFS.exists("/config.json")) {
        Serial.println("[Config] /config.json not found — using compiled defaults");
        LittleFS.end();
        return;
    }

    File f = LittleFS.open("/config.json", "r");
    if (!f) {
        Serial.println("[Config] Cannot open /config.json");
        LittleFS.end();
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    LittleFS.end();

    if (err) {
        Serial.printf("[Config] JSON parse error: %s — using compiled defaults\n", err.c_str());
        return;
    }

    // Helper: overwrite global only when key is present and non-empty
    auto str = [&](const char* key, String& dest) {
        const char* v = doc[key];
        if (v && *v) dest = v;
    };
    // Helper: overwrite global, decrypting if needed
    auto sec = [&](const char* key, String& dest) {
        const char* v = doc[key];
        if (v && *v) {
            String decrypted = cfgDecrypt(String(v));
            if (decrypted.length()) dest = decrypted;
        }
    };

    str("wifi_ssid",         g_wifi_ssid);
    sec("wifi_password",     g_wifi_pass);
    str("ai_provider",       g_ai_provider);
    sec("gemini_api_key",    g_gemini_api_key);
    str("gemini_tts_voice",  g_gemini_tts_voice);
    sec("openai_api_key",    g_openai_api_key);
    sec("anthropic_api_key", g_anthropic_api_key);
    str("greeting",          g_greeting);
    str("chat_model",        g_chat_model);
    str("gemini_tts_model",  g_gemini_tts_model);
    if (doc["brightness"].is<int>()) g_brightness = (uint8_t)constrain((int)doc["brightness"], 0, 255);
    if (doc["volume"].is<int>())      g_volume      = (uint8_t)constrain((int)doc["volume"],      0, 255);
    if (doc["k144_volume"].is<int>()) g_k144_volume = (uint8_t)constrain((int)doc["k144_volume"], 0, 100);
    str("face_style",        g_face_style);
    str("openai_tts_voice",  g_openai_tts_voice);
    str("kws_word",          g_kws_word);
    str("local_llm_model",   g_local_llm_model);
    sec("elevenlabs_api_key",  g_elevenlabs_api_key);
    str("elevenlabs_voice_id", g_elevenlabs_voice_id);
    str("tts_provider",        g_tts_provider);
    str("realtime_model",      g_realtime_model);
    str("local_asr_model",     g_local_asr_model);
    str("local_tts_model",     g_local_tts_model);
    if (doc["local_max_tokens"].is<int>())
        g_local_max_tokens = (uint16_t)constrain((int)doc["local_max_tokens"], 60, 300);
    if (doc["servo_pan_min"].is<int>())  g_servo_pan_min  = (int8_t)constrain((int)doc["servo_pan_min"],   0, 180);
    if (doc["servo_pan_max"].is<int>())  g_servo_pan_max  = (int8_t)constrain((int)doc["servo_pan_max"],   0, 180);
    if (doc["servo_tilt_min"].is<int>()) g_servo_tilt_min = (int8_t)constrain((int)doc["servo_tilt_min"],  0, 180);
    if (doc["servo_tilt_max"].is<int>()) g_servo_tilt_max = (int8_t)constrain((int)doc["servo_tilt_max"],  0, 180);

    Serial.printf("[Config] Loaded — WiFi: %s, provider: %s\n",
                  g_wifi_ssid.c_str(),
                  g_ai_provider.length() ? g_ai_provider.c_str() : "(compiled)");
}
