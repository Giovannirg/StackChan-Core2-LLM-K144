# Stack-chan Core2 — CLAUDE.md

Stack-chan is an AI-powered desktop robot companion on M5Stack Core2 (ESP32). Animated face,
dual-axis servo head, voice I/O, and AI conversation (Gemini / OpenAI / Claude / Realtime / Local).
Everything in this file is what took the most investigation to discover — read it before touching anything.

This file is intended for AI coding assistants (Claude, Copilot, etc.) and human contributors alike.
It documents non-obvious constraints, hardware quirks, and hard-won pitfalls so they don't have to be
rediscovered. If you find a new constraint or fix a new bug, please add it here.

---

## Quick orientation

| What | Where |
|------|-------|
| Compile-time defaults (WiFi, API keys, AI provider, greeting) | `src/ai_config.h` (gitignored — copy from `src/ai_config.h.example`) |
| Runtime config — load/save to LittleFS, AES-128-CTR encryption | `src/config_store.h` |
| Web UI to edit config at `http://<device-ip>` | `src/config_webserver.h` |
| Boot + main loop + AI orchestration | `src/main.cpp` |
| STT + LLM (Gemini / OpenAI / Claude / Realtime) | `src/ai_client.h` |
| OpenAI Realtime API WebSocket bidirectional audio | `src/realtime_client.h` |
| TTS streaming + ElevenLabs + lip-sync + SAM fallback | `src/tts_client.h` |
| Mic DMA recording + WAV construction | `src/audio_recorder.h` |
| Wake-word detection — currently disabled | `src/wake_word.h` |
| PWM + TTL SCS servo control + animations | `src/stackchan_servo.h` |
| Tone generation, speaker config | `src/stackchan_audio.h` |
| Avatar expression wrapper + true-black theme | `src/stackchan_expression.h` |
| Kawaii face definitions (Default / Cat / Capybara) | `src/avatar_faces.h` |
| UI overlays (recording, thinking, response, subtitles) | `src/conversation_ui.h` |
| Servo mode selection UI + NVS persistence | `src/init_screen.h`, `src/servo_mode.h` |
| K144 local LLM pipeline (KWS, ASR, LLM, MeloTTS) | `src/local_llm.h` |
| Minimal AudioOutput stub (for ESP8266SAM) | `lib/AudioOutputStub/AudioOutput.h` |
| Build-time version injection | `scripts/version.py` |
| User-editable runtime config (flash with `pio run -t uploadfs`) | `data/config.json` (gitignored — copy from `data/config.json.example`) |

Everything is header-only (no `.cpp` files). The entire program is ~2500 lines.

---

## Hardware

**Board:** M5Stack Core2 (ESP32, 240 MHz, 16 MB flash, 4 MB PSRAM)

| Resource | Usage |
|----------|-------|
| Core 0 | WiFi stack (pri 23), Speaker DMA task (pri 2), Mic DMA task (pri 2) |
| Core 1 | Arduino `loop()` (pri 1), Avatar RTOS task (pri 1) |
| SPI | Display + Avatar — **shared bus, requires synchronization** |
| I2S | Speaker Tx + Mic Rx — **reconfigured dynamically by M5Unified** |
| UART2 (GPIO 13/14) | TTL SCS servo (1 Mbps) or K144 LLM Module (same pins — mutually exclusive) |
| GPIO 32/33 | PWM tilt/pan servo (50 Hz) |
| PSRAM | All large audio buffers, API payloads |
| LittleFS | `/config.json` — runtime config (WiFi, API keys, voice, greeting) |

---

## Critical hardware constraints

### 1. SPI mutex — avatar task vs. loop()

The avatar renders its face at ~10 FPS from a FreeRTOS task on Core 1. If `loop()` writes to the
display at the same time, SPI corrupts and the device crashes.

**Rule:** call `stopAvatar()` + `delay(150)` before any direct display write; `startAvatar()` after.
The 150 ms gives the avatar task time to finish its current frame and release the SPI mutex.

```cpp
stopAvatar();          // avatar.stop() sets _isRunning=false; task finishes current frame
delay(150);            // wait for frame to complete before touching SPI
// ... safe to write display ...
startAvatar();
```

**Exception:** `avatar.setSpeechText()` and `avatar.setMouthOpenRatio()` are consumed by the
avatar task itself — they are safe to call from `loop()` without stopping the avatar.
We use `setSpeechText()` for "Listening...", "..." overlays, and TTS subtitles (lip-sync mode).

