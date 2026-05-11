#pragma once

// ─── M5Stack LLM Module (K144 / AX630C) ──────────────────────────────────────
//
// Communicates over Serial2 (115200 baud, JSON protocol) via Port C.
//
// PORT C CONFLICT: GPIO 13 (RX) / GPIO 14 (TX) is shared with the TTL SCS
// servo. When g_ai_provider == "local" at runtime, setup() forces PWM servo
// mode and logs the override.
//
// PIPELINE ARCHITECTURE:
//   K144: audio → KWS("HELLO") → ASR  (transcript → Core2 over Serial2)
//   Core2: receives transcript → localLLMInfer() → MeloTTS on K144 speaker (or SAM fallback)
//
//   IMPORTANT: K144 ASR ALWAYS requires KWS. Standalone ASR (without a KWS
//   work_id in the input list) never delivers messages — confirmed by all
//   official library examples. This is a K144 StackFlow firmware constraint.
//
//   LLM is set up as standalone (direct text input, not connected to ASR in
//   the pipeline). Core2 bridges: gets the ASR transcript from K144 over
//   Serial2, then pushes it to K144 LLM via inferenceAndWaitResult().
//   Connecting LLM to ASR in pipeline mode + MeloTTS setup crashes the ESP32
//   (K144 floods Serial2 with pipeline messages during setup).
//
// FIRMWARE UPGRADE (run on the module's Linux OS over SSH):
//   apt update && apt upgrade
//
// AVAILABLE LLM MODELS (install via apt on the module):
//   qwen2.5-0.5B-prefill-20e   (default, fast,  ~1 s/token)
//   qwen2.5-1.5B-ax630c        (smarter, ~2 s/token)
//   qwen3-0.6b-ax630c
//   llama3.2-1B-ax630c
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5ModuleLLM.h>
#include "ai_config.h"
#include "conversation_ui.h"  // showBootProgress() for K144 init progress display

static M5ModuleLLM s_mod_llm;

// ── Unit work_ids ─────────────────────────────────────────────────────────────
static String s_kws_work_id    = "";
static String s_asr_work_id    = "";
static String s_vad_work_id    = "";
static String s_llm_work_id    = "";
static String s_tts_work_id    = "";
static String s_melotts_work_id = "";
static bool   s_llm_ready       = false;
static bool   s_kws_ready       = false;
static bool   s_tts_ready       = false;     // true = legacy TTS set up; false = SAM fallback
static bool   s_melotts_ready   = false;     // true = MeloTTS set up (preferred over tts)
static bool   s_asr_is_whisper  = false;     // true = Whisper API (full utterance)
static bool   s_asr_is_sense_voice = false;  // true = SenseVoice API (full utterance, multilingual)

// ── Async state (written by localLLMUpdate, consumed by main.cpp loop) ───────
static String s_asr_accum           = "";    // ASR delta chunks accumulating
static String s_local_last_asr      = "";    // last complete ASR utterance
static bool     s_local_asr_ready     = false; // true = new transcript waiting
static bool     s_local_kws_fired     = false; // true = wake word just detected
static uint32_t s_kws_fired_at        = 0;     // millis() when KWS last fired (for timeout)

// ── Pipeline task state (inference + TTS run on Core 0, loop() stays free) ───
// Usage:
//   localLLMDispatch(transcript) — feeds input, triggers task; returns false if busy
//   s_pipeline_busy              — true while task is running (read from loop())
//   s_pipeline_done              — true when result is ready (read from loop())
//   s_pipeline_raw               — raw LLM output "[EMOTION] text" (read from loop())
//   s_pipeline_input             — transcript that was dispatched (read from loop())
static SemaphoreHandle_t s_pipeline_trigger = nullptr;
static volatile bool     s_pipeline_busy    = false;
static volatile bool     s_pipeline_done    = false;
static volatile bool     s_k144_error       = false; // true after Serial2 timeout/error — loop() re-inits
static String            s_pipeline_input   = "";
static String            s_pipeline_raw     = "";
// TTS mute window — prevents Whisper from transcribing K144 speaker output.
// Set to millis() + estimated playback duration after TTS fires; checked in
// the Whisper ASR handler so transcripts during playback are silently ignored.
static volatile uint32_t s_tts_mute_until  = 0;
// Auto-listen timestamp — when > 0 and millis() crosses it, loop() re-arms
// Whisper so the user can speak without repeating the wake word after a question.
static volatile uint32_t s_auto_listen_at = 0;
// Last TTS text spoken — used to detect Whisper picking up TTS output.
// A transcript that contains a substantial fragment of the last response is contaminated.
static String s_last_tts_text = "";
// Function call action from the current LLM response — set by pipeline task, consumed by loop().
// loop() reads s_pending_action_valid, dispatches the servo/expression call, clears the flag.
static volatile bool s_pending_action_valid = false;
static char          s_pending_action[16]   = "";

