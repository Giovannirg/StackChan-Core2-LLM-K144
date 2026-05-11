#pragma once

#include <M5Unified.h>
#include "ai_config.h"

// ─── Speaker re-init after any mic session ────────────────────────────────────
// Call immediately after M5.Mic.end().  Shared by recordWhilePressed(),
// recordTimedSeconds(), and wake_word.h so the logic lives in one place.
inline void speakerReinitAfterMic() {
    // IDF 4.4 releases I2S platform asynchronously — wait before re-registering.
    delay(80);
    M5.Speaker.end();
    {
        auto spk_cfg = M5.Speaker.config();
        spk_cfg.sample_rate      = 48000;
        spk_cfg.dma_buf_len      = 1024;
        spk_cfg.dma_buf_count    = 16;
        spk_cfg.task_pinned_core = 0;  // Core 0 — keep DMA off Core 1 (loop + avatar)
        spk_cfg.task_priority    = 2;
        M5.Speaker.config(spk_cfg);
    }
    bool ok = M5.Speaker.begin();
    Serial.printf("[SPK] re-init after mic → %s\n", ok ? "OK" : "FAIL");
    // 300 ms for I2S DMA to stabilise before first playback (from AI_StackChan_Ex).
    delay(300);
    M5.Speaker.setVolume(g_volume);
}

// ─── Recording result ─────────────────────────────────────────────────────────

struct AudioRecording {
    int16_t* samples;  // ps_malloc'd — call freeRecording() when done
    size_t   count;    // number of samples captured
    bool     valid;    // false if too short or allocation failed
};

// ─── Record while BtnB is held ───────────────────────────────────────────────
// Blocks until button is released or max duration reached.
// Allocates from PSRAM.

inline AudioRecording recordWhilePressed() {
    AudioRecording rec = {nullptr, 0, false};

    rec.samples = (int16_t*)ps_malloc(RECORD_MAX_SAMPLES * sizeof(int16_t));
    if (!rec.samples) {
        Serial.println("[MIC] PSRAM alloc failed");
        return rec;
    }

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate        = RECORD_SAMPLE_RATE;
    mic_cfg.noise_filter_level = 2;   // light noise gate (0=off, 1-9)
    // Pin mic DMA task to Core 0 alongside the speaker task, away from
    // the Arduino loop() and avatar task on Core 1.
    mic_cfg.task_pinned_core   = 0;
    mic_cfg.task_priority      = 2;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
    delay(100);   // let DMA ring buffer fill before first read

    // 1024 samples @ 16 kHz = 64 ms per chunk.
    // We use blocking=false (safe — no cross-task semaphore issues) and
    // pace ourselves to one read per chunk period so DMA always has fresh data.
    static const int CHUNK    = 1024;
    static const uint32_t CHUNK_MS = (CHUNK * 1000UL) / RECORD_SAMPLE_RATE;  // 64 ms
    int16_t chunk[CHUNK];

    uint32_t chunk_due  = millis();   // earliest time for the next read

    while (rec.count < (size_t)RECORD_MAX_SAMPLES) {
        M5.update();
        if (!M5.BtnB.isPressed()) break;  // button released

        // Pace reads: wait until at least one chunk period has elapsed since
        // the previous read, so the DMA ring buffer is freshly filled.
        int32_t wait = (int32_t)(chunk_due - millis());
        if (wait > 0) delay(wait);
        chunk_due = millis() + CHUNK_MS;

        size_t space  = RECORD_MAX_SAMPLES - rec.count;
        size_t to_rec = min((size_t)CHUNK, space);
        M5.Mic.record(chunk, to_rec, RECORD_SAMPLE_RATE, false);  // non-blocking — safe
        memcpy(rec.samples + rec.count, chunk, to_rec * sizeof(int16_t));
        rec.count += to_rec;
    }

    M5.Mic.end();
    speakerReinitAfterMic();

    // Require at least 0.5 seconds of audio
    rec.valid = (rec.count >= RECORD_SAMPLE_RATE / 2);
    return rec;
}

// ─── Build WAV in PSRAM ───────────────────────────────────────────────────────
// Returns ps_malloc'd buffer; caller must free(). Sets *out_size.

inline uint8_t* buildWav(const AudioRecording& rec, size_t* out_size) {
    const uint32_t data_size = rec.count * sizeof(int16_t);
    const uint32_t wav_size  = 44 + data_size;

    uint8_t* wav = (uint8_t*)ps_malloc(wav_size);
    if (!wav) { Serial.println("[WAV] PSRAM alloc failed"); return nullptr; }

    // RIFF/WAVE/fmt/data headers (little-endian)
    memcpy(wav,      "RIFF", 4);
    *(uint32_t*)(wav +  4) = wav_size - 8;
    memcpy(wav +  8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    *(uint32_t*)(wav + 16) = 16;
    *(uint16_t*)(wav + 20) = 1;                         // PCM
    *(uint16_t*)(wav + 22) = 1;                         // mono
    *(uint32_t*)(wav + 24) = RECORD_SAMPLE_RATE;
    *(uint32_t*)(wav + 28) = RECORD_SAMPLE_RATE * 2;   // byte rate
    *(uint16_t*)(wav + 32) = 2;                         // block align
    *(uint16_t*)(wav + 34) = 16;                        // bits/sample
    memcpy(wav + 36, "data", 4);
    *(uint32_t*)(wav + 40) = data_size;
    memcpy(wav + 44, rec.samples, data_size);

    *out_size = wav_size;
    return wav;
}

// ─── Record for a fixed duration ─────────────────────────────────────────────
// Used by wake word after keyword is confirmed: records up to `secs` seconds.
// Allocates from PSRAM.  Caller must call freeRecording() when done.

inline AudioRecording recordTimedSeconds(float secs) {
    AudioRecording rec = {nullptr, 0, false};

    size_t max_samples = (size_t)(RECORD_SAMPLE_RATE * secs);
    if (max_samples > (size_t)RECORD_MAX_SAMPLES) max_samples = RECORD_MAX_SAMPLES;

    rec.samples = (int16_t*)ps_malloc(max_samples * sizeof(int16_t));
    if (!rec.samples) {
        Serial.println("[MIC] PSRAM alloc failed");
        return rec;
    }

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate        = RECORD_SAMPLE_RATE;
    mic_cfg.noise_filter_level = 2;
    mic_cfg.task_pinned_core   = 0;
    mic_cfg.task_priority      = 2;
    M5.Mic.config(mic_cfg);
    M5.Mic.begin();
    delay(100);

    static const int      CHUNK    = 1024;
    static const uint32_t CHUNK_MS = (CHUNK * 1000UL) / RECORD_SAMPLE_RATE;
    int16_t chunk[CHUNK];
    uint32_t chunk_due = millis();

    while (rec.count < max_samples) {
        int32_t wait = (int32_t)(chunk_due - millis());
        if (wait > 0) delay(wait);
        chunk_due = millis() + CHUNK_MS;

        size_t space  = max_samples - rec.count;
        size_t to_rec = min((size_t)CHUNK, space);
        M5.Mic.record(chunk, to_rec, RECORD_SAMPLE_RATE, false);
        memcpy(rec.samples + rec.count, chunk, to_rec * sizeof(int16_t));
        rec.count += to_rec;
    }

    M5.Mic.end();
    speakerReinitAfterMic();

    rec.valid = (rec.count >= RECORD_SAMPLE_RATE / 2);
    return rec;
}

inline void freeRecording(AudioRecording& rec) {
    if (rec.samples) { free(rec.samples); rec.samples = nullptr; }
    rec.count = 0;
    rec.valid = false;
}
