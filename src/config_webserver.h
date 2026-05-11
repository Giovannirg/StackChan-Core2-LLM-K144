#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Stack-chan Configuration Web Server
//
// Accessible at:
//   http://<WiFi-IP>      — when connected to your network
//   http://192.168.4.1    — when in AP fallback mode ("stackchan-config")
//
// Call startConfigServer() after WiFi/AP is up.
// Call handleConfigServer() every loop iteration.
//
// Sensitive fields are displayed masked (first 6 chars + "...").
// Submitted values are AES-128-CTR encrypted before saving to LittleFS.
// ═══════════════════════════════════════════════════════════════════════════════

#include <WebServer.h>
#include "config_store.h"
#include "ai_config.h"

static WebServer s_http(80);

// ─── HTML template ────────────────────────────────────────────────────────────
// %PLACEHOLDERS% are replaced at serve time with current values.

static const char CONFIG_HTML[] = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stack-chan Config</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:15px/1.5 system-ui,sans-serif;background:#0d0d0d;color:#e0e0e0;padding:16px}
h1{font-size:1.4em;color:#5bf;margin-bottom:4px}
.sub{color:#666;font-size:.85em;margin-bottom:20px}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;padding:16px;margin-bottom:14px}
.card h2{font-size:.95em;color:#5bf;margin-bottom:12px;text-transform:uppercase;letter-spacing:.05em}
label{display:block;font-size:.8em;color:#888;margin-bottom:3px;margin-top:10px}
label:first-of-type{margin-top:0}
input,select{width:100%;padding:9px 11px;border-radius:6px;border:1px solid #333;background:#111;color:#e0e0e0;font-size:.95em}
input:focus,select:focus{outline:2px solid #5bf;border-color:#5bf}
.hint{font-size:.75em;color:#555;margin-top:3px}
.info-row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #222;font-size:.85em}
.info-row:last-child{border-bottom:none}
.info-val{color:#aaa;font-family:monospace;font-size:.8em}
.badge{display:inline-block;padding:2px 8px;border-radius:10px;background:#0d2a0d;color:#4c4;border:1px solid #2a4a2a;font-size:.75em}
.warn{color:#fa0;font-size:.75em;margin-top:4px}
button{width:100%;padding:13px;background:#5bf;color:#000;font-weight:700;font-size:1em;border:none;border-radius:8px;cursor:pointer;margin-top:6px;letter-spacing:.03em}
button:hover{background:#7df}
.footer{text-align:center;color:#333;font-size:.75em;margin-top:20px}
</style>
</head>
<body>
<h1>&#129302; Stack-chan</h1>
<p class="sub">Configuration &nbsp;<span class="badge">%PROVIDER%</span></p>

<div class="card">
  <h2>&#127758; WiFi</h2>
  <form method="POST" action="/save" id="frm">
    <label>Network SSID</label>
    <input name="ssid" value="%SSID%" autocomplete="off" spellcheck="false">
    <label>Password</label>
    <input name="pass" type="password" placeholder="%PASS_HINT%" autocomplete="new-password">
    <p class="hint">Leave blank to keep current password</p>
</div>

<div class="card">
  <h2>&#128273; API Keys</h2>
  <label>Gemini API Key</label>
  <input name="gemini_key" type="password" placeholder="%GEMINI_HINT%" autocomplete="off">
  <label>Gemini TTS Voice</label>
  <input name="gemini_voice" value="%GEMINI_VOICE%">
  <p class="hint">Aoede &middot; Charon &middot; Fenrir &middot; Kore &middot; Puck &middot; Zephyr (+ 24 more)</p>
  <label>OpenAI API Key</label>
  <input name="openai_key" type="password" placeholder="%OPENAI_HINT%" autocomplete="off">
  <label>OpenAI / Claude TTS Voice</label>
  <select name="openai_voice">
    <option value="alloy"   %SEL_OV_ALLOY%  >Alloy (neutral)</option>
    <option value="echo"    %SEL_OV_ECHO%   >Echo (clear, male)</option>
    <option value="fable"   %SEL_OV_FABLE%  >Fable (expressive)</option>
    <option value="onyx"    %SEL_OV_ONYX%   >Onyx (deep, male)</option>
    <option value="nova"    %SEL_OV_NOVA%   >Nova (warm, female)</option>
    <option value="shimmer" %SEL_OV_SHIMMER%>Shimmer (soft, female)</option>
  </select>
  <label>Anthropic API Key</label>
  <input name="anthropic_key" type="password" placeholder="%ANTHROPIC_HINT%" autocomplete="off">
  <label>ElevenLabs API Key</label>
  <input name="elevenlabs_key" type="password" placeholder="%ELEVENLABS_HINT%" autocomplete="off">
  <label>ElevenLabs Voice ID</label>
  <input name="elevenlabs_voice_id" value="%ELEVENLABS_VOICE_ID%" autocomplete="off" spellcheck="false">
  <p class="hint">Voice ID from elevenlabs.io (e.g. 21m00Tcm4TlvDq8ikWAM = Rachel). Only used when TTS Provider is set to ElevenLabs.</p>
  <label>TTS Provider</label>
  <select name="tts_provider">
    <option value="auto"        %SEL_TTS_AUTO%>Auto (use AI provider&rsquo;s default TTS)</option>
    <option value="elevenlabs"  %SEL_TTS_EL%  >ElevenLabs (overrides all providers)</option>
  </select>
  <p class="hint">Auto: Gemini provider uses Gemini TTS, OpenAI/Claude/Realtime use OpenAI TTS. ElevenLabs: always uses ElevenLabs regardless of AI provider.</p>
  <p class="hint">Leave a key field blank to keep the current value</p>
</div>

<div class="card">
  <h2>&#129302; AI Provider</h2>
  <label>Provider</label>
  <select name="ai_provider">
    <option value="gemini"   %SEL_GEMINI%  >&#127760; Gemini (STT + LLM + TTS via Google)</option>
    <option value="openai"   %SEL_OPENAI%  >&#127760; OpenAI (Whisper STT &middot; GPT LLM &middot; OpenAI TTS)</option>
    <option value="claude"   %SEL_CLAUDE%  >&#127760; Claude (Whisper STT &middot; Claude LLM &middot; OpenAI TTS)</option>
    <option value="realtime" %SEL_REALTIME%>&#9889; OpenAI Realtime (WSS bidirectional audio &mdash; &lt;2 s latency)</option>
    <option value="local"    %SEL_LOCAL%   >&#128187; Local LLM (K144 mic &middot; Whisper ASR &middot; local LLM &middot; Gemini/SAM TTS)</option>
  </select>
  <p class="hint">Takes effect after Save &amp; Restart. Realtime requires OpenAI API key. Local LLM requires M5Stack K144 module on Port C.</p>
  <p class="warn">Local LLM conflicts with TTL SCS servo &mdash; device will switch to PWM servo automatically.</p>
  <label>Wake Word (Local LLM only)</label>
  <input name="kws_word" value="%KWS_WORD%" autocomplete="off" spellcheck="false">
  <p class="hint">Word to say to activate the K144 microphone. Must match a word supported by the installed KWS model (default: HELLO).</p>
</div>

<div class="card">
  <h2>&#129302; AI Models</h2>
  <label>Chat model (cloud providers)</label>
  <input name="chat_model" value="%CHAT_MODEL%">
  <p class="hint">e.g. gemini-2.0-flash &middot; gemini-2.5-flash-preview &middot; gpt-4o-mini &middot; claude-sonnet-4-6</p>
  <label>Gemini TTS model</label>
  <input name="gemini_tts_model" value="%TTS_MODEL%">
  <p class="hint">Must be a dedicated TTS model: gemini-2.5-flash-preview-tts (chat models return no audio)</p>
  <label>Local LLM model (K144 module)</label>
  <select name="local_llm_model">
    <option value="qwen3-0.6B-ax630c"           %SEL_LLM_QWEN3%  >Qwen3 0.6B &mdash; recommended: best instruction following, multilingual</option>
    <option value="qwen2.5-1.5B-ax630c"          %SEL_LLM_Q15%   >Qwen2.5 1.5B p128 &mdash; current default (needs tokenizer service)</option>
    <option value="qwen2.5-1.5B-p256-ax630c"     %SEL_LLM_Q15P%  >Qwen2.5 1.5B p256 &mdash; longer context (needs tokenizer service)</option>
    <option value="deepseek-r1-1.5b-ax630c"      %SEL_LLM_DS%    >DeepSeek R1 1.5B &mdash; better reasoning &amp; math</option>
    <option value="openbuddy-llama3.2-1b-ax630c" %SEL_LLM_OB%    >OpenBuddy LLaMA 1B &mdash; multilingual fine-tune (EN/DE/ES/FR)</option>
    <option value="qwen2.5-0.5B-prefill-20e"     %SEL_LLM_Q05%   >Qwen2.5 0.5B &mdash; fastest, pre-installed, no tokenizer service</option>
  </select>
  <p class="hint">Install on K144 via ADB: <code>apt install --allow-unauthenticated llm-model-&lt;name&gt;</code>. Models marked "needs tokenizer service" require the qwen-tokenizer systemd service (see CLAUDE.md pitfall #22).</p>
  <label>Local ASR model (K144 module)</label>
  <select name="local_asr_model">
    <option value="sherpa-ncnn"      %SEL_ASR_SHERPA%>Sherpa-NCNN &mdash; English, streaming (confirmed working)</option>
    <option value="sherpa-bilingual" %SEL_ASR_SHBIL% >Sherpa-ONNX bilingual &mdash; Chinese + English streaming</option>
    <option value="sense-voice"      %SEL_ASR_SENSE% >SenseVoice &mdash; multilingual: ZH/EN/Cantonese/JA/KO (10 s chunks)</option>
    <option value="whisper-tiny"     %SEL_ASR_WTINY% >Whisper Tiny &mdash; EN/ZH/JA, fastest (needs llm-whisper pkg)</option>
    <option value="whisper-small"    %SEL_ASR_WSMALL%>Whisper Small &mdash; better accuracy (llm-whisper v1.8 does not support)</option>
  </select>
  <p class="hint">Sherpa-NCNN: confirmed working. Others require packages installed on K144: <code>apt install --allow-unauthenticated llm-model-sense-voice-small-10s llm-model-sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16 llm-whisper llm-vad llm-model-silero-vad</code></p>
  <label>Local TTS model (K144 module)</label>
  <select name="local_tts_model">
    <option value="melotts-en-default" %SEL_TTS_EN%>MeloTTS English (en-default) &mdash; confirmed working</option>
    <option value="melotts-en-us"      %SEL_TTS_ENUS%>MeloTTS English US (en-us)</option>
    <option value="melotts-es-es"      %SEL_TTS_ES%>MeloTTS Spanish (es-es)</option>
    <option value="single-speaker-english-fast" %SEL_TTS_LEGACY%>Legacy TTS &mdash; single-speaker-english-fast</option>
  </select>
  <p class="hint">MeloTTS plays on the K144 speaker. Falls back to SAM (Core2) if setup fails (e.g. heavy LLM exhausts memory). Set K144 volume to 50&ndash;70 for audible output.</p>
  <label>Max response tokens (K144 LLM)</label>
  <input type="number" name="local_max_tokens" value="%LOCAL_MAX_TOKENS%" min="60" max="300">
  <p class="hint">How many tokens the LLM may generate. 80 = ~1-2 short sentences (fastest). 120 = ~2-3 sentences. 256 = longer answers but may hit the 30 s timeout. Qwen3 needs 200+ to fit thinking + answer.</p>
</div>

<div class="card">
  <h2>&#128172; Personality</h2>
  <label>Boot Greeting</label>
  <input name="greeting" value="%GREETING%">
  <label>Face Style</label>
  <select name="face_style">
    <option value="default" %SEL_DEFAULT%>&#129302; Default</option>
    <option value="cat" %SEL_CAT%>&#128049; Cat</option>
    <option value="capybara" %SEL_CAPY%>&#128448; Capybara</option>
  </select>
  <p class="hint">Takes effect after Save &amp; Restart</p>
</div>

<div class="card">
  <h2>&#127774; Display &amp; Audio</h2>
  <label>Screen Brightness (0&ndash;255)</label>
  <input name="brightness" type="number" min="0" max="255" value="%BRIGHTNESS%">
  <p class="hint">100 = dim &middot; 150 = comfortable &middot; 200 = bright &middot; 255 = max</p>
  <label>Speaker Volume (0&ndash;255)</label>
  <input name="volume" type="number" min="0" max="255" value="%VOLUME%">
  <p class="hint">80 = comfortable &middot; 150 = loud &middot; 255 = max (may distort)</p>
  <label>K144 Module Speaker Volume (0&ndash;100 %)</label>
  <input name="k144_volume" type="number" min="0" max="100" value="%K144_VOLUME%">
  <p class="hint">K144 module&rsquo;s own speaker (wake-word beep). Default 0 = silent &mdash; prevents brownout when KWS fires.</p>
</div>

<div class="card">
  <h2>&#128295; Servo Calibration</h2>
  <p class="hint">Angles in degrees. Pan 0&deg; = far left, 180&deg; = far right. Tilt 0&deg; = up, 180&deg; = down. Takes effect after Save &amp; Restart. Use BtnC long-hold for servo test.</p>
  <label>Pan Min (left stop)</label>
  <input type="number" name="servo_pan_min" value="%SERVO_PAN_MIN%" min="0" max="180">
  <label>Pan Max (right stop)</label>
  <input type="number" name="servo_pan_max" value="%SERVO_PAN_MAX%" min="0" max="180">
  <label>Tilt Min (look up)</label>
  <input type="number" name="servo_tilt_min" value="%SERVO_TILT_MIN%" min="0" max="180">
  <label>Tilt Max (look down)</label>
  <input type="number" name="servo_tilt_max" value="%SERVO_TILT_MAX%" min="0" max="180">
</div>

<div class="card">
  <h2>&#128268; Firmware Info</h2>
  <div class="info-row"><span>Free heap</span><span class="info-val">%FREE_HEAP% KB</span></div>
  <div class="info-row"><span>Built</span><span class="info-val">%BUILD_DATE%</span></div>
</div>

  <button type="submit" form="frm">&#128190; Save &amp; Restart</button>
</form>

<p class="footer">Changes are encrypted and stored on device flash.</p>
</body>
</html>)rawhtml";

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Show only first 6 chars of key + "..." — enough to confirm which key is set
static String maskKey(const String& k) {
    if (k.isEmpty() || k.startsWith("YOUR_")) return "(not set)";
    if (k.length() <= 6) return "***";
    return k.substring(0, 6) + "...";
}

// Escape a string for use in an HTML attribute value (value="...")
static String htmlAttr(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '"')  out += "&quot;";
        else if (c == '&')  out += "&amp;";
        else if (c == '<')  out += "&lt;";
        else if (c == '\'') out += "&#39;";
        else if (c == '\n' || c == '\r') out += ' ';  // newlines break HTML attributes
        else                out += c;
    }
    return out;
}

static String buildPage() {
    String html(CONFIG_HTML);

    html.replace("%PROVIDER%",     g_ai_provider);
    html.replace("%SEL_GEMINI%",    g_ai_provider == "gemini"   ? "selected" : "");
    html.replace("%SEL_OPENAI%",    g_ai_provider == "openai"   ? "selected" : "");
    html.replace("%SEL_CLAUDE%",    g_ai_provider == "claude"   ? "selected" : "");
    html.replace("%SEL_REALTIME%",  g_ai_provider == "realtime" ? "selected" : "");
    html.replace("%SEL_LOCAL%",     g_ai_provider == "local"    ? "selected" : "");
    html.replace("%KWS_WORD%",     htmlAttr(g_kws_word));
    html.replace("%SSID%",         htmlAttr(g_wifi_ssid));
    html.replace("%PASS_HINT%",    g_wifi_pass.length() ? "(set — leave blank to keep)" : "(not set)");
    html.replace("%GEMINI_HINT%",  maskKey(g_gemini_api_key));
    html.replace("%GEMINI_VOICE%", htmlAttr(g_gemini_tts_voice));
    html.replace("%OPENAI_HINT%",  maskKey(g_openai_api_key));
    html.replace("%SEL_OV_ALLOY%",   g_openai_tts_voice == "alloy"   ? "selected" : "");
    html.replace("%SEL_OV_ECHO%",    g_openai_tts_voice == "echo"    ? "selected" : "");
    html.replace("%SEL_OV_FABLE%",   g_openai_tts_voice == "fable"   ? "selected" : "");
    html.replace("%SEL_OV_ONYX%",    g_openai_tts_voice == "onyx"    ? "selected" : "");
    html.replace("%SEL_OV_NOVA%",    g_openai_tts_voice == "nova"    ? "selected" : "");
    html.replace("%SEL_OV_SHIMMER%", g_openai_tts_voice == "shimmer" ? "selected" : "");
    html.replace("%ANTHROPIC_HINT%",  maskKey(g_anthropic_api_key));
    html.replace("%ELEVENLABS_HINT%", maskKey(g_elevenlabs_api_key));
    html.replace("%ELEVENLABS_VOICE_ID%", htmlAttr(g_elevenlabs_voice_id));
    html.replace("%SEL_TTS_AUTO%",    g_tts_provider == "auto"        ? "selected" : "");
    html.replace("%SEL_TTS_EL%",      g_tts_provider == "elevenlabs"  ? "selected" : "");
    html.replace("%GREETING%",     htmlAttr(g_greeting));
    html.replace("%SEL_DEFAULT%",  g_face_style == "default"  ? "selected" : "");
    html.replace("%SEL_CAT%",      g_face_style == "cat"      ? "selected" : "");
    html.replace("%SEL_CAPY%",     g_face_style == "capybara" ? "selected" : "");
    html.replace("%BRIGHTNESS%",   String(g_brightness));
    html.replace("%VOLUME%",       String(g_volume));
    html.replace("%K144_VOLUME%",  String(g_k144_volume));
    html.replace("%CHAT_MODEL%",   htmlAttr(g_chat_model));
    html.replace("%TTS_MODEL%",    htmlAttr(g_gemini_tts_model));
    html.replace("%SEL_LLM_QWEN3%", g_local_llm_model == "qwen3-0.6B-ax630c"           ? "selected" : "");
    html.replace("%SEL_LLM_Q15%",   g_local_llm_model == "qwen2.5-1.5B-ax630c"          ? "selected" : "");
    html.replace("%SEL_LLM_Q15P%",  g_local_llm_model == "qwen2.5-1.5B-p256-ax630c"     ? "selected" : "");
    html.replace("%SEL_LLM_DS%",    g_local_llm_model == "deepseek-r1-1.5b-ax630c"      ? "selected" : "");
    html.replace("%SEL_LLM_OB%",    g_local_llm_model == "openbuddy-llama3.2-1b-ax630c" ? "selected" : "");
    html.replace("%SEL_LLM_Q05%",   g_local_llm_model == "qwen2.5-0.5B-prefill-20e"     ? "selected" : "");
    html.replace("%SEL_ASR_SHERPA%", g_local_asr_model == "sherpa-ncnn"      ? "selected" : "");
    html.replace("%SEL_ASR_SHBIL%",  g_local_asr_model == "sherpa-bilingual" ? "selected" : "");
    html.replace("%SEL_ASR_SENSE%",  g_local_asr_model == "sense-voice"      ? "selected" : "");
    html.replace("%SEL_ASR_WTINY%",  g_local_asr_model == "whisper-tiny"     ? "selected" : "");
    html.replace("%SEL_ASR_WSMALL%", g_local_asr_model == "whisper-small"    ? "selected" : "");
    html.replace("%SEL_TTS_EN%",     g_local_tts_model == "melotts-en-default"          ? "selected" : "");
    html.replace("%SEL_TTS_ENUS%",   g_local_tts_model == "melotts-en-us"               ? "selected" : "");
    html.replace("%SEL_TTS_ES%",     g_local_tts_model == "melotts-es-es"               ? "selected" : "");
    html.replace("%SEL_TTS_LEGACY%", g_local_tts_model == "single-speaker-english-fast" ? "selected" : "");
    html.replace("%LOCAL_MAX_TOKENS%", String(g_local_max_tokens));
    html.replace("%SERVO_PAN_MIN%",  String(g_servo_pan_min));
    html.replace("%SERVO_PAN_MAX%",  String(g_servo_pan_max));
    html.replace("%SERVO_TILT_MIN%", String(g_servo_tilt_min));
    html.replace("%SERVO_TILT_MAX%", String(g_servo_tilt_max));
    html.replace("%FREE_HEAP%",    String(ESP.getFreeHeap() / 1024));
    html.replace("%BUILD_DATE%",   __DATE__ " " __TIME__);

    return html;
}

// ─── Route handlers ───────────────────────────────────────────────────────────

static void cfgHandleRoot() {
    s_http.send(200, "text/html; charset=utf-8", buildPage());
}

static void cfgHandleSave() {
    bool changed = false;

    // Update a global only when the form field is present and non-empty
    auto upd = [&](const char* arg, String& dest) {
        if (s_http.hasArg(arg) && s_http.arg(arg).length()) {
            dest   = s_http.arg(arg);
            changed = true;
        }
    };

    upd("ssid",            g_wifi_ssid);
    upd("pass",            g_wifi_pass);
    upd("ai_provider",     g_ai_provider);
    upd("gemini_key",      g_gemini_api_key);
    upd("gemini_voice",    g_gemini_tts_voice);
    upd("openai_key",      g_openai_api_key);
    upd("openai_voice",    g_openai_tts_voice);
    upd("kws_word",        g_kws_word);
    upd("anthropic_key",      g_anthropic_api_key);
    upd("elevenlabs_key",     g_elevenlabs_api_key);
    upd("elevenlabs_voice_id",g_elevenlabs_voice_id);
    upd("tts_provider",       g_tts_provider);
    upd("greeting",        g_greeting);
    if (s_http.hasArg("brightness") && s_http.arg("brightness").length()) {
        g_brightness = (uint8_t)constrain(s_http.arg("brightness").toInt(), 0, 255);
        M5.Display.setBrightness(g_brightness);
        changed = true;
    }
    if (s_http.hasArg("volume") && s_http.arg("volume").length()) {
        g_volume = (uint8_t)constrain(s_http.arg("volume").toInt(), 0, 255);
        M5.Speaker.setVolume(g_volume);
        changed = true;
    }
    if (s_http.hasArg("k144_volume") && s_http.arg("k144_volume").length()) {
        g_k144_volume = (uint8_t)constrain(s_http.arg("k144_volume").toInt(), 0, 100);
        changed = true;
    }
    upd("face_style",      g_face_style);
    upd("chat_model",       g_chat_model);
    upd("gemini_tts_model", g_gemini_tts_model);
    upd("local_llm_model",  g_local_llm_model);
    upd("local_asr_model",  g_local_asr_model);
    upd("local_tts_model",  g_local_tts_model);
    if (s_http.hasArg("local_max_tokens")) {
        int v = s_http.arg("local_max_tokens").toInt();
        g_local_max_tokens = (uint16_t)constrain(v, 60, 300);
    }
    if (s_http.hasArg("servo_pan_min")  && s_http.arg("servo_pan_min").length())  g_servo_pan_min  = (int8_t)constrain(s_http.arg("servo_pan_min").toInt(),  0, 180);
    if (s_http.hasArg("servo_pan_max")  && s_http.arg("servo_pan_max").length())  g_servo_pan_max  = (int8_t)constrain(s_http.arg("servo_pan_max").toInt(),  0, 180);
    if (s_http.hasArg("servo_tilt_min") && s_http.arg("servo_tilt_min").length()) g_servo_tilt_min = (int8_t)constrain(s_http.arg("servo_tilt_min").toInt(), 0, 180);
    if (s_http.hasArg("servo_tilt_max") && s_http.arg("servo_tilt_max").length()) g_servo_tilt_max = (int8_t)constrain(s_http.arg("servo_tilt_max").toInt(), 0, 180);

    if (changed) {
        saveConfig();
        s_http.send(200, "text/html; charset=utf-8",
            F("<!DOCTYPE html><html><head>"
              "<meta charset='UTF-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Saved</title>"
              "<style>body{font:16px system-ui,sans-serif;background:#0d0d0d;color:#e0e0e0;"
              "display:flex;flex-direction:column;align-items:center;justify-content:center;"
              "min-height:100vh;gap:12px;text-align:center}"
              "h2{color:#5f5;font-size:1.6em} p{color:#888}"
              "</style></head><body>"
              "<h2>&#10003; Config Saved</h2>"
              "<p>Restarting Stack-chan&hellip;</p>"
              "<script>setTimeout(()=>location.href='/',4000)</script>"
              "</body></html>"));
        delay(1500);
        ESP.restart();
    } else {
        // Nothing changed — redirect back
        s_http.sendHeader("Location", "/");
        s_http.send(302);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

inline void startConfigServer() {
    s_http.on("/",     HTTP_GET,  cfgHandleRoot);
    s_http.on("/save", HTTP_POST, cfgHandleSave);
    s_http.begin();
    Serial.printf("[WebCfg] Config UI at http://%s\n",
                  WiFi.getMode() == WIFI_AP
                      ? WiFi.softAPIP().toString().c_str()
                      : WiFi.localIP().toString().c_str());
}

inline void handleConfigServer() {
    s_http.handleClient();
}