// ─── Init K144: audio → KWS → ASR → LLM ─────────────────────────────────────
// Call once in setup() when g_ai_provider == "local". Returns true on success.
// LLM is standalone (direct text input). ASR is gated by KWS.
// Core2 bridges: localLLMUpdate() captures ASR transcripts, then loop()
// calls localLLMInfer() with the transcript.
inline bool localLLMBegin() {
    Serial.println("[LocalLLM] Initialising on Port C (GPIO 13/14, 115200 baud)...");
    Serial2.begin(115200, SERIAL_8N1, 13, 14);
    s_mod_llm.begin(&Serial2);

    // K144 runs Linux + StackFlow; it takes ~10–15 s to boot after power-on.
    // The ESP32 is ready in ~2 s, so we retry with progressive delays rather
    // than failing immediately. 6 attempts × 3 s = 18 s max wait.
    {
        bool connected = false;
        for (int attempt = 1; attempt <= 6; attempt++) {
            if (s_mod_llm.checkConnection()) { connected = true; break; }
            Serial.printf("[LocalLLM] K144 not ready (attempt %d/6) — waiting 3 s...\n", attempt);
            delay(3000);
        }
        if (!connected) {
            Serial.println("[LocalLLM] ERROR: No response after 18 s. Check power and cable.");

            showBootProgress("K144: no response — check cable", -1);
            return false;
        }
    }

    String ver = s_mod_llm.sys.version();
    Serial.printf("[LocalLLM] StackFlow firmware: %s\n", ver.c_str());

    // reset(true) blocks until K144 signals reset complete
    Serial.println("[LocalLLM] Resetting...");
    s_mod_llm.sys.reset(true);
    delay(500);  // brief extra settle after reset

    // ── System prompt ─────────────────────────────────────────────────────────
    // The emotion tag is placed at the START so the model outputs it immediately
    // before generating any tokens. Small models lose track of formatting rules
    // when they appear mid-generation.
    // /no_think disables qwen3 chain-of-thought mode (model is trained to respect this).
    // Must appear before any other instruction so the model sees it as a mode switch.
    static const char SYS[] =
        "/no_think "
        "You are Stack-chan, a cute friendly desktop robot. "
        "Reply in 1-2 sentences maximum. No lists, no newlines, no LaTeX, no markdown. "
        "Write all numbers and formulas in plain spoken words. "
        "Your reply MUST start with one of these mood tags: "
        "[HAPPY] [SAD] [ANGRY] [SLEEPY] [SURPRISED] [NEUTRAL] "
        "Optional: after the mood tag, add one action tag if it fits the request: "
        "[ACT:nod] [ACT:shake] [ACT:left] [ACT:right] [ACT:up] "
        "Use [ACT:left] or [ACT:right] when asked to look somewhere. "
        "Use [ACT:nod] for agreement. Use [ACT:shake] for disagreement. "
        "Example: [HAPPY] [ACT:nod] Yes, that is correct! "
        "Example: [NEUTRAL] [ACT:left] I think it is over there. "
        "Example: [HAPPY] Hello friend, I am doing great today! "
        "Example: [NEUTRAL] The area of a triangle is base times height divided by two.";

    // ── 1. Audio bus (sys.pcm) ───────────────────────────────────────────────
    {
        m5_module_llm::ApiAudioSetupConfig_t acfg;
        acfg.playVolume = g_k144_volume / 100.0f;  // 0–100 → 0.0–1.0
        Serial.printf("[LocalLLM] Audio bus: playVolume=%.2f (g_k144_volume=%d)\n",
                      acfg.playVolume, (int)g_k144_volume);
        String wid = s_mod_llm.audio.setup(acfg);
        if (wid.isEmpty()) {
            Serial.println("[LocalLLM] WARNING: audio bus failed — ASR/TTS may not work.");
        } else {
            Serial.printf("[LocalLLM] Audio  ready  work_id=%s\n", wid.c_str());
        }
    }

    // ── 2. KWS — configurable wake word (default "HELLO") ───────────────────
    // enaudio=false: tells StackFlow NOT to play the KWS confirmation beep.
    // The beep activates the K144 speaker amp which causes a current spike that
    // drops the 5V rail below the ESP32 brownout threshold on weak USB supplies.
    {
        m5_module_llm::ApiKwsSetupConfig_t cfg;
        cfg.kws       = g_kws_word;
        cfg.setParam("enaudio", false);   // suppress KWS beep → prevents brownout on USB power
        s_kws_work_id = s_mod_llm.kws.setup(cfg, "kws_setup", "en_US");
        if (s_kws_work_id.isEmpty()) {
            Serial.println("[LocalLLM] ERROR: KWS setup failed.");
            return false;
        }
        s_kws_ready = true;
        Serial.printf("[LocalLLM] KWS   ready  work_id=%s  keyword=%s\n",
                      s_kws_work_id.c_str(), g_kws_word.c_str());
    }

    // ── 3. ASR — model selectable at runtime ─────────────────────────────────
    // Four paths:
    //   sherpa-ncnn      — KWS-gated, streaming deltas, object="asr.utf-8.stream"
    //   sherpa-bilingual — KWS-gated, streaming, Chinese+English bilingual
    //   whisper-*        — KWS+VAD-gated, full utterance, object="asr.utf-8"
    //   sense-voice      — KWS-gated, full utterance, multilingual (EN/ES/DE/JA/KO/ZH)
    // All require KWS gating — standalone ASR never delivers messages (K144 constraint).
    s_asr_is_whisper    = g_local_asr_model.startsWith("whisper");
    s_asr_is_sense_voice = (g_local_asr_model == "sense-voice");

    if (s_asr_is_whisper) {
        // Whisper: sends one complete transcript per utterance (not streaming).
        // Requires llm-whisper package installed on K144 (separate from llm-asr).
        // Requires llm-vad + llm-model-silero-vad for speech segmentation.
        //
        // VAD: setup first so Whisper can use VAD output as gate
        {
            m5_module_llm::ApiVadSetupConfig_t vcfg;
            // vcfg.input = {"sys.pcm"} by default
            String vwid = s_mod_llm.vad.setup(vcfg, "vad_setup");
            if (!vwid.isEmpty()) {
                s_vad_work_id = vwid;
                Serial.printf("[LocalLLM] VAD   ready  work_id=%s\n", vwid.c_str());
            } else {
                Serial.println("[LocalLLM] WARNING: VAD setup failed — Whisper will use sys.pcm directly. "
                               "Install: apt install --allow-unauthenticated llm-vad llm-model-silero-vad");
            }
        }

        m5_module_llm::ApiWhisperSetupConfig_t cfg;
        cfg.model = g_local_asr_model;  // "whisper-small"
        // Whisper input_type is "sys.pcm" only — adding any work_id to input crashes the service.
        // Use default input {"sys.pcm"}: Whisper runs continuously.
        // Software-gate: localLLMUpdate() only accepts transcripts after s_local_kws_fired.
        // cfg.input left at default {"sys.pcm"}
        // enkws=false: suppress ASR service's built-in KWS voice acknowledgment (male voice on HELLO).
        cfg.setParam("enkws", false);
        s_asr_work_id = s_mod_llm.whisper.setup(cfg, "asr_setup", "en_US");
        if (s_asr_work_id.isEmpty()) {
            Serial.printf("[LocalLLM] WARNING: Whisper setup failed (%s) — falling back to sherpa-ncnn.\n"
                          "[LocalLLM]   Install: apt install --allow-unauthenticated llm-whisper llm-model-%s\n",
                          g_local_asr_model.c_str(), g_local_asr_model.c_str());
            s_asr_is_whisper = false;
        } else {
            Serial.printf("[LocalLLM] ASR   ready  work_id=%s  model=%s (Whisper)\n",
                          s_asr_work_id.c_str(), g_local_asr_model.c_str());
        }
    }

    if (s_asr_is_sense_voice) {
        // SenseVoice: multilingual ASR (EN/ES/DE/JA/KO/ZH), 10 s chunks.
        // Uses the asr API with sense-voice-small-10s model.
        // Delivers a complete utterance per message (object="asr.utf-8").
        m5_module_llm::ApiAsrSetupConfig_t cfg;
        cfg.model           = "sense-voice-small-10s";
        cfg.response_format = "asr.utf-8";
        cfg.input           = {"sys.pcm", s_kws_work_id};
        cfg.enkws           = false;  // suppress KWS voice acknowledgment
        s_asr_work_id = s_mod_llm.asr.setup(cfg, "asr_setup");
        if (s_asr_work_id.isEmpty()) {
            Serial.println("[LocalLLM] WARNING: SenseVoice setup failed — falling back to sherpa-ncnn.\n"
                           "[LocalLLM]   Install: apt install --allow-unauthenticated llm-model-sense-voice-small-10s");
            s_asr_is_sense_voice = false;
        } else {
            Serial.printf("[LocalLLM] ASR   ready  work_id=%s  model=sense-voice-small-10s (multilingual)\n",
                          s_asr_work_id.c_str());
        }
    }

    if (!s_asr_is_whisper && !s_asr_is_sense_voice) {
        // sherpa-ncnn (English) or sherpa-onnx bilingual (Chinese+English).
        // Streaming deltas; hardware-gated by KWS.
        m5_module_llm::ApiAsrSetupConfig_t cfg;
        cfg.input = {"sys.pcm", s_kws_work_id};
        cfg.enkws = false;  // suppress KWS voice acknowledgment (male voice on HELLO fire)
        if (g_local_asr_model == "sherpa-bilingual") {
            cfg.model = "sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16";
        }
        // else: default model = sherpa-ncnn-streaming-zipformer-20M-2023-02-17
        s_asr_work_id = s_mod_llm.asr.setup(cfg, "asr_setup", "en_US");
        if (s_asr_work_id.isEmpty()) {
            Serial.println("[LocalLLM] ERROR: ASR setup failed. "
                           "Install: apt install llm-asr");
            return false;
        }
        Serial.printf("[LocalLLM] ASR   ready  work_id=%s  model=%s\n",
                      s_asr_work_id.c_str(),
                      g_local_asr_model == "sherpa-bilingual" ? "sherpa-onnx-bilingual" : "sherpa-ncnn");
    }

    // ── 4. LLM — standalone (direct text input, NOT pipeline-connected) ──────
    // We do NOT set cfg.input = {asr_work_id, kws_work_id} here.
    // Connecting LLM to ASR in pipeline mode + subsequent MeloTTS setup
    // causes K144 to flood Serial2 and crashes the ESP32 during setup().
    // Instead, Core2 calls localLLMInfer(transcript) after ASR finishes.
    //
    // IMPORTANT: 1.5B models require the qwen-tokenizer HTTP service (systemd)
    // which starts after the K144 resets. If llm.setup() is called before the
    // service is up, it returns an incomplete work_id (e.g. "llm" not "llm.1004")
    // and all inferences fail with error -97.
    //
    // A valid work_id always contains a dot (e.g. "llm.1004"). Retry up to 5×
    // with 5 s between attempts rather than guessing a fixed upfront delay.
    {
        m5_module_llm::ApiLlmSetupConfig_t cfg;
        cfg.model         = g_local_llm_model;
        cfg.prompt        = SYS;
        cfg.max_token_len = g_local_max_tokens;
        cfg.enkws         = false;  // suppress auto-spoken LLM response on KWS fire (male voice)

        // A valid work_id contains a dot (e.g. "llm.1004").
        // A bare "llm" means the qwen-tokenizer HTTP service isn't ready yet.
        //
        // qwen2.5-1.5B and qwen3-0.6B share the same Qwen tokenizer vocabulary
        // (both return eos_id 151645). A single tokenizer_qwen2.5-1.5B-ax630c.py
        // service serves both — no switching needed.
        // Measured startup: K144 Linux boot + Python transformers load = ~65 s.
        // Wait 70 s then retry up to 3× with 10 s gaps.
        // Do NOT call exit() between retries — corrupts K144 state.
        // Do NOT hammer rapidly — floods the service and resets its startup timer.
        {
            bool needs_tokenizer = g_local_llm_model.indexOf("1.5B") >= 0 ||
                                   g_local_llm_model.indexOf("1.5b") >= 0 ||
                                   g_local_llm_model.indexOf("qwen3") >= 0;
            if (needs_tokenizer) {
                // sys.reset() triggers a full K144 Linux reboot (~30 s boot +
                // ~35 s for systemd + Python transformers to load = ~65 s total).
                // Show animated boot progress so the user knows we're not frozen.
    
                Serial.println("[LocalLLM] Waiting 70 s for K144 reboot + qwen-tokenizer service...");
                const uint32_t WAIT_MS = 70000;
                uint32_t t0 = millis();
                while (millis() - t0 < WAIT_MS) {
                    uint32_t elapsed = millis() - t0;
                    int pct = (int)(elapsed * 100 / WAIT_MS);
                    char label[48];
                    snprintf(label, sizeof(label), "K144 starting... %ds", (int)(elapsed / 1000));
                    showBootProgress(label, pct);
                    delay(400);
                }
            }
        }
        const int MAX_TRIES = 3;
        const int RETRY_MS  = 10000;
        for (int attempt = 1; attempt <= MAX_TRIES; attempt++) {
            s_llm_work_id = s_mod_llm.llm.setup(cfg);
            if (!s_llm_work_id.isEmpty() && s_llm_work_id.indexOf('.') >= 0) break;

            char label[48];
            snprintf(label, sizeof(label), "LLM retry %d/%d — tokenizer...", attempt, MAX_TRIES);
            showBootProgress(label, -1);
            Serial.printf("[LocalLLM] LLM attempt %d/%d work_id='%s' — waiting %ds...\n",
                          attempt, MAX_TRIES, s_llm_work_id.c_str(), RETRY_MS / 1000);
            s_llm_work_id = "";
            delay(RETRY_MS);
        }
        if (s_llm_work_id.isEmpty() || s_llm_work_id.indexOf('.') < 0) {
            Serial.printf("[LocalLLM] ERROR: LLM setup failed after %d attempts (%ds). "
                          "Is qwen-tokenizer.service running? (systemctl status qwen-tokenizer)\n",
                          MAX_TRIES, MAX_TRIES * RETRY_MS / 1000);
            return false;
        }
        Serial.printf("[LocalLLM] LLM   ready  model=%-30s  work_id=%s\n",
                      g_local_llm_model.c_str(), s_llm_work_id.c_str());
        s_llm_ready = true;
    }

    // ── 5. TTS — standalone text input, plays on K144 speaker ────────────────
    // Tries MeloTTS English first (v1.6+, natural neural TTS).
    // Falls back to legacy single-speaker TTS, then SAM on Core2 speaker.
    // enoutput=false (default): no PCM stream back to Core2.
    // enaudio=true (default): K144 plays audio on its own speaker.
    //
    // Wait for LLM model to finish loading into RAM before requesting more memory.
    // llm.setup() returns once K144 assigns a work_id, but the model continues
    // loading asynchronously. 0.5B: ~3 s. 1.5B: ~15 s.
    {
        bool is_heavy = g_local_llm_model.indexOf("1.5B") >= 0 ||
                        g_local_llm_model.indexOf("1.5b") >= 0 ||
                        g_local_llm_model.indexOf("qwen3") >= 0;
        // 0.5B: 10 s — sys.reset() restarts all StackFlow services including
        //   MeloTTS which needs time to reload its model (~8 s measured).
        // 1.5B/qwen3: 15 s — larger model loads more slowly.
        uint32_t tts_wait = is_heavy ? 15000 : 10000;
        Serial.printf("[LocalLLM] Waiting %u s for LLM to finish loading before TTS setup...\n",
                      tts_wait / 1000);
        delay(tts_wait);
    }
    {
        m5_module_llm::ApiMelottsSetupConfig_t cfg;
        cfg.model = g_local_tts_model;
        cfg.setParam("enkws", false);  // suppress StackFlow KWS-triggered audio (male voice)
        const char* tts_locale = g_local_tts_model.startsWith("melotts-es") ? "es_ES" : "en_US";
        // Retry up to 6× with 5 s gaps — MeloTTS service may still be loading
        // its model after sys.reset(). Do not call exit() between retries.
        for (int attempt = 1; attempt <= 6; attempt++) {
            char label[48];
            snprintf(label, sizeof(label), "TTS setup attempt %d/6...", attempt);
            showBootProgress(label, -1);
            Serial.printf("[LocalLLM] TTS melotts attempt %d/6 model=%s locale=%s\n",
                          attempt, g_local_tts_model.c_str(), tts_locale);
            s_melotts_work_id = s_mod_llm.melotts.setup(cfg, "melotts_setup", tts_locale);
            if (!s_melotts_work_id.isEmpty()) {
                s_melotts_ready = true;
                Serial.printf("[LocalLLM] TTS   ready  work_id=%s  model=%s\n",
                              s_melotts_work_id.c_str(), g_local_tts_model.c_str());
                break;
            }
            if (attempt < 6) delay(5000);
        }
    }
    if (!s_melotts_ready) {
        // Legacy TTS fallback — model file uses hyphens, library default uses underscores
        m5_module_llm::ApiTtsSetupConfig_t cfg;
        cfg.model = "single-speaker-english-fast";
        s_tts_work_id = s_mod_llm.tts.setup(cfg, "tts_setup", "en_US");
        if (!s_tts_work_id.isEmpty()) {
            s_tts_ready = true;
            Serial.printf("[LocalLLM] TTS   ready  work_id=%s  model=single-speaker-english-fast\n",
                          s_tts_work_id.c_str());
        } else {
            Serial.println("[LocalLLM] TTS setup failed — SAM on Core2 speaker active");
        }
    }

    Serial.println("[LocalLLM] Pipeline ready. Say the wake word then speak.");
    return true;
}

