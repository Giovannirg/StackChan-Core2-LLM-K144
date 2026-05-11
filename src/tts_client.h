#pragma once

// Text-to-speech with movie-style subtitle overlay.
//
// While audio plays:
//  - Words appear one-by-one at the bottom of the screen over the avatar face.
//  - The subtitle bar is redrawn every 20 ms so it stays visible despite the
//    avatar RTOS task periodically overwriting the screen.
//
// Gemini provider → gemini-2.5-flash-preview-tts  (base64 PCM, stream-decoded)
// OpenAI/Claude   → OpenAI TTS API "pcm" format   (raw binary, streamed)
//
// Both produce int16_t PCM @ 24 kHz played via M5.Speaker.playRaw().

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Avatar.h>
#include <ESP8266SAM.h>        // offline TTS fallback
#include "ai_config.h"
#include "config_store.h"
#include "conversation_ui.h"   // drawSubtitleBar / clearSubtitleBar

#define TTS_SAMPLE_RATE  24000

// ─── Lip-sync ─────────────────────────────────────────────────────────────────
// Call setLipSyncAvatar(&avatar) in setup() to enable lip-sync and avatar-based
// subtitles during TTS.  When set, playWithSubtitles() drives mouth open ratio
// from PCM amplitude instead of writing directly to the display.
static m5avatar::Avatar* s_lipsync_av = nullptr;
inline void setLipSyncAvatar(m5avatar::Avatar* av) { s_lipsync_av = av; }

// ─── Avatar lifecycle callbacks ───────────────────────────────────────────────
// Set in setup() so samSpeak() can stop the avatar before direct display writes
// (showReaderUI) and restart it after.  Without this, the avatar task overwrites
// the reader/error screen within one frame (~100 ms).
static void (*s_av_stop_fn)()  = nullptr;
static void (*s_av_start_fn)() = nullptr;
inline void setAvatarLifecycle(void (*stop_fn)(), void (*start_fn)()) {
    s_av_stop_fn  = stop_fn;
    s_av_start_fn = start_fn;
}

// Compute peak amplitude over a ~50 ms window centred at `pos` and map to [0,1].
inline float lipSyncRatio(const int16_t* pcm, size_t samples, size_t pos,
                           uint32_t sample_rate) {
    const size_t HALF = sample_rate / 40;  // 25 ms each side → 50 ms window
    size_t lo = (pos > HALF) ? pos - HALF : 0;
    size_t hi = min(pos + HALF, samples);
    int32_t peak = 0;
    for (size_t i = lo; i < hi; i++) {
        int32_t s = abs((int32_t)pcm[i]);
        if (s > peak) peak = s;
    }
    // Speech peaks are rarely near INT16_MAX; boost 3× then clamp.
    float ratio = (float)peak / 32767.0f * 3.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return ratio;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Streaming base64 decoder (4-char chunks → 3 bytes, no extra buffer)
// ═══════════════════════════════════════════════════════════════════════════════

static const int8_t B64_DEC[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
};

inline int b64DecodeChunk(const uint8_t in[4], uint8_t out[3]) {
    int8_t a = B64_DEC[in[0] & 0x7F], b = B64_DEC[in[1] & 0x7F];
    int8_t c = B64_DEC[in[2] & 0x7F], d = B64_DEC[in[3] & 0x7F];
    if (a < 0 || b < 0) return 0;
    out[0] = (uint8_t)((a << 2) | (b >> 4));
    if (in[2] == '=') return 1;
    out[1] = (uint8_t)((b << 4) | (c >> 2));
    if (in[3] == '=') return 2;
    out[2] = (uint8_t)((c << 6) | d);
    return 3;
}

// Scan WiFiClient stream until marker string appears. Returns true if found.
inline bool streamScanFor(WiFiClient* s, const char* marker,
                           uint32_t timeout_ms = 20000) {
    int mlen = strlen(marker), mpos = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (s->available()) {
            char c = s->read();
            mpos = (c == marker[mpos]) ? mpos + 1 : (c == marker[0] ? 1 : 0);
            if (mpos == mlen) return true;
        } else delay(1);
    }
    return false;
}

