# Stack-chan Core2 — FreeRTOS Architecture

This document describes the real-time operating system layout of the Stack-chan firmware:
which tasks run on which core, how they communicate, what synchronization mechanisms are in
place, and what the known architectural trade-offs are.

---

## Dual-core layout

```
┌──────────────────────────────────────────────────────────┐
│  ESP32 — 240 MHz, dual Xtensa LX6                        │
│                                                          │
│  Core 0                   Core 1                         │
│  ─────────────────────    ─────────────────────────────  │
│  WiFi stack  (pri 23)     Avatar task        (pri  8)    │
│  Speaker DMA (pri  2)     Arduino loop()     (pri  1)    │
│  Mic DMA     (pri  2)                                    │
└──────────────────────────────────────────────────────────┘
```

**Design principle:** all DMA and radio work is isolated on Core 0. Core 1 is reserved for
the avatar rendering task and the main `loop()`. This prevents DMA interrupts from stalling
the display pipeline, and prevents display work from adding latency to audio DMA.

---

## Task inventory

| Task | Core | Priority | Stack | Created by |
|------|------|----------|-------|-----------|
| WiFi stack | 0 | 23 | (IDF internal) | `WiFi.begin()` |
| Speaker DMA | 0 | 2 | (M5Unified internal) | `M5.Speaker.begin()` |
| Mic DMA | 0 | 2 | (M5Unified internal) | `M5.Mic.begin()` |
| Avatar render | 1 | 8 | (M5Stack-Avatar internal) | `avatar.init(8)` |
| Arduino loop() | 1 | 1 | **16 KB** (raised from default 8 KB) | Arduino framework |

**Face selection** (`CatFace`, `CapybaraFace`) is a one-time heap allocation in `startAvatar()`
before `avatar.init()`. It adds no new tasks or synchronization primitives — the face object
lives for the device lifetime and is accessed only by the avatar render task.

### Why the loop stack is 16 KB

The default Arduino loop stack is 8 KB. The HTTP → JSON → TTS call chain
(HTTPS connect → streaming decode → base64 → PCM → playRaw) is deep enough to overflow it.
Set in `platformio.ini`:

```ini
-UCONFIG_ARDUINO_LOOP_STACK_SIZE      ; undefine sdkconfig.h's 8192 first
-DCONFIG_ARDUINO_LOOP_STACK_SIZE=16384
```

---

## Shared resources and how they are protected

### 1. SPI display bus

The avatar task (pri 8) renders frames to the ILI9342C display continuously via the shared
SPI bus. If `loop()` writes to the display at the same time, SPI state corrupts and the
device crashes.

**Protection: soft-stop pattern**

```cpp
inline void stopAvatar() {
    if (avatarRunning) {
        avatar.stop();   // sets internal _isRunning = false
        delay(150);      // yields CPU; avatar task sees the flag, finishes frame, parks
        avatarRunning = false;
    }
}
```

`delay()` on ESP32-Arduino calls `yield()`, which gives the FreeRTOS scheduler a chance to
run the avatar task (pri 8 > loop pri 1). The avatar task checks `_isRunning` at the top of
each frame and exits if false. After 150 ms the SPI bus is guaranteed to be idle.

**`avatarRunning` is `volatile`** so the compiler cannot cache it in a register across the
yield point.

**Thread-safe avatar calls (no stop needed):**

| Call | Why it is safe |
|------|---------------|
| `avatar.setSpeechText(text)` | Consumed by avatar task internally; write is atomic |
| `avatar.setMouthOpenRatio(r)` | Same — consumed by avatar task |

These are used for "Listening...", "..." overlays, TTS subtitles, and lip-sync from `loop()`
without ever stopping the avatar.

### 2. I2S bus (speaker ↔ mic)

`M5.Mic.end()` reconfigures I2S and GPIO0, which is shared between the NS4168 speaker LRCK
and the SPM1423 mic CLK. After mic use, `M5.Speaker.begin()` is a no-op because the
`_begun` flag is still set.

**Protection: `speakerReinitAfterMic()` in `audio_recorder.h`**

```cpp
delay(80);              // IDF 4.4 releases I2S platform asynchronously
M5.Speaker.end();       // clears _begun flag
// ... configure and re-begin speaker on Core 0 ...
delay(300);             // I2S DMA stabilisation
M5.Speaker.setVolume(100);
```

Critical config: `task_pinned_core = 0` — speaker DMA must stay on Core 0 after re-init.
Setting this to Core 1 would put the DMA task in direct competition with `loop()` and the
avatar task.

### 3. Mic recording — non-blocking only

`M5.Mic.record(..., blocking=true)` uses a cross-task semaphore. Called from `loop()` on
Core 1 while I2S DMA runs on Core 0, this triggers a FreeRTOS assert at `queue.c:832`.

**Protection:** always `blocking=false` with manual 64 ms chunk pacing:

```cpp
M5.Mic.record(chunk, 1024, RECORD_SAMPLE_RATE, false);
delay(CHUNK_MS);   // 64 ms — let DMA fill exactly one chunk
```

---

## Inter-task communication

No explicit queues or semaphores are currently created by application code. Communication
happens through three mechanisms:

### A. Thread-safe avatar API

`setSpeechText()` and `setMouthOpenRatio()` cross from `loop()` to the avatar task safely
because the Avatar library serialises access internally.

