/**
 * Stack-chan Audio Controller
 * Handles sound generation for expressions and feedback
 * Uses M5Stack Core2's built-in speaker with tone generation
 */

#ifndef STACKCHAN_AUDIO_H
#define STACKCHAN_AUDIO_H

#include <Arduino.h>
#include <M5Unified.h>

class StackchanAudio {
public:
    StackchanAudio() : volume(128), isMuted(false) {}
    
    void begin() {
        // Larger DMA buffers prevent underruns when reading 24 kHz PCM from PSRAM.
        // 48 kHz internal rate gives M5Unified a clean 2:1 upsample from our 24 kHz TTS.
        auto spk_cfg = M5.Speaker.config();
        spk_cfg.sample_rate      = 48000;
        spk_cfg.dma_buf_len      = 1024;
        spk_cfg.dma_buf_count    = 16;  // 16×1024 @48kHz = ~340ms; absorbs cross-core IPC latency
        // Pin the speaker DMA-feed task to Core 1, away from the WiFi stack
        // (Core 0, priority 23).  WiFi at pri 23 preempts anything at pri 2 on
        // Core 0, starving the I2S DMA buffer and causing audio distortion.
        // On Core 1 the avatar is stopped during TTS playback, so the speaker
        // task (pri 2) only competes with loop() (pri 1) — which yields via delay(10).
        spk_cfg.task_pinned_core = 1;
        spk_cfg.task_priority    = 2;
        M5.Speaker.config(spk_cfg);
        M5.Speaker.begin();
        M5.Speaker.setVolume(volume);
        Serial.println("Audio initialized");
    }
    
    void setVolume(uint8_t vol) {
        volume = vol;
        M5.Speaker.setVolume(volume);
    }
    
    void mute(bool muted) {
        isMuted = muted;
        M5.Speaker.setVolume(muted ? 0 : volume);
    }
    
    void playStartup() {
        if (isMuted) return;
        
        // Cheerful startup melody
        playTone(523, 100);  // C5
        delay(50);
        playTone(659, 100);  // E5
        delay(50);
        playTone(784, 100);  // G5
        delay(50);
        playTone(1047, 200); // C6
    }
    
    void playHappy() {
        if (isMuted) return;
        
        // Happy chirp - ascending notes
        playTone(880, 80);   // A5
        delay(30);
        playTone(988, 80);   // B5
        delay(30);
        playTone(1175, 100); // D6
        delay(30);
        playTone(1319, 150); // E6
    }
    
    void playAngry() {
        if (isMuted) return;
        
        // Angry buzz - low harsh tones
        playTone(150, 100);
        delay(50);
        playTone(130, 100);
        delay(50);
        playTone(110, 150);
    }
    
    void playSad() {
        if (isMuted) return;
        
        // Sad melody - descending minor
        playTone(440, 150);  // A4
        delay(50);
        playTone(392, 150);  // G4
        delay(50);
        playTone(330, 200);  // E4
    }
    
    void playSurprise() {
        if (isMuted) return;
        
        // Surprised sound - quick ascending
        playTone(400, 50);
        delay(20);
        playTone(600, 50);
        delay(20);
        playTone(800, 50);
        delay(20);
        playTone(1200, 100);
    }
    
    void playSleepy() {
        if (isMuted) return;
        
        // Sleepy yawn - slow descending
        playTone(500, 200);
        delay(100);
        playTone(400, 200);
        delay(100);
        playTone(300, 300);
    }
    
    void playDoubt() {
        if (isMuted) return;
        
        // Questioning sound
        playTone(400, 100);
        delay(50);
        playTone(450, 100);
        delay(50);
        playTone(500, 150);
    }
    
    void playAck() {
        if (isMuted) return;
        
        // Simple acknowledgment beep
        playTone(800, 50);
        delay(30);
        playTone(1000, 80);
    }
    
    void playError() {
        if (isMuted) return;
        
        // Error buzzer
        playTone(200, 150);
        delay(100);
        playTone(200, 150);
    }
    
    // Play custom tone
    void playTone(uint16_t frequency, uint16_t duration) {
        if (isMuted) return;
        M5.Speaker.tone(frequency, duration);
        delay(duration);
        M5.Speaker.stop();
    }
    
    // Play custom melody
    void playMelody(const uint16_t* frequencies, const uint16_t* durations, int length) {
        if (isMuted) return;
        for (int i = 0; i < length; i++) {
            if (frequencies[i] > 0) {
                playTone(frequencies[i], durations[i]);
            } else {
                delay(durations[i]); // Rest
            }
            delay(20); // Brief pause between notes
        }
    }

private:
    uint8_t volume;
    bool isMuted;
};

#endif // STACKCHAN_AUDIO_H