**`setAvatarLifecycle(stop_fn, start_fn)`** in `tts_client.h` registers callbacks so `samSpeak()`
can stop/start the avatar around `showReaderUI()` even though `tts_client.h` has no direct
reference to `stopAvatar()`. Registered in `setup()` alongside `setLipSyncAvatar()`.

### 2. I2S / GPIO0 — mic tears down speaker

`M5.Mic.end()` reconfigures I2S and GPIO0 (which is shared between the NS4168 speaker LRCK and
the SPM1423 mic CLK). After mic use, `M5.Speaker.begin()` is a **no-op** because `_begun=true`.

**Fix:** use `speakerReinitAfterMic()` (defined in `audio_recorder.h`) immediately after every
`M5.Mic.end()`. It is shared by `recordWhilePressed()`, `recordTimedSeconds()`, and `vadProbe()`
in `wake_word.h`. Do not inline it or remove calls to it.

```cpp
inline void speakerReinitAfterMic() {
    delay(80);                    // IDF 4.4 releases I2S platform asynchronously
    M5.Speaker.end();             // clears _begun flag
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate      = 48000;
    spk_cfg.dma_buf_len      = 1024;
    spk_cfg.dma_buf_count    = 16;   // 16 × 1024 = ~341 ms DMA headroom
    spk_cfg.task_pinned_core = 1;
    spk_cfg.task_priority    = 2;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.begin();
    delay(300);                   // I2S DMA stabilisation
    M5.Speaker.setVolume(100);
}
```

### 3. Mic must use non-blocking record

`M5.Mic.record(..., blocking=true)` signals I2S completion via a cross-task semaphore
(`xQueueGenericSend queue.c:832`). When called from `loop()` (Core 1) while the I2S DMA task
runs on Core 0, this triggers a FreeRTOS assert and crashes.

**Fix:** always use `blocking=false` with manual pacing (one 1024-sample read per 64 ms chunk):

```cpp
M5.Mic.record(chunk, 1024, RECORD_SAMPLE_RATE, false);
```

### 4. playRaw() is non-blocking — check isPlaying() carefully

`M5.Speaker.playRaw()` hands the buffer to the DMA task and returns immediately. Calling
`isPlaying()` right after always returns `false`. Must `delay(100)` first to let the DMA task
start consuming the buffer.

Additionally, `isPlaying()` goes `false` when the last chunk is **queued** to the DMA hardware,
not when it finishes playing. The DMA buffer is 16 × 1024 samples (~683 ms at 24 kHz) deep.
`playWithSubtitles()` guards against this with a three-part loop condition:

```cpp
while (next_word < word_count ||      // all words must be shown
       M5.Speaker.isPlaying()  ||      // DMA still active
       (millis() - start) < dur_ms + 800)  // full duration + drain margin
```

---

## Build flags — why they exist

| Flag | Reason |
|------|--------|
| `-DBOARD_HAS_PSRAM` | Enable `ps_malloc()` / PSRAM heap |
| `-mfix-esp32-psram-cache-issue` | Cache coherency fix required when PSRAM runs at 80 MHz |
| `board_build.arduino.psram_speed = 80` | PSRAM bus 40→80 MHz — doubles throughput for large buffer ops |
| `-UCONFIG_ARDUINO_LOOP_STACK_SIZE` | Undefine SDK's 8192 default first (suppresses redefinition warning) |
| `-DCONFIG_ARDUINO_LOOP_STACK_SIZE=16384` | HTTP + JSON + TTS call chains overflow the default 8 KB loop stack |
| `-DCORE_DEBUG_LEVEL=1` | Minimal serial debug (level 3 floods the serial monitor) |
| `board_build.f_cpu = 240000000L` | Lock CPU at max frequency |
| `board_build.filesystem = littlefs` | LittleFS for `/config.json` storage |

---

## FreeRTOS task map

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| WiFi stack | 0 | 23 | system | 802.11 radio — do not touch |
| Speaker DMA | 0 | 2 | system | `M5.Speaker.playRaw()` consumer |
| Mic DMA | 0 | 2 | system | `M5.Mic.record()` producer |
| `llm_pipe` | 0 | 2 | 8 KB | K144 LLM inference + TTS — started by `localLLMStartTasks()` |
| `webcfg` | 0 | 1 | 8 KB | HTTP config UI — started in `setup()` after `connectWiFi()` |
| Avatar | 1 | 1 | M5Unified | Face rendering at ~10 FPS |
| `loop()` (Arduino) | 1 | 1 | 16 KB | Buttons, servo, touch, idle animations |

