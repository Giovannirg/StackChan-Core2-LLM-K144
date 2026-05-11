# Stack-chan Core2 — Functional Specification Document

**Status:** Draft v0.3
**Platform:** M5Stack Core2 (ESP32)

---

## 1. Product Overview

Stack-chan is a desktop AI companion robot. It has an animated face, a dual-axis servo head, a
microphone, a speaker, and an internet connection. The user interacts with it by voice: hold a
button and speak, and Stack-chan answers in natural language using a configurable AI backend.

The device is designed to sit on a desk and be left on. It should feel alive — blinking,
looking around, reacting to touch, and occasionally surprising the user.

---

## 2. Hardware Summary

| Component | Detail |
|-----------|--------|
| MCU | ESP32 (240 MHz dual-core, 4 MB PSRAM, 16 MB flash) |
| Display | 2" IPS LCD, 320×240, SPI |
| Touch | FT6336 capacitive touch on display |
| Speaker | NS4168 internal amplifier, I2S |
| Microphone | SPM1423 PDM mic, I2S (shares GPIO0 with speaker) |
| Buttons | BtnA (left), BtnB (middle), BtnC (right) — physical tactile |
| Servos | Option A: SG90-class PWM on GPIO32/33 (tilt/pan); Option B: Feetech SCS0009 TTL smart servos via UART2 GPIO13/14 |
| Connectivity | 802.11 b/g/n WiFi |
| Storage | LittleFS on flash partition for runtime config |
| Power | USB-C or internal LiPo |

---

## 3. User Interactions

### 3.1 Physical Buttons

| Button | Short Press | Long Hold (≥ 1.5 s) | At Boot |
|--------|-------------|----------------------|---------|
| **BtnA** | Happy expression + nod | — | Hold to re-run servo type selector |
| **BtnB** | — | **Hold to record voice → AI conversation** | — |
| **BtnC** | Clear AI conversation history | Run servo test sequence | — |

### 3.2 Touch Zones

The display is divided into regions. Touch anywhere while the avatar is showing:

| Zone | Behaviour |
|------|-----------|
| Top third | Happy expression + nod + happy sound |
| Middle left | Doubt expression + look left |
| Middle centre | Sad expression + surprise sound |
| Middle right | Doubt expression + look right |
| Bottom third | Sleepy expression + sleepy sound + tilt down |

Touch end: 500 ms delay, then return to Neutral + center servos.

### 3.3 Voice Input (BtnB Hold-to-Record)

1. User holds BtnB.
2. Avatar shows "Listening..." in speech bubble.
3. Audio is recorded from the mic at 16 kHz, 16-bit mono, up to 8 seconds.
4. On release (or 8 s timeout), recording stops.
5. If less than 0.5 s of audio: discard silently.
6. Valid recording → AI pipeline (§5).

### 3.4 Wake Word (Planned — currently disabled)

Passive, hands-free activation on the phrase **"Oye chispita"** (Spanish).

1. Every ~1.5 s, a short VAD probe (32 ms) checks ambient energy.
2. If energy is above threshold, a 3-second recording is taken.
3. Audio is transcribed by the AI provider.
4. If the transcript contains "chisp" (covers "chispita", "chispa" and spelling variants), the
   wake is confirmed.
5. Stack-chan plays a confirmation chirp + happy nod expression.
6. A 5-second command recording window opens ("Listening..." in speech bubble).
7. The command recording goes through the full AI pipeline (§5).

**Current status:** disabled — ambient noise floor (~14 000 RMS) exceeds the default threshold.
Needs calibration before re-enabling. See `src/wake_word.h`.

---

## 4. Boot Sequence