// Scan stream for two markers simultaneously.
// Returns 1 if m1 found first, 2 if m2 found first, 0 on timeout.
inline int streamScanEither(WiFiClient* s,
                             const char* m1, const char* m2,
                             uint32_t timeout_ms) {
    int l1 = strlen(m1), l2 = strlen(m2);
    int p1 = 0, p2 = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (!s->available()) { delay(1); continue; }
        char c = s->read();
        p1 = (c == m1[p1]) ? p1 + 1 : (c == m1[0] ? 1 : 0);
        p2 = (c == m2[p2]) ? p2 + 1 : (c == m2[0] ? 1 : 0);
        if (p1 == l1) return 1;
        if (p2 == l2) return 2;
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Subtitle + progressive word-reveal playback
// ═══════════════════════════════════════════════════════════════════════════════

// Split `text` into words, store in `words[]`, return count.
inline int splitWords(const char* text, String words[], int max_words) {
    int count = 0;
    const char* p = text;
    while (*p && count < max_words) {
        while (*p == ' ') p++;   // skip spaces
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') p++;
        words[count++] = String(text).substring(start - text, p - text);
    }
    return count;
}

// ─── Subtitle paging ──────────────────────────────────────────────────────────
// Pre-divide words into pages that each fit within the subtitle bar
// (2 lines × 26 chars = 52 chars max per page, matching drawSubtitleBar).
// Words are revealed one-by-one within each page; when the page is exhausted
// the bar clears and the next page starts — no text ever drops from the front.

static const int SUB_PAGE_MAX_CHARS = 52;  // 2 × SUB_MAX_LINE (26) from conversation_ui.h

struct SubPage {
    int start_word;
    int end_word;
};

inline int buildSubtitlePages(const String* words, int word_count,
                               SubPage* pages, int max_pages) {
    int page_count = 0;
    int i = 0;
    while (i < word_count && page_count < max_pages) {
        SubPage& p  = pages[page_count++];
        p.start_word = i;
        int used     = 0;
        while (i < word_count) {
            int need = (used ? 1 : 0) + (int)words[i].length();  // space + word
            if (used + need > SUB_PAGE_MAX_CHARS) break;
            used += need;
            i++;
        }
        // Guard: single word longer than limit — take it alone (truncation in drawSubtitleBar)
        if (i == p.start_word) i++;
        p.end_word = i;
    }
    return page_count;
}

// Play PCM and show subtitles (words appear gradually in sync with speech).
// sample_rate defaults to TTS_SAMPLE_RATE; pass a different value when the
// actual rate is known from the audio container (e.g. WAV header).
//
// Lip-sync mode (when setLipSyncAvatar() has been called):
//   - Avatar stays running; subtitles go through avatar.setSpeechText().
//   - Mouth open ratio is driven by PCM amplitude every ~50 ms.
// Direct-draw mode (fallback, avatar must be stopped by caller):
//   - Subtitles drawn directly to display via drawSubtitleBar().
inline void playWithSubtitles(const char* text, int16_t* pcm, size_t samples,
                               uint32_t sample_rate = TTS_SAMPLE_RATE) {
    const int MAX_WORDS = 64;
    const int MAX_PAGES = 20;
    String    words[MAX_WORDS];
    SubPage   pages[MAX_PAGES];
    int       word_count = splitWords(text, words, MAX_WORDS);
    int       page_count = buildSubtitlePages(words, word_count, pages, MAX_PAGES);

    uint32_t dur_ms      = (uint32_t)((uint64_t)samples * 1000UL / sample_rate);
    uint32_t ms_per_word = (word_count > 0) ? (dur_ms / word_count) : 400;
    // Clamp: no word faster than 150ms, no word slower than 800ms
    ms_per_word = constrain(ms_per_word, 150u, 800u);

    M5.Speaker.setVolume(g_volume);
    bool ok = M5.Speaker.playRaw(pcm, samples, sample_rate,
                                  false, 1, 0, false);
    Serial.printf("[TTS] playRaw %u samples → %s\n", samples, ok ? "OK" : "FAIL");
    if (!ok) return;

    // playRaw() is non-blocking — delay so DMA starts before we enter the loop.
    delay(100);

    uint32_t start       = millis();
    String   visible     = "";
    String   lastDrawn   = "";
    int      next_word   = 0;
    int      cur_page    = 0;
    uint32_t lastLipSync = 0;

    // Loop until:
    //   • every word has been revealed (next_word == word_count), AND
    //   • audio has finished + DMA drained (isPlaying() false AND dur_ms+800 elapsed)
    while (next_word < word_count ||
           M5.Speaker.isPlaying() ||
           (millis() - start) < dur_ms + 800) {
        uint32_t elapsed = millis() - start;

        // Reveal next word(s) on schedule
        bool changed = false;
        while (next_word < word_count &&
               elapsed >= (uint32_t)(next_word * ms_per_word)) {
            // Page flip: current page exhausted → clear bar and advance
            if (cur_page < page_count - 1 && next_word >= pages[cur_page].end_word) {
                cur_page++;
                visible = "";
            }
            if (visible.length()) visible += " ";
            visible += words[next_word];
            next_word++;
            changed = true;
        }

        if (s_lipsync_av) {
            // Lip-sync + avatar subtitles: avatar is running, use its API
            if (changed && visible != lastDrawn) {
                s_lipsync_av->setSpeechText(visible.c_str());
                lastDrawn = visible;
            }
            // Update mouth ratio every ~50 ms
            if (elapsed - lastLipSync >= 50) {
                lastLipSync = elapsed;
                size_t pos = (size_t)((uint64_t)elapsed * sample_rate / 1000);
                if (pos > samples) pos = samples;
                s_lipsync_av->setMouthOpenRatio(lipSyncRatio(pcm, samples, pos, sample_rate));
            }
        } else {
            // Direct-draw mode: avatar must be stopped by caller
            if (changed && visible != lastDrawn) {
                drawSubtitleBar(visible);
                lastDrawn = visible;
            }
        }

        delay(10);
    }

    // Close mouth and clear subtitles
    if (s_lipsync_av) {
        s_lipsync_av->setMouthOpenRatio(0.0f);
        // Hold last subtitle until DMA fully drains, then clear
        delay(2500);
        s_lipsync_av->setSpeechText("");
    } else {
        drawSubtitleBar(visible);
        delay(2500);
        clearSubtitleBar();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SAM (Software Automatic Mouth) — offline TTS fallback
//
// No internet required. Generates a robotic voice locally on the ESP32.
// Library: earlephilhower/ESP8266SAM (added to platformio.ini).
//
// SAM writes 16-bit stereo samples into SamBufOut; we take the mono left
// channel, play via M5Unified at SAM's own sample rate, and show the full
// text as a subtitle while it speaks.
// ═══════════════════════════════════════════════════════════════════════════════

// Custom AudioOutput that captures SAM's output into a PSRAM buffer
// instead of routing to hardware I2S (which M5Unified already owns).
class SamBufOut : public AudioOutput {
public:
    int16_t* buf;
    size_t   count = 0;
    size_t   cap;
    int      rate  = 11025;  // SAM calls SetRate(); captured here

    SamBufOut(int16_t* b, size_t c) : buf(b), cap(c) {}

    bool SetRate(int hz)  override { rate = hz; hertz = hz; return true; }
    bool ConsumeSample(int16_t sample[2]) override {
        if (count < cap) buf[count++] = sample[0];  // left channel = mono
        return true;
    }
};

// Sanitize text for speech: strip code blocks, LaTeX math, backslash sequences,
// problematic punctuation, non-ASCII. Truncate at sentence boundary ≤ limit chars.
// Returns heap-allocated string — caller must free(). Returns nullptr on OOM.
inline char* ttsSanitize(const char* text, size_t char_limit = 280) {
    size_t len  = strlen(text);
    char*  safe = (char*)malloc(len + 1);
    if (!safe) return nullptr;
    size_t j     = 0;
    bool in_code = false;
    bool in_math = false;
    for (size_t i = 0; i < len; ) {
        if (i + 2 < len && text[i]=='`' && text[i+1]=='`' && text[i+2]=='`') {
            in_code = !in_code; i += 3; continue;
        }
        if (in_code) { i++; continue; }
        if (text[i] == '\\' && i + 1 < len) {
            char nx = text[i+1];
            if (nx == '[' || nx == '(') { in_math = true;  i += 2; continue; }
            if (nx == ']' || nx == ')') { in_math = false; i += 2; continue; }
            i++; continue;
        }
        if (in_math) { i++; continue; }
        uint8_t c = (uint8_t)text[i++];
        if (c == '\n' || c == '\r' || c == '\t') {
            if (j == 0 || safe[j-1] != ' ') safe[j++] = ' ';
        } else if (c >= 32 && c <= 126) {
            if (c=='{' || c=='}' || c=='^' || c=='_' ||
                c=='[' || c==']' || c=='|' || c=='~' ||
                c=='#' || c=='$' || c=='`') continue;
            safe[j++] = (char)c;
        }
        // non-ASCII silently dropped
    }
    // collapse runs of spaces
    size_t k = 0;
    for (size_t i = 0; i < j; i++) {
        if (safe[i]==' ' && k>0 && safe[k-1]==' ') continue;
        safe[k++] = safe[i];
    }
    safe[k] = '\0';
    // Truncate at last sentence-end before char_limit
    if (k > char_limit) {
        size_t cut = char_limit;
        for (size_t i = char_limit; i > char_limit / 2; i--) {
            if (safe[i] == ' ' && i > 0 &&
                (safe[i-1] == '.' || safe[i-1] == '!' || safe[i-1] == '?')) {
                cut = i; break;
            }
        }
        safe[cut] = '\0';
    }
    return safe;
}

inline void samSpeak(const char* text) {
    if (!text || !*text) { showReaderUI(String(text ? text : "")); return; }

    // SAM outputs at 22050 Hz (not 11 kHz as originally assumed).
    // 220000 samples @ 22050 Hz ≈ 10 s — enough for any single response.
    const size_t MAX_SAMPLES = 220000;
    int16_t* buf = (int16_t*)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!buf) { showReaderUI(String(text)); return; }

    SamBufOut*   out = new SamBufOut(buf, MAX_SAMPLES);
    ESP8266SAM*  sam = new ESP8266SAM;
    sam->SetSpeed(64);
    sam->SetPitch(64);
    sam->SetThroat(110);
    sam->SetMouth(160);
    {
        char* safe = ttsSanitize(text, 280);
        if (safe) {
            sam->Say(out, safe);
            free(safe);
        } else {
            sam->Say(out, text);  // OOM — try anyway
        }
    }
    delete sam;

    size_t   samples = out->count;
    uint32_t rate    = (uint32_t)out->rate;
    delete out;

    // Guard: SAM sometimes omits SetRate(); clamp to a sane value
    if (rate < 8000 || rate > 48000) rate = 11025;
    Serial.printf("[TTS/SAM] %u samples @ %u Hz\n", samples, rate);

    if (samples > 0) {
        // 2× linear-interpolation upsample: SAM's ~11 kHz output causes harsh
        // aliasing through the speaker.  Upsampling to ~22 kHz before playback
        // removes most of that harshness without audible ringing.
        size_t   up_count = samples * 2;
        int16_t* up_buf   = (int16_t*)ps_malloc(up_count * sizeof(int16_t));
        if (up_buf) {
            for (size_t i = 0; i < samples - 1; i++) {
                up_buf[i * 2]     = buf[i];
                up_buf[i * 2 + 1] = (int16_t)(((int32_t)buf[i] + buf[i + 1]) / 2);
            }
            up_buf[(samples - 1) * 2]     = buf[samples - 1];
            up_buf[(samples - 1) * 2 + 1] = buf[samples - 1];
        }
        const int16_t* play_buf   = up_buf ? up_buf   : buf;
        size_t         play_count = up_buf ? up_count  : samples;
        uint32_t       play_rate  = up_buf ? rate * 2  : rate;

        // Reuse playWithSubtitles() for word-by-word reveal + lip-sync,
        // same as cloud TTS — SAM is now a full-quality fallback.
        playWithSubtitles(text, const_cast<int16_t*>(play_buf), play_count, play_rate);
        if (up_buf) free(up_buf);
    } else {
        Serial.println("[TTS/SAM] 0 samples — falling back to reader");
        // Avatar must be stopped before writing directly to the display;
        // otherwise the avatar RTOS task overwrites the screen within one frame.
        if (s_av_stop_fn) s_av_stop_fn();
        showReaderUI(String(text));
        if (s_av_start_fn) s_av_start_fn();
    }
    free(buf);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Gemini TTS  (runtime-selectable — also used by "local" provider)
// ═══════════════════════════════════════════════════════════════════════════════

inline void geminiSpeak(const char* text) {
    // Build request body
    // system_instruction tells the model to produce audio-only output, preventing
    // the HTTP 400 "Model tried to generate text, but it should only be used for TTS" error.
    String body = "{"
                  "\"system_instruction\":{\"parts\":[{\"text\":\"Read aloud the following text.\"}]},"
                  "\"contents\":[{\"parts\":[{\"text\":\"";
    for (const char* p = text; *p; p++) {
        if      (*p == '"')  body += "\\\"";
        else if (*p == '\\') body += "\\\\";
        else if (*p == '\n') body += ' ';
        else                 body += *p;
    }
    body += "\"}]}],"
            "\"generationConfig\":{"
            "\"responseModalities\":[\"AUDIO\"],"
            "\"speechConfig\":{\"voiceConfig\":{"
            "\"prebuiltVoiceConfig\":{\"voiceName\":\"";
    body += g_gemini_tts_voice;
    body += "\"}}}}}";

    char url[256];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
        g_gemini_tts_model.c_str(), g_gemini_api_key.c_str());

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(25000);
    http.addHeader("Content-Type", "application/json");

    Serial.printf("[TTS/Gemini] POST %u byte body\n", body.length());
    int code = http.POST(body);
    Serial.printf("[TTS/Gemini] HTTP %d\n", code);

    // Retry once on transient server errors (500/503) — usually resolves in < 2 s
    if (code == 500 || code == 503) {
        Serial.println("[TTS/Gemini] transient error — retrying in 2 s...");
        http.end();
        delay(2000);
        http.begin(client, url);
        http.setTimeout(25000);
        http.addHeader("Content-Type", "application/json");
        code = http.POST(body);
        Serial.printf("[TTS/Gemini] retry HTTP %d\n", code);
    }

    if (code != 200) {
        String err = http.getString();
        Serial.println("[TTS/Gemini] " + err.substring(0, 400));
        http.end();
        samSpeak(text);   // quota / rate-limit / persistent error → SAM fallback
        return;
    }

    WiFiClient* stream = http.getStreamPtr();

    // Scan for "data": (audio) or "text": (model returned plain text instead of audio).
    // Both markers are watched simultaneously so a non-TTS model is detected in < 1 s
    // rather than waiting for a 20 s timeout. The "data": key appears inside the
    // inlineData / inline_data object regardless of camelCase vs snake_case variant.
    {
        int hit = streamScanEither(stream, "\"data\":", "\"text\":", 15000);
        if (hit != 1) {
            // Drain remaining bytes for diagnostics
            char dbg[513] = {};
            int  n = 0;
            uint32_t t1 = millis();
            while (n < 512 && millis() - t1 < 1500) {
                if (stream->available()) dbg[n++] = (char)stream->read();
                else delay(1);
            }
            if (hit == 2)
                Serial.printf("[TTS/Gemini] ERROR: model returned text, not audio"
                              " — use a dedicated TTS model (e.g. gemini-2.5-flash-preview-tts): %s\n", dbg);
            else
                Serial.printf("[TTS/Gemini] ERROR: neither 'data' nor 'text' found in response: %s\n", dbg);
            http.end();
            samSpeak(text);
            return;
        }
    }

    // Skip whitespace / opening quote to reach the base64 value
    {
        uint32_t t0 = millis();
        bool found_quote = false;
        while (millis() - t0 < 3000) {
            if (stream->available()) {
                uint8_t c = stream->read();
                if (c == '"') { found_quote = true; break; }
            } else delay(1);
        }
        if (!found_quote) {
            Serial.println("[TTS/Gemini] ERROR: opening quote of base64 data not found");
            http.end();
            samSpeak(text);
            return;
        }
    }

    // Stream-decode base64 PCM into PSRAM (max ~10 s @ 24 kHz 16-bit)
    const size_t PCM_MAX = 480000;
    int16_t* pcm = (int16_t*)ps_malloc(PCM_MAX);
    if (!pcm) {
        Serial.println("[TTS/Gemini] PSRAM alloc failed");
        http.end();
        samSpeak(text);
        return;
    }

    uint8_t  chunk[4];
    int      cpos      = 0;
    size_t   pcm_bytes = 0;
    uint32_t t0        = millis();

    while (millis() - t0 < 15000 && pcm_bytes < PCM_MAX - 4) {
        if (stream->available()) {
            uint8_t c = stream->read();
            if (c == '"') break;
            if (c < 32)  continue;
            chunk[cpos++] = c;
            if (cpos == 4) {
                uint8_t out[3];
                int n = b64DecodeChunk(chunk, out);
                memcpy((uint8_t*)pcm + pcm_bytes, out, n);
                pcm_bytes += n;
                cpos = 0;
                t0   = millis();
            }
        } else {
            delay(1);
        }
    }
    http.end();

    // Log first 8 raw bytes so we can identify the actual container format:
    //   Raw PCM:  first samples near 0 (silence at start of speech)
    //   WAV:      starts with 52 49 46 46 ("RIFF")
    //   MP3:      starts with FF FB / FF E3 / ID3...
    if (pcm_bytes >= 16) {
        const uint8_t* raw = (const uint8_t*)pcm;
        Serial.printf("[TTS/Gemini] first 8 bytes: %02X %02X %02X %02X  %02X %02X %02X %02X\n",
                      raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
    }

    // If Gemini wrapped the PCM in a WAV container, skip the 44-byte header.
    size_t   pcm_offset = 0;
    uint32_t play_rate  = TTS_SAMPLE_RATE;
    {
        const uint8_t* raw = (const uint8_t*)pcm;
        if (pcm_bytes >= 44 &&
            raw[0]=='R' && raw[1]=='I' && raw[2]=='F' && raw[3]=='F' &&
            raw[8]=='W' && raw[9]=='A' && raw[10]=='V' && raw[11]=='E') {
            // Read the sample rate from the WAV fmt chunk (bytes 24-27, little-endian)
            play_rate  = raw[24] | ((uint32_t)raw[25]<<8) |
                         ((uint32_t)raw[26]<<16) | ((uint32_t)raw[27]<<24);
            pcm_offset = 44;
            Serial.printf("[TTS/Gemini] WAV container detected — skipping header, rate=%u\n", play_rate);
        }
    }

    size_t samples = (pcm_bytes - pcm_offset) / sizeof(int16_t);
    Serial.printf("[TTS/Gemini] decoded %u bytes → %u samples @ %u Hz (%.1f s)\n",
                  pcm_bytes, samples, play_rate, samples / (float)play_rate);

    if (samples > 0) {
        int16_t* play_pcm = (int16_t*)((uint8_t*)pcm + pcm_offset);
        playWithSubtitles(text, play_pcm, samples, play_rate);
    } else {
        Serial.println("[TTS/Gemini] ERROR: 0 samples decoded");
        free(pcm);
        samSpeak(text);
        return;
    }
    free(pcm);
}

// ═══════════════════════════════════════════════════════════════════════════════
// OpenAI TTS  (used for "openai" and "claude" providers)
// ═══════════════════════════════════════════════════════════════════════════════

inline void openaiSpeak(const char* text) {
    String body = "{\"model\":\"tts-1\","
                  "\"voice\":\"" + g_openai_tts_voice + "\","
                  "\"response_format\":\"pcm\","
                  "\"input\":\"";
    for (const char* p = text; *p; p++) {
        if      (*p == '"')  body += "\\\"";
        else if (*p == '\\') body += "\\\\";
        else if (*p == '\n') body += ' ';
        else                 body += *p;
    }
    body += "\"}";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.openai.com/v1/audio/speech");
    http.setTimeout(20000);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("Authorization", "Bearer " + g_openai_api_key);

    int code = http.POST(body);
    Serial.printf("[TTS/OpenAI] HTTP %d\n", code);

    if (code != 200) {
        Serial.println("[TTS/OpenAI] " + http.getString().substring(0, 120));
        http.end();
        samSpeak(text);   // quota / rate-limit → offline SAM fallback
        return;
    }

    int      content_len = http.getSize();
    size_t   buf_size    = (content_len > 0) ? (size_t)content_len : 350000;
    int16_t* pcm         = (int16_t*)ps_malloc(buf_size);
    if (!pcm) { Serial.println("[TTS/OpenAI] PSRAM alloc failed"); http.end(); samSpeak(text); return; }

    WiFiClient* stream   = http.getStreamPtr();
    size_t      bytes_rd = 0;
    uint32_t    t0       = millis();

    while (bytes_rd < buf_size && millis() - t0 < 12000) {
        size_t avail = stream->available();
        if (avail) {
            size_t n = stream->readBytes((uint8_t*)pcm + bytes_rd,
                                         min(avail, buf_size - bytes_rd));
            bytes_rd += n;
            t0        = millis();
        } else if (!http.connected()) break;
        else delay(2);
    }
    http.end();

    size_t samples = bytes_rd / sizeof(int16_t);
    Serial.printf("[TTS/OpenAI] %u bytes → %u samples (%.1f s)\n",
                  bytes_rd, samples, samples / (float)TTS_SAMPLE_RATE);

    if (samples > 0) {
        playWithSubtitles(text, pcm, samples);
    } else {
        Serial.println("[TTS/OpenAI] ERROR: 0 samples received");
        free(pcm);
        samSpeak(text);
        return;
    }
    free(pcm);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ElevenLabs TTS  (optional — set g_tts_provider = "elevenlabs" in web UI)
// Requests raw PCM16 at 24 kHz via the streaming endpoint; plays with lip-sync.
// Falls back to SAM on error. Supports all AI providers (Gemini/OpenAI/Claude).
// ═══════════════════════════════════════════════════════════════════════════════

inline void elevenLabsSpeak(const char* text) {
    String body = "{\"text\":\"";
    for (const char* p = text; *p; p++) {
        if      (*p == '"')  body += "\\\"";
        else if (*p == '\\') body += "\\\\";
        else if (*p == '\n') body += ' ';
        else                 body += *p;
    }
    body += "\",\"model_id\":\"eleven_multilingual_v2\","
            "\"voice_settings\":{\"stability\":0.5,\"similarity_boost\":0.75}}";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.elevenlabs.io/v1/text-to-speech/"
                 + g_elevenlabs_voice_id + "/stream?output_format=pcm_24000";
    http.begin(client, url);
    http.setTimeout(20000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("xi-api-key",   g_elevenlabs_api_key);

    int code = http.POST(body);
    Serial.printf("[TTS/ElevenLabs] HTTP %d\n", code);

    if (code != 200) {
        Serial.println("[TTS/ElevenLabs] " + http.getString().substring(0, 120));
        http.end();
        samSpeak(text);
        return;
    }

    int      content_len = http.getSize();
    size_t   buf_size    = (content_len > 0) ? (size_t)content_len : 480000;
    int16_t* pcm         = (int16_t*)ps_malloc(buf_size);
    if (!pcm) { Serial.println("[TTS/ElevenLabs] PSRAM alloc failed"); http.end(); samSpeak(text); return; }

    WiFiClient* stream   = http.getStreamPtr();
    size_t      bytes_rd = 0;
    uint32_t    t0       = millis();

    while (bytes_rd < buf_size && millis() - t0 < 15000) {
        size_t avail = stream->available();
        if (avail) {
            size_t n = stream->readBytes((uint8_t*)pcm + bytes_rd,
                                         min(avail, buf_size - bytes_rd));
            bytes_rd += n;
            t0 = millis();
        } else if (!http.connected()) break;
        else delay(2);
    }
    http.end();

    size_t samples = bytes_rd / sizeof(int16_t);
    Serial.printf("[TTS/ElevenLabs] %u bytes → %u samples (%.1f s @ 24kHz)\n",
                  bytes_rd, samples, samples / 24000.0f);

    if (samples > 0) {
        playWithSubtitles(text, pcm, samples, 24000);
    } else {
        Serial.println("[TTS/ElevenLabs] 0 samples — falling back to SAM");
        free(pcm);
        samSpeak(text);
        return;
    }
    free(pcm);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Unified TTS entry point — dispatches on g_ai_provider + g_tts_provider.
//   g_tts_provider = "elevenlabs" → ElevenLabs for all cloud providers
//   g_tts_provider = "auto":
//     openai / claude / realtime → OpenAI TTS
//     gemini / local             → Gemini TTS (SAM fallback built in)
// Falls back to SAM when WiFi is down regardless of provider.
// ═══════════════════════════════════════════════════════════════════════════════

inline void speakText(const char* text) {
    if (!text || !*text) return;
    Serial.printf("[TTS] tts=%s provider=%s\n", g_tts_provider.c_str(), g_ai_provider.c_str());
    bool online = WiFi.status() == WL_CONNECTED;
    if (!online) { samSpeak(text); return; }

    if (g_tts_provider == "elevenlabs") {
        elevenLabsSpeak(text);
        return;
    }
    // auto: use provider default
    if (g_ai_provider == "openai" || g_ai_provider == "claude" || g_ai_provider == "realtime") {
        openaiSpeak(text);
    } else {
        geminiSpeak(text);   // gemini + local — SAM fallback built into geminiSpeak
    }
}