**`llm_pipe` task flow:**
1. Waits on `s_pipeline_trigger` (binary semaphore given by `localLLMDispatch()`)
2. Sets `s_pipeline_busy = true`
3. Calls `localLLMInfer()` — blocks up to 30 s on Serial2
4. Extracts response text, calls `localK144Speak()` — blocks for estimated TTS duration
5. `portMEMORY_BARRIER()` then writes `s_pipeline_raw`, sets `s_pipeline_done = true`, clears `s_pipeline_busy`

**`loop()` coordination with `llm_pipe`:**
- `localLLMUpdate()` (Serial2 reader) is skipped while `s_pipeline_busy` — task owns Serial2
- `localLLMDispatch(transcript)` returns false if `s_pipeline_busy` — no double-dispatch
- Result consumed when `s_pipeline_done`: `portMEMORY_BARRIER()` then `parseAIResponse(s_pipeline_raw)` → `applyEmotion()`

**Cross-core memory barriers:**
Two `portMEMORY_BARRIER()` pairs guard shared state written on Core 0 and read on Core 1:
- `s_pipeline_raw` (String) / `s_pipeline_done` (volatile bool) — barrier before setting `done`
  on Core 0; barrier after reading `done` on Core 1. Without this, Core 1 can read a stale
  String while `done` is already visible (`volatile` prevents compiler reorder but not CPU
  store-buffer reorder across Xtensa LX6 cores).
- `s_pending_action[]` (char array) / `s_pending_action_valid` (volatile bool) — barrier between
  `strncpy()` and setting `valid`. Same race: array write must be flushed before the flag.

---

## OS / HAL optimisations already applied

These are in `setup()` and `connectWiFi()` — do not remove them:

- **`btStop()`** — Bluetooth is never used. Stopping it before WiFi reclaims ~40 KB heap.
- **`WiFi.setSleep(false)`** — Disables DTIM beacon sleep (~100 ms latency per packet otherwise).
- **`WiFi.setAutoReconnect(true)`** — Silently re-associates if the AP drops.
- **Speaker + Mic tasks pinned to Core 0, priority 2** — keeps DMA off Core 1.
- **`llm_pipe` task pinned to Core 0, priority 2** — K144 inference + TTS off the main loop.
- **`webcfg` poll interval: 20 ms** — saves ~175 context switches/sec on Core 0 vs. 5 ms.
- **`loop()` adaptive yield** — `vTaskDelay(busy ? 10 : 20 ms)`. Idle = 20 ms (avatar gets full
  time-slice); active (pipeline busy / KWS fired / Serial2 data) = 10 ms for responsive polling.
- **`localLLMUpdate()` message cap** — processes at most 8 `responseMsgList` entries per
  `loop()` call. Prevents `loop()` stalling for tens of ms during Whisper batch arrivals.

---

## Runtime config system

Config is loaded from `/config.json` on LittleFS at boot (`loadConfig()` in `config_store.h`).

**Sensitive fields** (WiFi password, all API keys) are stored **AES-128-CTR encrypted**:
- Key = HMAC-SHA256(salt=`"stackchan-cfg-v1"`, msg=WiFi station MAC) → first 16 bytes
- 16-byte random nonce from `esp_random()`, prepended to ciphertext
- Stored as `<ENC>base64(nonce || ciphertext)`

To flash a fresh `data/config.json`: `pio run -t uploadfs`
After the first `saveConfig()` call, sensitive fields are encrypted in-place.

**Provider switching** is fully runtime via `g_ai_provider` string in the web UI — no recompile needed.

**Model names are runtime-editable** via the web UI — `g_chat_model` and `g_gemini_tts_model`
override the compiled-in defaults from `ai_config.h`.

---

## Web config UI

`startConfigServer()` and `handleConfigServer()` in `config_webserver.h`.

- Available at `http://<WiFi-IP>` or `http://192.168.4.1` in AP fallback mode.
- AP mode (`SSID: stackchan-config`) activates automatically when STA WiFi fails at boot.
- Sensitive fields shown masked (first 6 chars + `...`); leave blank to keep current value.
- On save: updates globals → `saveConfig()` → `ESP.restart()`.

**Cards in the UI:** WiFi · API Keys · AI Models · Personality · Display & Audio · Servo Calibration · Firmware Info

The **Personality** card has a **Face Style** dropdown (`default` / `cat` / `capybara`).
The face is applied at boot before `avatar.init()`. Changes take effect after save + restart.