1. Load runtime config from LittleFS (`/config.json`); fall back to compiled-in defaults.
2. Init M5Stack hardware (display, speaker, mic).
3. Stop Bluetooth — not needed, reclaims ~40 KB heap.
4. Check BtnA: if held, or if no servo mode is saved, run **servo type selector**.
5. Splash screen: "Stack-chan / AI Companion".
6. Connect to WiFi (15 s timeout). On failure: start AP `stackchan-config` (192.168.4.1).
7. Start config web server.
8. Show IP address or AP URL on display for 2 s.
9. Init servos → center position.
10. Init speaker.
11. Start avatar face animation.
12. Apply true-black theme and Neutral expression.
13. Register lip-sync + avatar lifecycle callbacks with TTS engine.
14. Play startup tone.
15. Happy expression + nod + speak boot greeting via **SAM** (local, no cloud TTS on boot).
16. Return to Neutral + center. Device is ready.

---

## 5. AI Conversation Pipeline

### 5.1 Provider Selection

Set at compile time via `AI_PROVIDER` in `ai_config.h`. Three providers are supported:

| Provider | STT | LLM | TTS |
|----------|-----|-----|-----|
| Gemini | Audio sent inline (no separate STT step) | `g_chat_model` (default: `gemini-2.0-flash`) | `g_gemini_tts_model` (default: `gemini-2.5-flash-preview-tts`) |
| OpenAI | Whisper-1 | `g_chat_model` (default: `gpt-4o-mini`) | OpenAI TTS (`tts-1`, `nova` voice) |
| Claude | Whisper-1 (OpenAI key required) | `g_chat_model` (default: `claude-sonnet-4-6`) | OpenAI TTS (`tts-1`, `nova` voice) |

Provider selection affects `AI_PROVIDER_*` preprocessor constants — it is not a runtime setting.
API keys and **model names** can be set at runtime via the web config UI without recompiling.

### 5.2 Pipeline Steps

```
[Recording] → [WAV build] → [AI call] → [Emotion parse] → [TTS + lip-sync]
```

1. **WAV build** — 44-byte RIFF/WAVE header prepended to raw PCM in PSRAM.
2. **AI call** (`aiChat()`) — HTTPS POST to provider endpoint. Includes:
   - System prompt (personality + response format instructions)
   - Last 4 conversation pairs as history context
   - Current audio (Gemini) or transcribed text (OpenAI/Claude)
3. **Response parsing** — AI must reply in:
   ```
   USER_SAID: <what the user said>
   [EMOTION] Response text
   ```
   Valid emotion tags: `HAPPY`, `SAD`, `ANGRY`, `SLEEPY`, `SURPRISED`, `NEUTRAL`.
4. **Emotion application** — avatar expression set, servo animation triggered.
5. **TTS** — response text spoken aloud (§6).
6. **History update** — transcript + response appended to conversation history.

### 5.3 Error Handling

- PSRAM allocation failure → `showErrorUI("WAV alloc failed")`
- AI HTTP error or empty response → `showErrorUI(error_message)`
- TTS HTTP non-200 → SAM offline fallback (§6.2)
- WiFi lost → reconnect attempt; AP fallback if it fails

---

## 6. Text-to-Speech

### 6.1 Online TTS

**Gemini:** `gemini-2.5-flash-preview-tts`, voice configurable at runtime (default: `Fenrir`).
Response is base64-encoded PCM inside a JSON envelope, streamed and decoded on-device.
May return a WAV container (RIFF header detected and skipped automatically) or raw PCM.

**OpenAI TTS:** `tts-1`, `nova` voice (configurable at compile time). Raw 24 kHz PCM streamed
directly as binary.

Both paths feed decoded PCM into `playWithSubtitles()`.

### 6.2 SAM Offline Fallback

When the online TTS HTTP response is not 200 (quota, rate-limit, network error), or when the
Gemini TTS model returns text instead of audio (wrong model configured):