// ─── Call every loop() when LOCAL provider is active ─────────────────────────
// Reads one Serial2 message per call. Sets s_local_kws_fired when wake word
// is detected, s_local_asr_ready + s_local_last_asr when an utterance ends.
// main.cpp loop() consumes those flags to run localLLMInfer() + TTS.
inline void localLLMUpdate() {
    if (!s_llm_ready && !s_kws_ready) return;

    // Guard: skip the 50 ms getResponse() timeout when K144 has nothing to say.
    // Without this, every loop() stalls 50 ms even when idle, dropping the main
    // loop from ~100 Hz to ~16 Hz and making avatar / buttons / servo sluggish.
    if (!Serial2.available()) return;

    s_mod_llm.update();

    // Cap at 8 messages per loop() call — prevents loop() stalling for tens of ms
    // if K144 floods Serial2 (e.g. Whisper batch). Remainder processed next iteration.
    // Use index loop so we can erase only the processed prefix; unprocessed stay in list.
    constexpr int MAX_MSGS_PER_CALL = 8;
    auto& msgList = s_mod_llm.msg.responseMsgList;
    int process_count = (int)msgList.size() < MAX_MSGS_PER_CALL
                        ? (int)msgList.size() : MAX_MSGS_PER_CALL;
    for (int _mi = 0; _mi < process_count; _mi++) {
        auto& msg = msgList[_mi];


        // ── KWS event ─────────────────────────────────────────────────────────
        // Guard against double-fire: ignore KWS if ASR session already in progress.
        // Also ignore during TTS mute window — K144 mic picks up "Hello!" in TTS
        // output and hardware-fires KWS, starting a contaminated Whisper session.
        if (msg.work_id == s_kws_work_id && !s_local_kws_fired && !s_local_asr_ready) {
            if (millis() < s_tts_mute_until) {
                Serial.println("[LocalLLM] KWS ignored — TTS mute window active");
                continue;
            }
            Serial.println("[LocalLLM] KWS: 'HELLO' detected — ASR active");
            s_local_kws_fired = true;
            s_kws_fired_at    = millis();
        }

        // ── ASR transcript ────────────────────────────────────────────────────
        if (msg.work_id == s_asr_work_id) {
            JsonDocument doc;
            deserializeJson(doc, msg.raw_msg);
            if (doc["error"]["code"].as<int>() == 0) {

                if (msg.object == "asr.utf-8") {
                    // Full utterance: Whisper (continuous sys.pcm input) or SenseVoice.
                    // SenseVoice: hardware-gated by KWS (kws_work_id in input).
                    // Whisper: runs continuously — software-gated by s_local_kws_fired
                    //   so we only dispatch after wake word fires.
                    if (s_asr_is_whisper && !s_local_kws_fired) continue;
                    // Mute window: ignore Whisper transcripts while K144 speaker is
                    // playing — prevents mic feedback loop (TTS output → Whisper → loop).
                    if (s_asr_is_whisper && millis() < s_tts_mute_until) {
                        Serial.println("[LocalLLM] Whisper transcript ignored — TTS mute window active");
                        continue;
                    }
                    // Whisper: data is a plain string  {"data": " Hello."}
                    // SenseVoice/others: may use {"data": {"delta": "..."}} or {"data": {"text": "..."}}
                    const char* text = nullptr;
                    if (doc["data"].is<const char*>()) {
                        text = doc["data"].as<const char*>();
                    } else {
                        text = doc["data"]["delta"];
                        if (!text || !*text) text = doc["data"]["text"];
                    }
                    if (text && *text) {
                        String transcript = text;
                        transcript.trim();
                        // Reject very short transcripts — likely mic noise or hallucination.
                        if (transcript.length() < 10) {
                            Serial.printf("[LocalLLM] ASR: too short (%d chars), ignoring: \"%s\"\n",
                                          transcript.length(), transcript.c_str());
                            continue;
                        }
                        // Contamination check: reject if transcript contains a 25+ char
                        // substring from the last TTS response (Whisper picked up speaker).
                        if (s_asr_is_whisper && s_last_tts_text.length() >= 25) {
                            String tLow = transcript;  tLow.toLowerCase();
                            String rLow = s_last_tts_text; rLow.toLowerCase();
                            // Extract a 25-char probe from the middle of the TTS text
                            int probe_start = rLow.length() / 4;
                            String probe = rLow.substring(probe_start, probe_start + 25);
                            probe.trim();
                            if (probe.length() >= 20 && tLow.indexOf(probe) >= 0) {
                                Serial.printf("[LocalLLM] Whisper transcript contaminated (TTS echo), ignoring\n");
                                s_local_kws_fired = false;
                                continue;
                            }
                        }
                        Serial.printf("[LocalLLM] ASR (%s): \"%s\"\n",
                                      s_asr_is_whisper ? "whisper" : "sense-voice",
                                      transcript.c_str());
                        s_local_last_asr  = transcript;
                        s_local_asr_ready = true;
                        s_local_kws_fired = false;
                    }
                } else if (msg.object == "asr.utf-8.stream") {
                    // Streaming deltas: sherpa-ncnn (English) or sherpa-onnx (bilingual).
                    // REPLACE not append — each delta is the full growing transcript.
                    const char* delta = doc["data"]["delta"];
                    bool finish = doc["data"]["finish"] | false;
                    if (delta && *delta) s_asr_accum = delta;
                    if (finish) {
                        s_asr_accum.trim();
                        if (s_asr_accum.length() >= 10) {
                            Serial.printf("[LocalLLM] ASR (sherpa): \"%s\"\n",
                                          s_asr_accum.c_str());
                            s_local_last_asr  = s_asr_accum;
                            s_local_asr_ready = true;
                        } else {
                            Serial.printf("[LocalLLM] ASR: too short (%d chars), ignoring: \"%s\"\n",
                                          s_asr_accum.length(), s_asr_accum.c_str());
                            s_asr_accum = "";
                        }
                        s_local_kws_fired = false;
                        // s_asr_accum intentionally NOT cleared here — loop() shows it in
                        // the speech bubble until the pipeline dispatches, then clears it.
                    }
                }
            }
        }
    }

    // Erase only the messages we processed; leave any remainder for next call.
    if (process_count > 0)
        msgList.erase(msgList.begin(), msgList.begin() + process_count);
}

