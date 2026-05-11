/**
 * Stack-chan Expression Controller
 * Manages facial expressions and animations
 */

#ifndef STACKCHAN_EXPRESSION_H
#define STACKCHAN_EXPRESSION_H

#include <Arduino.h>
#include <Avatar.h>

using namespace m5avatar;

class StackchanExpression {
public:
    StackchanExpression() 
        : avatar(nullptr)
        , currentExpression(Expression::Neutral)
        , expressionStartTime(0)
        , expressionDuration(0)
        , autoRevert(true)
    {}
    
    void begin(Avatar* avatarPtr) {
        avatar = avatarPtr;
        if (avatar) {
            avatar->setExpression(Expression::Neutral);
        }
    }
    
    void setExpression(Expression expr, uint32_t duration = 2000) {
        if (!avatar) return;
        
        currentExpression = expr;
        avatar->setExpression(expr);
        expressionStartTime = millis();
        expressionDuration = duration;
        
        // Set mouth and eye states based on expression
        switch (expr) {
            case Expression::Happy:
                avatar->setMouthOpenRatio(0.5f);
                break;
            case Expression::Angry:
                avatar->setMouthOpenRatio(0.3f);
                break;
            case Expression::Sad:
                avatar->setMouthOpenRatio(0.2f);
                break;
            case Expression::Sleepy:
                avatar->setMouthOpenRatio(0.1f);
                break;
            case Expression::Doubt:
                avatar->setMouthOpenRatio(0.0f);
                break;
            default:
                avatar->setMouthOpenRatio(0.0f);
                break;
        }
        
        Serial.printf("Expression set: %d\n", (int)expr);
    }
    
    void setAutoRevert(bool enable) {
        autoRevert = enable;
    }
    
    void update() {
        if (!avatar || !autoRevert) return;
        
        // Auto-revert to neutral after duration
        if (currentExpression != Expression::Neutral) {
            if (millis() - expressionStartTime > expressionDuration) {
                setExpression(Expression::Neutral);
            }
        }
    }
    
    // Set gaze direction (-1.0 to 1.0 for both X and Y)
    void setGaze(float x, float y) {
        if (!avatar) return;
        // Note: M5Stack-Avatar uses rotation for gaze simulation
        avatar->setRotation(x * 15.0f);
    }
    
    // Open/close mouth (0.0 to 1.0)
    void setMouthOpen(float ratio) {
        if (!avatar) return;
        avatar->setMouthOpenRatio(ratio);
    }
    
    // Speaking animation
    void speak(bool speaking) {
        if (!avatar) return;
        if (speaking) {
            avatar->setSpeechText("...");
        } else {
            avatar->setSpeechText("");
        }
    }
    
    // Set speech bubble text
    void setSpeech(const char* text) {
        if (!avatar) return;
        avatar->setSpeechText(text);
    }
    
    // Get current expression
    Expression getExpression() const {
        return currentExpression;
    }
    
    // Custom color palette
    void setColorPalette(uint16_t primary, uint16_t secondary, uint16_t background) {
        if (!avatar) return;
        ColorPalette palette;   // stack — no heap leak per theme switch
        palette.set(COLOR_PRIMARY,    primary);
        palette.set(COLOR_SECONDARY,  secondary);
        palette.set(COLOR_BACKGROUND, background);
        avatar->setColorPalette(palette);
    }
    
    // Preset color themes
    void setThemeDefault() {
        setColorPalette(TFT_WHITE, TFT_BLACK, TFT_BLACK);
    }
    
    void setThemeNight() {
        setColorPalette(0x4208, TFT_DARKGREY, TFT_BLACK);
    }
    
    void setThemeHappy() {
        setColorPalette(TFT_YELLOW, TFT_ORANGE, TFT_NAVY);
    }
    
    void setThemeSleepy() {
        setColorPalette(0x7BEF, TFT_DARKGREY, 0x0010);
    }

private:
    Avatar* avatar;
    Expression currentExpression;
    uint32_t expressionStartTime;
    uint32_t expressionDuration;
    bool autoRevert;
};

#endif // STACKCHAN_EXPRESSION_H