1. `samSpeak()` generates audio locally using `ESP8266SAM`.
2. Audio is captured into PSRAM via a `SamBufOut` shim, bypassing ESP8266Audio's I2S layer.
3. Output (~22050 Hz) is 2× linearly upsampled to ~44100 Hz to reduce aliasing harshness.
4. Fed into `playWithSubtitles()` — same word-by-word subtitle reveal and lip-sync as cloud TTS.
5. If SAM generates 0 samples (edge case): avatar stops, `showReaderUI()` displays the text
   on screen for reading, avatar restarts.

The boot greeting always uses SAM directly, regardless of provider (no cloud TTS call at boot).

SAM tuning: Speed 64, Pitch 64, Throat 110, Mouth 160.

**Gemini TTS detection:** the stream parser watches simultaneously for `"data":` (audio) and
`"text":` (non-audio response). If a chat model is mistakenly used as the TTS model, the
`"text":` marker is found in < 1 s and SAM fallback activates immediately, with the model's
actual response text logged to serial.

### 6.3 Lip-sync

During all TTS playback (online and SAM), the avatar mouth is driven in real time:

- Every 50 ms, peak PCM amplitude over a ±25 ms window is mapped to `[0, 1]` (3× boosted).
- `avatar.setMouthOpenRatio(ratio)` is called — thread-safe, no display lock needed.
- Words appear progressively in the avatar's speech bubble (`avatar.setSpeechText()`),
  timed to estimated word duration.
- Avatar **never stops** during normal TTS — the face continues animating and blinking.

---

## 7. Configuration System

### 7.1 Compiled Defaults

`src/ai_config.h` — all sensitive values. Edit before building. Acts as factory defaults.

### 7.2 Runtime Config

`data/config.json` — flashed once with `pio run -t uploadfs`. After first save via the web UI,
the file on LittleFS is updated in-place (uploadfs is no longer needed).

Loaded at every boot before hardware init. Globals in `config_store.h`:

| Global | Plain/Encrypted | Editable via web |
|--------|-----------------|------------------|
| `g_wifi_ssid` | Plain | Yes |
| `g_wifi_pass` | Encrypted | Yes |
| `g_gemini_api_key` | Encrypted | Yes |
| `g_gemini_tts_voice` | Plain | Yes |
| `g_openai_api_key` | Encrypted | Yes |
| `g_anthropic_api_key` | Encrypted | Yes |
| `g_greeting` | Plain | Yes |
| `g_chat_model` | Plain | Yes — LLM model name |
| `g_gemini_tts_model` | Plain | Yes — must be a dedicated TTS model |
| `g_face_style` | Plain | Yes — `"default"` / `"cat"` / `"capybara"` |
| `g_ai_provider` | Plain | Read-only (display only) |

### 7.3 Encryption

AES-128-CTR, device-bound:
- Key = HMAC-SHA256(salt=`"stackchan-cfg-v1"`, msg=WiFi station MAC) → first 16 bytes
- 16-byte nonce from hardware RNG per field per save
- Stored as: `<ENC>` + base64(nonce ∥ ciphertext)

The key is tied to the device's WiFi MAC — a filesystem image from one device cannot be
decrypted on another.

### 7.4 Web Config UI

Served at `http://<device-ip>` (STA mode) or `http://192.168.4.1` (AP mode).

- Single-page dark-themed HTML form, no JavaScript framework.
- Cards: **WiFi** · **API Keys** · **AI Models** · **Personality** · **Firmware Info**
- **AI Models** card: editable chat model and Gemini TTS model fields (no recompile needed).
- **Personality** card: greeting text + **Face Style** dropdown (`Default` / `Cat` / `Capybara`).
- Sensitive fields show masked hints (first 6 chars + `...`); leave blank to keep current value.
- On submit: saves config, sends "Restarting..." page, reboots after 1.5 s.
- AP fallback SSID: `stackchan-config` (no password).
- Web server is non-blocking (`handleClient()` called every `loop()` iteration).

---

## 8. Avatar & Expressions

### 8.1 Expressions