// ─── K144 TTS — fire text to K144 speaker, wait for estimated duration ───────
// Priority: MeloTTS English (natural neural TTS) → legacy TTS → SAM on Core2.
// Audio plays on the K144's own speaker when TTS/MeloTTS is active. Requires a
// good power supply — K144 speaker amp draws too much current on USB alone.
// Falls back to samSpeak() automatically when no K144 TTS is available.
inline void localK144Speak(const char* text) {
    if (!text || !*text) return;
    if (!s_melotts_ready && !s_tts_ready) {
        extern void samSpeak(const char*);
        samSpeak(text);
        return;
    }
    // Sanitize before sending to K144 TTS — strip LaTeX, code blocks, backslash
    // sequences, non-ASCII, and truncate at 280 chars at a sentence boundary.
    // MeloTTS chokes on the same inputs that break SAM.
    extern char* ttsSanitize(const char* text, size_t char_limit);
    char* safe = ttsSanitize(text, 280u);
    const char* speak = safe ? safe : text;
    Serial.printf("[LocalLLM/TTS] >>> %s\n", speak);
    // Fire and forget — K144 synthesises + plays asynchronously.
    if (s_melotts_ready) {
        s_mod_llm.melotts.inference(s_melotts_work_id, String(speak), 0);
    } else {
        s_mod_llm.tts.inference(s_tts_work_id, String(speak), 0);
    }
    // Estimate playback duration based on sanitized length (may be truncated).
    // 40 ms/char ≈ 150 wpm average speech rate; 800 ms base for TTS startup latency.
    uint32_t wait_ms = (uint32_t)(strlen(speak) * 40) + 800;
    if (safe) free(safe);
    delay(wait_ms);
    // Mute Whisper for 2 s after K144 speaker stops — prevents mic picking up TTS
    s_tts_mute_until = millis() + 2000;
}