### B. Static function pointer callbacks

`tts_client.h` has no direct reference to the avatar object. Two callbacks are registered
in `setup()`:

```cpp
setAvatarLifecycle(stopAvatar, startAvatar);   // for samSpeak() error path
setLipSyncAvatar(&avatar);                      // for playWithSubtitles() mouth drive
```

This keeps `tts_client.h` decoupled from `main.cpp` while still allowing it to stop the
avatar before direct display writes on the SAM fallback path.

### C. Global state polled in loop()

Button state (`M5.BtnB.isPressed()`), touch coordinates, and `avatarRunning` are plain
globals read and written only from `loop()` (same task, same core). No race conditions
exist for these because there is only one writer and one reader, always in the same
execution context.

---

## Blocking operations in loop()

`loop()` is single-threaded. During AI and TTS calls it blocks for seconds at a time:

| Operation | Typical duration | Blocks loop? |
|-----------|-----------------|-------------|
| `connectWiFi()` | up to 15 s at boot | Yes — once |
| `aiChat()` (STT + LLM) | 2–8 s | Yes |
| `geminiSpeak()` HTTP + stream | 3–12 s | Yes |
| `openaiSpeak()` HTTP + stream | 2–8 s | Yes |
| `playWithSubtitles()` | audio duration + ~1.2 s drain | Yes |
| `samSpeak()` SAM synthesis | < 1 s | Yes |

**During these windows the robot does not respond to button presses or touch.**
This is intentional for the conversational flow — the robot is mid-sentence.

The avatar task (pri 8) continues rendering the face uninterrupted throughout, because it
runs on the same core but at higher priority. Mouth animation and speech text are driven
via the thread-safe `setSpeechText()` / `setMouthOpenRatio()` calls from inside the
blocking TTS pipeline.

---

## Stack overflow detection

`configCHECK_FOR_STACK_OVERFLOW=2` is set in `platformio.ini`. Mode 2 checks the stack
canary on every context switch (cheap — a few cycles). If any task overflows, the hook in
`main.cpp` fires:

```cpp
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char* pcTaskName) {
    Serial.printf("\n[FATAL] Stack overflow in task: %s — rebooting\n", pcTaskName);
    Serial.flush();
    esp_restart();
}
```

The device reboots cleanly and prints the offending task name instead of hanging silently
with corrupted memory.

---

## PSRAM usage

All large audio buffers are allocated in PSRAM (`ps_malloc`) to keep SRAM available for
task stacks and the avatar frame buffers.

| Buffer | Size | Pool |
|--------|------|------|
| Recording (8 s @ 16 kHz) | 256 KB | PSRAM |
| WAV (recording + 44 B header) | ~256 KB | PSRAM |
| TTS PCM — Gemini / OpenAI | ~480 KB | PSRAM |
| SAM PCM pre-upsample (~22 kHz) | ~440 KB | PSRAM |
| SAM PCM post-upsample (×2) | ~880 KB | PSRAM |
| API JSON payload | 2–5 KB | PSRAM |
| Conversation history (4 pairs) | ~1.2 KB | SRAM |
| Avatar frame buffers | ~100 KB | SRAM |

Recording and TTS buffers are allocated per request and freed before the next one.
SAM briefly holds both the original and upsampled buffers simultaneously (~1.3 MB peak).

---

## Known architectural trade-offs

### AI pipeline blocks loop()

The HTTP round-trip and streaming TTS decode run synchronously in `loop()`. Moving them to
a dedicated Core 0 task with a result queue would allow `loop()` to stay responsive (servo
idle animations, touch, button feedback) during the 2–8 s AI call. This would be the single
highest-impact architectural improvement but requires redesigning `runAIPipeline()` as a
FreeRTOS task with `xQueueSend` / `xQueueReceive` handoff.

### SPI soft-stop vs. proper mutex

The `stopAvatar() + delay(150)` pattern is a timing assumption, not a lock. A binary
semaphore that the avatar task takes before each frame and gives after would be a true
guarantee. Implementing it requires patching the M5Stack-Avatar library to inject the
semaphore around its render loop, which is non-trivial.

The current approach is safe in practice because:
- Both tasks share Core 1 (no true parallelism — only time-slicing)
- The avatar checks `_isRunning` at the top of each frame loop
- 150 ms >> one frame period (~100 ms at ~10 FPS)

### No task watchdog feeds

Long delays (2–2.5 s in `playWithSubtitles()`, up to 15 s in `connectWiFi()`) could trip
a task watchdog if it is configured with a short period. The Arduino framework's `delay()`
calls `yield()` which feeds the idle task watchdog indirectly, but an explicit
`esp_task_wdt_reset()` call inside long loops would be more robust.

---

## Quick reference — adding new features

| Scenario | Pattern to follow |
|----------|------------------|
| New long-running HTTP call | Run in `loop()` synchronously — same pattern as `aiChat()` |
| New direct display write | `stopAvatar()` → write → `startAvatar()` |
| New speech/subtitle | `avatar.setSpeechText()` — safe from `loop()` without stop |
| New audio playback | `speakerReinitAfterMic()` if following mic use; use `playWithSubtitles()` |
| New large buffer | `ps_malloc()` — allocate in PSRAM, free when done |
| New background task | Pin to Core 0, priority ≤ 2; never touch display or call `M5.Speaker` directly |