| Expression | Trigger |
|------------|---------|
| Neutral | Default; after touch ends; after AI response |
| Happy | BtnA; top touch; AI returns HAPPY; wake word confirmed |
| Sad | Mid-centre touch; AI returns SAD |
| Doubt | Mid-left / mid-right touch; AI returns SURPRISED |
| Angry | AI returns ANGRY |
| Sleepy | Bottom touch; BtnC short-tap; AI returns SLEEPY |

### 8.2 Idle Animations

When not touched, every 3 seconds:
- Servo pan/tilt offset by ±15°/±5° randomly.
- Avatar rotated ±10° (`avatar.setRotation()`).
- Random blink interval: 2–5 seconds.

### 8.3 Theme

True-black background (`TFT_BLACK`) applied via `setThemeDefault()` in
`stackchan_expression.h`. This improves perceived contrast on the IPS display and eliminates
the default grey avatar background.

### 8.4 Face Styles

Three selectable faces, configured via `g_face_style` in the web UI (Personality card).
Applied at boot before `avatar.init()` — takes effect after save + restart.

| Style | Description |
|-------|-------------|
| `default` | Standard M5Stack-Avatar ellipse face |
| `cat` | Kawaii cat: wide soft ears + pink inner, large round eyes with slit pupil, double specular highlight, gaze tracking, `^` blink, blush ovals, tiny heart-shaped nose, short stylised whiskers, gentle `∪` smile (flips to `∩` on Sad/Angry) |
| `capybara` | Kawaii capybara: small round ears, naturally squinted eyes (capped at 65% open for permanent chill look), oversized blush, soft oval snout with dark button nose and nostrils, wide content `∪` smile |

Face components live in `src/avatar_faces.h`. All extend `Drawable` (the library's abstract
base). Custom `CatFace` and `CapybaraFace` subclass `Face` using the 10-arg constructor
with explicit `BoundingRect` positions — the same pattern as the library's own `DogFace`.
Each eye component draws its own ear (determined by `getCenterX() < 160`), since the
`Face` API has no cheek/decoration slot. Gaze tracking is applied to all pupils.

---

## 9. Servo System

### 9.1 Mode Selection

Runs once at first boot (or hold BtnA at boot). The selected mode is saved to NVS and loaded
on all subsequent boots.

| Mode | Hardware | Characteristics |
|------|----------|-----------------|
| PWM | SG90-class on GPIO32 (tilt) / GPIO33 (pan) | Software smoothing 2°/frame, ~50 Hz |
| TTL SCS | Feetech SCS0009 on UART2 (GPIO13 TX, GPIO14 RX) @ 1 Mbps | Hardware closed-loop, silent, precise |

### 9.2 Limits

Pan: 45–135°, Tilt: 70–110°. Center: 90°/90°.

### 9.3 Animations

| Animation | Description |
|-----------|-------------|
| `nod()` | Quick tilt down + return to center |
| `shake()` | Pan left–right–left |
| `lookLeft()` / `lookRight()` | Move to pan limit |
| `tiltDown()` | Move to tilt max |
| `center()` | Return both axes to 90° |
| `moveRelative()` | Offset from current position (used by idle animation) |

### 9.4 Servo Test

BtnC long-hold (≥ 1.5 s) runs a calibration sequence: center → pan left → pan right → center
→ tilt min → tilt max → done. Each position holds 1.5 s. Labels shown in speech bubble.

---

## 10. Network & Connectivity

### 10.1 WiFi

- STA mode: credentials from runtime config.
- 15-second connection timeout at boot.
- On success: DTIM sleep disabled, auto-reconnect enabled.
- On failure: AP mode (`stackchan-config`, no password, IP `192.168.4.1`).
- Config web server starts on whichever network is active.

### 10.2 HTTPS

All AI and TTS API calls use `WiFiClientSecure` with `setInsecure()` (no certificate pinning).
Timeout: 45 s for AI calls, 25 s for TTS, 20 s for OpenAI TTS.