// ─── Blocking LLM inference — push text, collect "[EMOTION] response" ────────
inline String localLLMInfer(const String& userText) {
    if (!s_llm_ready) return "[SAD] Local module not ready.";

    // qwen3 ignores /no_think in the system prompt but respects it in the user turn.
    // Prepend it for any qwen3 model so the model skips chain-of-thought entirely.
    String query = userText;
    if (g_local_llm_model.startsWith("qwen3")) {
        query = "/no_think " + userText;
    }

    String response = "";
    Serial.printf("[LocalLLM] >>> %s\n", query.c_str());
    Serial.print("[LocalLLM] <<< ");

    // Timeout: g_local_max_tokens × ~0.5 s/token + 10 s headroom.
    // At 80 tokens default that's ~50 s. Keep well above 30 s to avoid
    // false -97 errors on longer responses with heavier models.
    uint32_t infer_timeout_ms = (uint32_t)g_local_max_tokens * 500 + 10000;
    int err = s_mod_llm.llm.inferenceAndWaitResult(
        s_llm_work_id,
        query,
        [&response](String& delta) {
            response += delta;
            Serial.print(delta);
        },
        infer_timeout_ms
    );
    Serial.println();

    if (err != 0) {
        Serial.printf("[LocalLLM] Inference error %d — K144 may be disconnected or overloaded\n", err);
        s_k144_error = true;
        return "[SAD] I had trouble thinking locally.";
    }

    response.trim();

    // Strip qwen3 chain-of-thought <think>...</think> blocks.
    // qwen3 outputs reasoning inside <think> tags before the actual answer.
    // We remove the entire block so only the actual response reaches TTS.
    {
        int ts = response.indexOf("<think>");
        if (ts >= 0) {
            int te = response.indexOf("</think>", ts);
            if (te > ts) {
                // Complete block: keep text before and after it
                response = response.substring(0, ts) + response.substring(te + 8);
            } else {
                // Truncated block (hit max_token_len inside think): drop from <think> onward
                response = response.substring(0, ts);
            }
            response.trim();
            Serial.printf("[LocalLLM] stripped <think> block; cleaned: %s\n", response.c_str());
        }
    }

    // Discard any ASR/KWS messages that piled up in responseMsgList while
    // inferenceAndWaitResult() was blocking (up to 30 s of streaming data).
    // Without this the list grows until heap is exhausted and the device crashes.
    s_mod_llm.msg.responseMsgList.clear();

    // If <think> stripping left an empty response (model used all tokens on reasoning),
    // return a short verbal cue so TTS has something to say.
    if (!response.length()) {
        Serial.println("[LocalLLM] WARNING: response empty after <think> strip — model needs more tokens or /no_think");
        return "[NEUTRAL] Hmm, let me think about that.";
    }
    return response;
}