Custom face classes live in `src/avatar_faces.h`. They extend `Drawable` (not `Eye`/`Mouth`
which are `final`). The `Face` 10-arg constructor takes `Drawable*` + `BoundingRect*` pairs,
matching the pattern in the library's own `DogFace.h`. There is no `Cheek` slot — blush and
ears are drawn from inside the eye component by testing `getCenterX() < 160`.

The **AI Models** card has two editable fields:
- **Chat model** — LLM for conversation (e.g. `gemini-2.0-flash`, `gpt-4o-mini`, `claude-sonnet-4-6`)
- **Gemini TTS model** — must be a dedicated TTS model (`gemini-2.5-flash-preview-tts`).
  Regular chat/multimodal models return text, not audio. The TTS pipeline detects this via
  `streamScanEither("\"data\":", "\"text\":")` and falls back to SAM immediately.

---

## AI provider selection

Provider is **runtime-selectable** via the web config UI dropdown — no recompile needed.
`g_ai_provider` string dispatches at runtime in `aiChat()`, `speakText()`, `wakeTranscribe()`.

| Value | STT | LLM | TTS |
|-------|-----|-----|-----|
| `gemini` | Gemini (audio inline) | `g_chat_model` | `g_gemini_tts_model` |
| `openai` | Whisper | `g_chat_model` | OpenAI TTS (`tts-1`) |
| `claude` | Whisper (OpenAI key needed) | `g_chat_model` | OpenAI TTS (`tts-1`) |
| `realtime` | built-in (WSS audio) | GPT-4o Realtime | built-in (WSS audio, PCM16 24kHz) |
| `local` | K144 KWS+ASR (offline) | K144 LLM | MeloTTS on K144 / SAM on Core2 |

**`g_tts_provider`** overrides TTS regardless of AI provider:
- `"auto"` (default) — each provider uses its own TTS
- `"elevenlabs"` — all cloud providers use ElevenLabs TTS; does not apply to `local` or `realtime`

**Realtime API:** `realtimeChat()` handles STT, LLM, and TTS in one WebSocket round-trip.
`runAIPipeline()` skips `speakText()` when `g_ai_provider == "realtime"` because audio is
already played inside `realtimeChat()`.

### LOCAL provider — K144 Module-LLM (AX630C)

**Port C conflict:** GPIO 13/14 is shared with TTL SCS servo. `setup()` forces PWM mode when
`g_ai_provider == "local"` and servo mode is TTL_SCS.

**Pipeline (fully offline):**
```
K144 mic → KWS("HELLO") → ASR → transcript over Serial2 → Core2
Core2: localLLMInfer(transcript) → K144 LLM → MeloTTS on K144 speaker (SAM fallback on Core2)
```

**K144 speaker brownout — use `enaudio=false`:**

The K144 speaker amp draws a large current spike when KWS fires. On weak USB supplies this
drops the 5V rail below the ESP32 brownout threshold (~2.43V). Fix: pass
`cfg.setParam("enaudio", false)` in `ApiKwsSetupConfig_t` before `kws.setup()`. This suppresses
the KWS audio feedback entirely. `playVolume=0.0` does NOT prevent brownout.

**Critical K144 ASR constraint:**

K144 standalone ASR **never** works. `input = {"sys.pcm"}` only → ZERO messages, regardless of
`enkws` flag. You must include the KWS work_id:
`cfg.input = {"sys.pcm", kws_work_id}`.

**Required setup order:** `audio.setup()` → `kws.setup()` → `asr.setup(input={"sys.pcm", kws_work_id})` → `llm.setup()`