### 10.3 API Endpoints

| Provider | Endpoint |
|----------|----------|
| Gemini chat | `generativelanguage.googleapis.com/v1beta/models/{model}:generateContent` |
| Gemini TTS | `generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent` |
| Gemini transcribe (wake word) | Same as Gemini chat — minimal prompt, no history |
| OpenAI STT | `api.openai.com/v1/audio/transcriptions` (Whisper-1, multipart/form-data) |
| OpenAI chat | `api.openai.com/v1/chat/completions` |
| OpenAI TTS | `api.openai.com/v1/audio/speech` |
| Anthropic | `api.anthropic.com/v1/messages` |

---

## 11. Conversation History

- Last 4 user+assistant pairs kept in `s_history[]` in SRAM (~1.2 KB).
- FIFO eviction: oldest pair dropped when full.
- Included in every AI request as context.
- BtnC short-tap: clear all history (`convClear()`).
- History is not persisted across reboots.

---

## 12. Audio System

### 12.1 Recording

- Sample rate: 16 kHz, 16-bit mono, PSRAM buffer.
- Max duration: 8 s (configurable via `RECORD_MAX_SECONDS`).
- DMA task pinned to Core 0, priority 2.
- Non-blocking reads, paced at 1024 samples per 64 ms chunk.
- Noise filter level: 2 (light gate, 0=off, 1–9).
- Two recording modes:
  - `recordWhilePressed()` — stops when BtnB released or 8 s elapsed.
  - `recordTimedSeconds(secs)` — fixed duration, used by wake word command capture.

### 12.2 Speaker

- Sample rate: 48 kHz (speaker DMA config).
- DMA: 16 buffers × 1024 samples = ~341 ms headroom.
- Task pinned to Core 1 (in M5Unified speaker config), priority 2.
- Volume: 100/255 (below clipping on Core2's small amplifier).
- Must be fully re-initialised after every mic session (see CLAUDE.md §2).

### 12.3 Startup Tone

Short synthesised tone sequence played at the end of `setup()` via `audioController.playStartup()`.

---

## 13. Open Issues / Known Limitations

| # | Issue | Status |
|---|-------|--------|
| 1 | Wake word VAD threshold needs calibration (ambient RMS ~14k vs. threshold 600) | In progress — disabled |
| 2 | Provider selection requires recompile (not runtime-switchable) | By design for now |
| 3 | Conversation history not persisted across reboots | Accepted — SRAM only |
| 4 | No certificate pinning on HTTPS (setInsecure) | Accepted — ESP32 heap constraint |
| 5 | `uploadfs` overwrites saved config (credentials lost) | Known; document clearly |
| 6 | Gemini TTS preview model may change or be gated | Monitor; fallback to SAM works. Model name is now runtime-editable via web UI |

---

## 14. Planned / Future Features

| Feature | Notes |
|---------|-------|
| Wake word re-enable | After VAD calibration in a real deployment environment |
| Wake word multi-language | Add configurable keyword list |
| Display brightness auto-adjust | Use ambient light sensor on Core2 if available |
| Volume control via web UI | Expose speaker volume as a runtime config field |
| OTA firmware update | Via web UI — requires HTTPS OTA partition |
| Persistent conversation history | Save last N turns to LittleFS on shutdown |
| Emotion → LED | M5Stack Core2 has RGB LED (AXP192 controlled) — could pulse colour |
| Runtime provider switching | Would require `#if` to become runtime dispatch with dynamic function pointers |
| **Local LLM via M5Stack K144 (AX630C)** | **Hardware ordered.** AX630C module connects via UART and runs models on-device. New `AI_PROVIDER_LOCAL` alongside existing cloud providers. Provider switching will need to become runtime-selectable. Cloud providers remain as fallback / alternative. |
| Multi-user voice profiles | Not feasible on current hardware without speaker ID model |