// ─── Pipeline FreeRTOS task — runs on Core 0 ─────────────────────────────────
// Waits for localLLMDispatch() to give s_pipeline_trigger, then:
//   1. Runs LLM inference with streaming TTS — fires MeloTTS per sentence as
//      tokens arrive rather than waiting for the full response first.
//   2. Writes s_pipeline_raw, sets s_pipeline_done, clears s_pipeline_busy.
//
// Streaming TTS works because melotts.inference(..., timeout=0) is fire-and-forget:
// it only writes to Serial2 and never reads back, so it is safe to call inside the
// inferenceAndWaitResult callback without interfering with the LLM token stream.
//
// MeloTTS ack messages from K144 accumulate in responseMsgList alongside LLM tokens
// and are cleared by responseMsgList.clear() at the end — they don't affect parsing.
static void localLLMPipelineTask(void*) {
    extern char* ttsSanitize(const char* text, size_t char_limit);  // defined in tts_client.h
    extern void  samSpeak(const char*);                              // defined in tts_client.h

    for (;;) {
        xSemaphoreTake(s_pipeline_trigger, portMAX_DELAY);
        s_pipeline_done = false;
        s_pipeline_busy = true;

        String query = s_pipeline_input;
        if (g_local_llm_model.startsWith("qwen3")) query = "/no_think " + query;

        String   response     = "";   // full raw LLM output (→ s_pipeline_raw)
        String   tts_buf      = "";   // in-progress sentence fragment
        String   sam_buf      = "";   // accumulates full text for SAM fallback
        bool     tag_cleared  = false;// true once the '[EMOTION]' closing ] has been passed
        uint32_t tts_total_ms = 0;    // sum of estimated playback durations sent to MeloTTS
        uint32_t tts_fired_at = 0;    // millis() of first melotts.inference() call
        s_last_tts_text = "";         // reset contamination guard for this response
        s_pending_action_valid = false; // reset function call from previous response

        // Fire one chunk to MeloTTS (non-blocking) or accumulate for SAM fallback.
        // LOCAL mode always uses MeloTTS → SAM; g_tts_provider is for cloud providers only.
        auto fireTTS = [&](String chunk) {
            chunk.replace("\r\n", " "); chunk.replace('\n', ' '); chunk.replace('\r', ' ');
            while (chunk.indexOf("  ") >= 0) chunk.replace("  ", " ");
            chunk.trim();
            if (chunk.isEmpty()) return;
            if (!s_melotts_ready) {
                // MeloTTS unavailable — accumulate for SAM fallback played at end
                if (sam_buf.length()) sam_buf += ' ';
                sam_buf += chunk;
                return;
            }
            char* safe = ttsSanitize(chunk.c_str(), 280u);
            const char* sp = safe ? safe : chunk.c_str();
            if (sp && *sp) {
                Serial.printf("[LocalLLM/TTS] >>> %s\n", sp);
                // timeout=0 → fire-and-forget, never reads Serial2, safe inside callback
                s_mod_llm.melotts.inference(s_melotts_work_id, String(sp), 0);
                if (tts_fired_at == 0) tts_fired_at = millis();
                tts_total_ms += (uint32_t)(strlen(sp) * 40) + 800;
                // Update subtitle — SubtitleOverlay reads g_subtitle_text each frame
                setSubtitleText(sp);
                // Accumulate spoken text for contamination detection
                if (s_last_tts_text.length()) s_last_tts_text += ' ';
                s_last_tts_text += String(sp);
            }
            if (safe) free(safe);
        };

        Serial.printf("[LocalLLM] >>> %s\n", query.c_str());
        Serial.print("[LocalLLM] <<< ");

        uint32_t timeout_ms = (uint32_t)g_local_max_tokens * 500 + 10000;

        int err = s_mod_llm.llm.inferenceAndWaitResult(
            s_llm_work_id, query,
            [&](String& delta) {
                response += delta;
                Serial.print(delta);

                // Skip the '[EMOTION]' prefix — don't speak it.
                // For qwen3 <think>...</think>[EMOTION] responses, the first ']'
                // found may be </think>; a second check advances past '[EMOTION]' too.
                // If the response grows past 20 chars with no '[', the model doesn't
                // use emotion tags at all — start streaming immediately.
                if (!tag_cleared) {
                    int b = response.indexOf(']');
                    if (b >= 0) {
                        String after = response.substring(b + 1);
                        after.trim();
                        // If the next non-space char is '[', we hit </think> not [EMOTION]
                        // — advance to the second ']'.
                        if (after.startsWith("[")) {
                            int b2 = response.indexOf(']', b + 1);
                            if (b2 >= 0) {
                                tag_cleared = true;
                                tts_buf = response.substring(b2 + 1);
                                tts_buf.trim();
                            }
                        } else {
                            tag_cleared = true;
                            tts_buf = after;
                        }
                    } else if (response.length() > 20) {
                        // No '[' found in first 20+ chars — model skips emotion tags.
                        // Treat the full accumulated response so far as speech.
                        tag_cleared = true;
                        tts_buf = response;
                    }
                    return;
                }

                tts_buf += delta;

                // Extract [ACT:xxx] function call tag if present (strip it from tts_buf).
                // The tag may arrive in tts_buf at any position (e.g. " [ACT:nod] text...").
                // Store in s_pending_action — loop() dispatches it after s_pipeline_done.
                if (!s_pending_action_valid) {
                    int actStart = tts_buf.indexOf("[ACT:");
                    if (actStart >= 0) {
                        int actEnd = tts_buf.indexOf(']', actStart + 5);
                        if (actEnd > actStart + 5) {
                            String act = tts_buf.substring(actStart + 5, actEnd);
                            act.trim();
                            strncpy(s_pending_action, act.c_str(), sizeof(s_pending_action) - 1);
                            s_pending_action[sizeof(s_pending_action) - 1] = '\0';
                            portMEMORY_BARRIER();  // ensure array write visible before flag
                            s_pending_action_valid = true;
                            // Remove the [ACT:xxx] tag from speech text
                            String before = tts_buf.substring(0, actStart);
                            String after2 = tts_buf.substring(actEnd + 1);
                            tts_buf = before + after2;
                            tts_buf.trim();
                            Serial.printf("[LocalLLM] Function call: %s\n", s_pending_action);
                        }
                    }
                }

                // Fire TTS when we hit a sentence boundary (need ≥12 chars to avoid clips)
                int best = -1;
                for (int i = (int)tts_buf.length() - 1; i >= 12; i--) {
                    char c = tts_buf[i];
                    if (c == '.' || c == '!' || c == '?') { best = i; break; }
                }
                if (best >= 0) {
                    String sent = tts_buf.substring(0, best + 1);
                    tts_buf     = tts_buf.substring(best + 1);
                    fireTTS(sent);
                }
            },
            timeout_ms
        );
        Serial.println();

        // Discard ASR/MeloTTS ack messages that arrived during inference
        s_mod_llm.msg.responseMsgList.clear();

        if (err != 0) {
            Serial.printf("[LocalLLM] Inference error %d — K144 may be disconnected or overloaded\n", err);
            s_k144_error = true;
            response = "[SAD] I had trouble thinking locally.";
            tts_buf  = "";
            fireTTS("I had trouble thinking locally.");
        }

        response.trim();

        // Strip qwen3 <think>...</think> blocks from the stored raw response
        {
            int ts = response.indexOf("<think>");
            if (ts >= 0) {
                int te = response.indexOf("</think>", ts);
                response = (te > ts) ?
                    response.substring(0, ts) + response.substring(te + 8) :
                    response.substring(0, ts);
                response.trim();
                Serial.printf("[LocalLLM] stripped <think>; cleaned: %s\n", response.c_str());
                // If no TTS fired yet (entire budget was <think> content), re-extract tts_buf
                if (tts_fired_at == 0 && sam_buf.isEmpty()) {
                    int b = response.indexOf(']');
                    tts_buf = (b >= 0) ? response.substring(b + 1) : response;
                    tts_buf.trim();
                }
            }
        }

        if (response.isEmpty()) {
            response = "[NEUTRAL] Hmm, let me think about that.";
            if (tts_buf.isEmpty() && sam_buf.isEmpty()) tts_buf = "Hmm, let me think about that.";
        }

        // Fallback: if the model didn't output an [EMOTION] tag (e.g. 1.5B model),
        // tag_cleared never became true and tts_buf was never populated.
        // Treat the full response as speech in that case.
        if (tts_fired_at == 0 && sam_buf.isEmpty() && tts_buf.isEmpty()) {
            tts_buf = response;
            // Strip emotion tag if present: "[NEUTRAL] text" → "text"
            int lb = tts_buf.indexOf('[');
            int rb = tts_buf.indexOf(']');
            if (lb >= 0 && rb > lb && rb < 30) tts_buf = tts_buf.substring(rb + 1);
            tts_buf.trim();
            Serial.println("[LocalLLM] No emotion tag — using full response as TTS input");
        }

        // Flush any remaining sentence fragment (text after last boundary)
        if (!tts_buf.isEmpty()) fireTTS(tts_buf);

        // SAM fallback: MeloTTS unavailable — play all collected text now
        if (!sam_buf.isEmpty()) {
            s_last_tts_text = sam_buf;
            samSpeak(sam_buf.c_str());
        }

        // Wait for in-flight MeloTTS playback to finish.
        // Also stamp the mute window so Whisper ignores K144 speaker output
        // for (estimated duration + 2 s) — prevents mic feedback loop.
        if (tts_fired_at > 0) {
            uint32_t elapsed = millis() - tts_fired_at;
            if (elapsed < tts_total_ms) delay(tts_total_ms - elapsed);
            s_tts_mute_until = millis() + 5000;  // 5 s tail after playback ends
        } else if (!sam_buf.isEmpty()) {
            // SAM path — mute window already elapsed (SAM is synchronous)
            s_tts_mute_until = millis() + 1000;
        }

        // Auto-listen: if response ends with '?' (LLM asked the user something),
        // schedule Whisper re-arm for when the mute window clears.
        // Only for Whisper — Sherpa needs the hardware KWS gate.
        {
            String trimmed = response;
            trimmed.trim();
            // Strip trailing punctuation duplicates: "today?." → check for '?'
            bool endsQuestion = false;
            for (int i = (int)trimmed.length() - 1; i >= 0; i--) {
                char c = trimmed[i];
                if (c == '?' ) { endsQuestion = true; break; }
                if (c != '.' && c != ' ' && c != '\n') break;
            }
            if (endsQuestion && s_asr_is_whisper) {
                // Re-arm after mute window + 500 ms buffer
                s_auto_listen_at = s_tts_mute_until + 500;
                Serial.printf("[LocalLLM] Response ends with '?' — auto-listen at mute+500ms\n");
            }
        }
        setSubtitleText("");   // clear subtitle — loop() will handle final state
        s_pipeline_raw  = response;
        portMEMORY_BARRIER();  // ensure s_pipeline_raw write visible before done flag
        s_pipeline_done = true;
        s_pipeline_busy = false;
    }
}