**`audio.setup()` is mandatory** before ASR/TTS — initialises `sys.pcm` (K144's audio bus).

**Do NOT connect LLM to ASR in pipeline mode** (`cfg.input = {asr_work_id, kws_work_id}`).
After LLM pipeline setup, K144 floods Serial2 with messages and subsequent `melotts.setup()`
crashes the ESP32. Keep LLM in standalone mode (default `input`).

**Whisper** requires both model data AND the service package:
`apt install --allow-unauthenticated llm-whisper llm-model-whisper-tiny`
`llm-whisper v1.8` supports `whisper-tiny` and `whisper-base` only (not `whisper-small`).
Whisper input must be `{"sys.pcm"}` only — adding KWS/VAD work_ids causes SIGINT crash.
Output `data` field is a plain string: `{"data": " Hello."}` not `{"data":{"delta":"..."}}`.
See pitfall #23 for all Whisper constraints.

**Installed packages (StackFlow v1.6, lib-llm v1.8):**
- `llm-asr` / sherpa-ncnn-streaming-zipformer-20m ← confirmed working English streaming ASR
- `llm-kws` + GigaSpeech KWS model
- `llm-melotts` / `melotts-en-default` ← **working** with `qwen2.5-0.5B-prefill-20e`
- `llm-whisper` v1.8 + `llm-model-whisper-tiny` ← **working**
- `llm-vad` / silero-vad ← sets up fine but cannot be used in Whisper input chain
- LLM: `qwen2.5-0.5B-prefill-20e` — recommended (fast, no CoT, no tokenizer service)
- LLM: `qwen2.5-1.5B-ax630c` — needs tokenizer HTTP service (see pitfall #22)
- LLM: `qwen3-0.6B-ax630c` — needs tokenizer service; has CoT overhead (see pitfall #24)

**`g_local_max_tokens` default: 80.** Increase to 200+ for qwen3.

**MeloTTS wait estimate:** `strlen(speak) * 40 ms + 800 ms`. Fire-and-forget.

**ASR timeout:** 8 s for Sherpa/SenseVoice, 40 s for Whisper.

### Function calling — `[ACT:xxx]` prompt engineering

The system prompt instructs the LLM to optionally emit `[ACT:nod/shake/left/right/up]` after
the `[EMOTION]` tag. The `llm_pipe` task strips this tag and stores it in `s_pending_action`.
`loop()` dispatches it to `servoController` after `s_pipeline_done`.

Reliability may be poor with a 0.5B model (not trained on function calling). Upgrade path:
install a Pulsar2-converted function-calling model alongside existing models — see the project
README for the JSON registration file format and tokenizer script requirements.

### ElevenLabs TTS

```
POST https://api.elevenlabs.io/v1/text-to-speech/{voice_id}/stream?output_format=pcm_24000
Headers: xi-api-key: {key}, Content-Type: application/json
Body: {"text":"...", "model_id":"eleven_multilingual_v2"}
```

Response is raw PCM16 at 24000 Hz (no WAV header). Buffered into PSRAM then fed into
`playWithSubtitles(..., 24000)`.

### OpenAI Realtime API

```
record (16 kHz PCM16) → resample 16→24 kHz → WSS wss://api.openai.com/v1/realtime
  → session.update + input_audio_buffer.append (base64, 3072 samples/frame)
  → input_audio_buffer.commit + response.create
  ← response.audio.delta (base64 PCM16 24kHz → PSRAM)
  ← response.audio_transcript.delta (text)
  ← response.done
→ playWithSubtitles(transcript, pcm, samples, 24000)
```

Auth headers: `Authorization: Bearer {openai_key}` + `OpenAI-Beta: realtime=v1`

Resampling: `rtResample16to24()` — 3:2 linear interpolation (2 input → 3 output samples).

### Gemini TTS request format

```json
{
  "system_instruction": {"parts": [{"text": "Read aloud the following text."}]},
  "contents": [{"parts": [{"text": "..."}]}],
  "generationConfig": {
    "responseModalities": ["AUDIO"],
    "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": "Fenrir"}}}
  }
}
```

**Do not** add `responseMimeType` — it only accepts text MIME types and returns HTTP 400.
`responseModalities: ["AUDIO"]` is the correct way to request audio output.

### SAM voice tuning

SAM defaults sound harsh. Current tuning in `tts_client.h`:
- Speed 64 (default 72) — slower = more articulate
- Pitch 64 (unchanged)
- Throat 110 (default 128) — less nasal
- Mouth 160 (default 128) — more open / brighter vowels

Output is ~22050 Hz, 2× linearly upsampled to ~44100 Hz before `playRaw()`. `samSpeak()` feeds
the upsampled buffer into `playWithSubtitles()` — same lip-sync and subtitle reveal as cloud TTS.

---

## Conversation flow

```
Boot:
  samSpeak(g_greeting)                   // local SAM — no cloud TTS on boot

BtnB held:
  → avatar.setSpeechText("Listening...")
  → recordWhilePressed()
  → avatar.setSpeechText("")

valid recording?
  no  → return
  yes →
    avatar.setSpeechText("...")
    buildWav()
    aiChat(wav)                            // HTTPS to provider, ~2–8 s
    avatar.setSpeechText("")
    applyEmotion()
    delay(400)
    speakText()                            // lip-sync: avatar STAYS running throughout
      → geminiSpeak() / openaiSpeak() / elevenLabsSpeak()
        → playWithSubtitles()
        → if HTTP error: samSpeak()
    applyEmotion()
    delay(500)
    servoController.center()
```

**Key architectural point:** the avatar never stops during the normal happy path.
`setSpeechText()` and `setMouthOpenRatio()` are thread-safe avatar-task API calls.
`stopAvatar()` is only called on error paths that need direct display writes.

---

## Lip-sync

`setLipSyncAvatar(&avatar)` is called in `setup()`. When set, `playWithSubtitles()` drives the
mouth via `avatar->setMouthOpenRatio()`. Peak amplitude over a 50 ms window is mapped to `[0, 1]`
with a 3× boost. Both cloud TTS and SAM go through `playWithSubtitles()`.

---

## SubtitleOverlay — in-avatar text rendering

For LOCAL mode, subtitles are rendered inside the avatar sprite (avoids the M5GFX balloon).

- `g_subtitle_text` (String, `conversation_ui.h`) — global read by the overlay at render time.
- `setSubtitleText(const char* t)` — thread-safe write from any task.
- `SubtitleOverlay` (`avatar_faces.h`) — wraps the face's `Mouth` Drawable; renders in the
  bottom 47 px of the 320×240 sprite (y=193–240).
- `avatar.getFace()->setMouth(new SubtitleOverlay(avatar.getFace()->getMouth()))` in `setup()`
  after `startAvatar()` and `avatar.setScale(1.0)`.

**Scale must be 1.0:** at scale 0.65 (library default), in-sprite text appears proportionally
smaller. Always call `avatar.setScale(1.0)` after `startAvatar()`.

**Do NOT call `setSpeechText()` for subtitles in LOCAL mode** — it creates a balloon overlay
that fights with `SubtitleOverlay`. Use `setSubtitleText()` instead.

**Char-count word wrap:** use `CHARS_PER_LINE = 34` (Font4 at 0.85 scale). Never call
`textWidth()` from `Drawable::draw()` — watchdog crash. See pitfall #31.

---

## Streaming TTS for LOCAL provider

MeloTTS is fired per sentence during LLM inference (Alexa-style latency).

- Sentence boundary = `≥ 12 chars` ending in `.`, `?`, `!`, `,` or `≥ 45 chars` word boundary.
- `melotts.inference(work_id, text, 0)` — timeout=0 is fire-and-forget, safe inside callback.
- `s_tts_mute_until` — KWS and Whisper both gated for 5 s after TTS completes.
- `s_auto_listen_at` — armed when response ends with `?` and ASR is Sherpa (not Whisper).

**Whisper feedback loop prevention:**
1. KWS + Whisper mute window (`s_tts_mute_until`)
2. Min length filter: transcripts < 10 chars discarded
3. Contamination check: 25-char probe from middle of `s_last_tts_text` rejects TTS echoes

---

## Conversation history

4 most recent user+assistant pairs in `s_history[]` (`ai_client.h`). FIFO eviction.
BtnC short-tap clears history (`convClear()`). Not persisted across reboots.

---

## Emotion tag protocol

System prompt format:
```
USER_SAID: <transcript>
[EMOTION] Response text
```

Valid tags: `HAPPY`, `SAD`, `ANGRY`, `SLEEPY`, `SURPRISED`, `NEUTRAL`. Falls back to `NEUTRAL`.
Trimmed before uppercasing — `[ HAPPY ]` (with spaces) parses correctly.

---

## Wake word — currently disabled

Implemented in `src/wake_word.h`, commented out in `loop()` pending VAD calibration.

1. Cooldown gate — probe at most once per `WAKE_COOLDOWN_MS` (1500 ms)
2. VAD probe — 512 samples (~32 ms), compute RMS; skip if `< WAKE_VAD_THRESHOLD`
3. Full 3-second recording
4. STT — `geminiTranscribe()` or `whisperTranscribe()`
5. Keyword match — configurable string in `wake_word.h`

To re-enable: measure ambient RMS from serial, set `WAKE_VAD_THRESHOLD` ~30% above baseline,
uncomment `handleWakeWord()` in `loop()`.

---

## Idle animations & interaction tracking

`lastInteraction` stamped on: `onTouchStart()`, `BtnA/C wasPressed()`, `runAIPipeline()` start,
K144 `s_pipeline_done` result. Any new interaction path should stamp `lastInteraction = millis()`.

- **Active** (< 30 s): random glances every 3 s via `servoController.moveRelative()`.
- **Idle** (≥ 30 s): `servoController.breathe()` — sine-wave on both axes.

`applyEmotion()` drives servo in sync:
- `HAPPY` → `nod()`, `SAD`/`SLEEPY` → `tiltDown()`, `ANGRY` → `shake()`, `SURPRISED` → `lookUp()`, `NEUTRAL` → `center()`.

---

## Servo modes

Selected at first boot (or hold BtnA at boot), saved to NVS:

- **PWM** — SG90-class on GPIO 32 (tilt) / GPIO 33 (pan). Software smoothing 2°/frame.
- **TTL SCS** — Feetech SCS0009 via UART2 (GPIO 13 TX, GPIO 14 RX, 1 Mbps). Hardware closed-loop.

Runtime calibration: `g_servo_pan_min/max` (45°/135°), `g_servo_tilt_min/max` (70°/110°).
All four persisted in `/config.json`. BtnC long-hold runs `runServoTest()`.

`breathe()` — sine-wave sway (tilt: 0.8 rad/s ±4°, pan: 0.3 rad/s ±6°).
`lookAtScreenCoord(x, y)` — maps display pixel to servo angles; X is inverted (left on screen =
pan right from robot's perspective).

---

## Memory budget (approximate)

| Buffer | Size | Pool |
|--------|------|------|
| Recording (8 s @ 16 kHz) | 256 KB | PSRAM |
| WAV (recording + 44 B header) | ~256 KB | PSRAM |
| TTS PCM Gemini / OpenAI (10 s @ 24 kHz) | ~480 KB | PSRAM |
| SAM PCM pre-upsample (~22 kHz) | ~440 KB | PSRAM |
| SAM PCM upsampled 2× (~44100 Hz) | ~880 KB | PSRAM |
| API JSON payload | 2–5 KB | PSRAM |
| Conversation history (4 pairs) | ~1.2 KB | SRAM |
| Avatar frame buffers | ~100 KB | SRAM |

SAM briefly holds both buffers simultaneously (~1.3 MB peak). All large buffers use `ps_malloc()`.

---

## Common pitfalls

1. **No sound after recording** — speaker re-init after `M5.Mic.end()` was removed. See constraint #2.
2. **FreeRTOS assert `queue.c:832`** — `blocking=true` passed to `M5.Mic.record()`. See constraint #3.
3. **Display corruption / crash** — direct display write while avatar is running. See constraint #1.
4. **`playRaw → OK` but silent** — missing `delay(100)` before `isPlaying()` loop. See constraint #4.
5. **SAM won't compile** — `lib/AudioOutputStub/AudioOutput.h` missing or moved to `src/`.
6. **Loop stack overflow** — 16 KB is set; adding deep call chains can still overflow it.
7. **`M5.Speaker.begin()` is a no-op** — call `M5.Speaker.end()` first to clear `_begun` flag.
8. **`showReaderUI` / `showErrorUI` invisible** — called while avatar running. Must `stopAvatar()` first (or use `s_av_stop_fn` callback in `tts_client.h`).
9. **Wake word fires constantly** — `WAKE_VAD_THRESHOLD` below ambient RMS. Measure floor, set ~30% above.
10. **Config not persisting** — `pio run -t uploadfs` overwrites `data/config.json`. Only use for first-time setup.
11. **Gemini TTS returns text instead of audio** — TTS model must be a dedicated TTS model (`gemini-2.5-flash-preview-tts`), not a chat model.
12. **`responseMimeType` in Gemini TTS body** — returns HTTP 400. Use only `responseModalities: ["AUDIO"]`.
13. **Last word cut off / subtitles clear early** — `isPlaying()` goes false ~700 ms before DMA drains. Do not simplify the three-part loop condition in `playWithSubtitles()`.
14. **Face style not changing** — `g_face_style` must be exactly `"default"`, `"cat"`, or `"capybara"`.
15. **Custom face compile errors** — `Eye`, `Mouth`, `Eyeblow` are `final`; extend `Drawable` instead. `Eyeblow` (not `Eyebrow`) takes `(width, height, isLeft)`. `BoundingRect(top, left)` — first arg is Y.
16. **K144 ASR produces no messages** — standalone ASR (`input = {"sys.pcm"}` only) never works. Must include KWS work_id: `cfg.input = {"sys.pcm", kws_work_id}`. Confirmed exhaustively.
17. **K144 `audio.setup()` missing** — required before ASR/TTS to initialise `sys.pcm`. Without it, mic is silent.
18. **K144 pipeline-LLM + MeloTTS crashes ESP32** — connecting LLM to ASR floods Serial2; subsequent `melotts.setup()` crashes. Keep LLM in standalone mode.
19. **K144 setup crash after "LLM ready" log** — crash is in the next setup call. Remove it and use SAM TTS until root cause is confirmed.
20. **K144 KWS fires → ESP32 powers off on weak USB** — speaker amp current spike. Fix: `cfg.setParam("enaudio", false)` in `ApiKwsSetupConfig_t`. `playVolume=0.0` does NOT prevent it.
21. **K144 inference heap exhaustion** — without clearing `responseMsgList` after `localLLMInfer()`, hundreds of ASR messages accumulate and crash. Call `s_mod_llm.msg.responseMsgList.clear()` at end.
22. **K144 qwen2.5-1.5B needs tokenizer HTTP service** — `tokenizer_type: 2` requires a Python HTTP server at `localhost:8080`. Create a systemd service on K144 running `tokenizer_qwen2.5-1.5B-ax630c.py`. Verify: `curl -s http://localhost:8080/eos_id` → `{"eos_id": 151645}`.
23. **K144 Whisper constraints (exhaustive):**
    - `llm-whisper` service package must be installed separately from `llm-model-whisper-tiny`
    - `llm-whisper v1.8` supports only `whisper-tiny` and `whisper-base` — `whisper-small` causes SIGINT crash
    - Whisper `input` must be `{"sys.pcm"}` only — adding KWS/VAD work_ids causes SIGINT crash
    - Whisper `data` field is a plain string: `doc["data"].as<const char*>()`, not `doc["data"]["delta"]`
    - Use `continue` not `break` in the software gate — `break` skips KWS events in the same batch
    - ASR timeout must be ≥ 40 s for Whisper (Sherpa: 8 s)
24. **qwen3 chain-of-thought exhausts token budget** — `<think>...</think>` consumes all tokens. Set `max_token_len` to 256+. `/no_think` in prompts is ignored by the ax630c build. Strip `<think>` in `localLLMInfer()`.
25. **K144 legacy TTS broken in fw v1.6** — `single_speaker_english_fast` was removed. Use `melotts-en-default`.
26. **qwen-tokenizer service: one script serves all Qwen models** — `qwen2.5-1.5B` and `qwen3-0.6B` share the same vocabulary (`eos_id: 151645`). A bad work_id (`"llm"` instead of `"llm.1004"`) means the tokenizer service is wrong or not running.
27. **KWS brownout — only `enaudio=false` works** — `playVolume=0.0` does NOT suppress the speaker amp. See pitfall #20.
28. **`s_asr_accum` clears before display updates** — Sherpa often sends all deltas + `finish=true` in one burst. Do NOT clear `s_asr_accum` on `finish`. Let `loop()` clear it after `s_pipeline_done`.
29. **WiFi badge overwrites other speech text** — `handleIdleAnimations()` calls `avatar.setSpeechText("No WiFi")` every 5 s. Guard with `&& !s_pipeline_busy && s_asr_accum.isEmpty()` if conflicts arise.
30. **`showBootProgress()` extern declaration fails to link** — it is `inline` in `conversation_ui.h`. `extern` declarations fail. Fix: `#include "conversation_ui.h"` directly wherever it is needed.
31. **`textWidth()` inside avatar task causes watchdog crash** — calling `spi->textWidth()` per word per frame in `Drawable::draw()` triggers a watchdog reset. Use char-count wrapping (`CHARS_PER_LINE` constant) only.
32. **All StackFlow services have `enkws=true` by default** — set `enkws=false` on every service to suppress the male-voice KWS acknowledgement. Use `cfg.setParam("enkws", false)` for Whisper and MeloTTS (no struct field).
33. **MeloTTS v1.6 model name overridden in the library** — `api_melotts.cpp` hardcodes `melotts-en-default` for non-zh/non-ja locales. Log lines showing other model names are misleading.
34. **Streaming MeloTTS (timeout=0) is safe inside `inferenceAndWaitResult` callback** — it only writes to Serial2. Do NOT use a non-zero timeout inside this callback.
35. **Avatar `setScale(1.0)` must be explicit** — library default is 0.65. Always call `avatar.setScale(1.0)` after `startAvatar()` when using `SubtitleOverlay`.
36. **Auto-listen unreliable for Whisper** — 30s chunks capture TTS noise. `s_auto_listen_at` is only armed when `!s_asr_is_whisper`.
37. **`volatile` alone does not synchronise cross-core data on ESP32** — `volatile` prevents compiler reorder but NOT CPU store-buffer reorder across Xtensa LX6 cores. Always pair `volatile` flag handoffs with `portMEMORY_BARRIER()` on both sides.
38. **`localLLMUpdate()` must cap messages per call** — cap at `MAX_MSGS_PER_CALL = 8`; erase processed prefix with `msgList.erase(begin, begin + N)`. Do NOT use `clear()` when a cap is in place.