// ─── Dispatch a transcript to the pipeline task ───────────────────────────────
// Returns false if already busy or not initialised. Safe to call from loop().
inline bool localLLMDispatch(const String& transcript) {
    if (s_pipeline_busy || !s_llm_ready || s_pipeline_trigger == nullptr) return false;
    s_pipeline_input = transcript;
    xSemaphoreGive(s_pipeline_trigger);
    return true;
}

// ─── Change the KWS wake word at runtime ─────────────────────────────────────
// Tears down the current KWS service and restarts with the new keyword.
// Only safe to call when the pipeline is idle (!s_pipeline_busy).
// Saves the new word to persistent config automatically.
// Returns true on success.
inline bool localKWSRestart(const String& new_word) {
    if (s_pipeline_busy) {
        Serial.println("[LocalLLM] KWS change skipped — pipeline busy");
        return false;
    }
    m5_module_llm::ApiKwsSetupConfig_t cfg;
    cfg.kws = new_word;
    cfg.setParam("enaudio", false);   // suppress beep → prevents brownout
    String wid = s_mod_llm.kws.setup(cfg, "kws_setup", "en_US");
    if (wid.isEmpty()) {
        Serial.printf("[LocalLLM] KWS restart failed for keyword=%s\n", new_word.c_str());
        return false;
    }
    s_kws_work_id = wid;
    g_kws_word    = new_word;
    saveConfig();
    Serial.printf("[LocalLLM] KWS  changed  work_id=%s  keyword=%s\n",
                  wid.c_str(), new_word.c_str());
    return true;
}

// ─── Start background tasks — call once after localLLMBegin() returns true ───
// Guards against being called again on recovery — the existing llm_pipe task
// loops back to xSemaphoreTake() after each inference and picks up the new
// LLM context automatically (s_llm_work_id + s_llm_ready are globals).
inline void localLLMStartTasks() {
    if (s_pipeline_trigger == nullptr) {
        s_pipeline_trigger = xSemaphoreCreateBinary();
        xTaskCreatePinnedToCore(
            localLLMPipelineTask, "llm_pipe",
            8192,           // stack: inference JSON + String ops
            nullptr, 2,     // priority 2 — same as speaker/mic DMA tasks
            nullptr, 0);    // Core 0 — keeps Core 1 free for loop() + avatar
        Serial.println("[LocalLLM] Pipeline task started on Core 0");
    } else {
        Serial.println("[LocalLLM] Pipeline task already running — skipping re-create");
    }
}
