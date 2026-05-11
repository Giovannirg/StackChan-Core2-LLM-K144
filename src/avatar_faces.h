#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// avatar_faces.h  —  Kawaii Cat and Capybara faces for M5Stack-Avatar
//
// Also defines SubtitleOverlay — a Mouth wrapper that draws word-wrapped
// subtitle text in the bottom panel of the sprite (y=168–238).
// Reads g_subtitle_text from conversation_ui.h each frame.
// The default Balloon is suppressed by never calling avatar.setSpeechText(non_empty).
//
// Selectable via g_face_style ("default" | "cat" | "capybara").
// Applied in startAvatar() before avatar.init().
//
// Library API facts (confirmed from source):
//   Extend Drawable — Eye/Mouth/Eyeblow are final.
//   Face(mouth, mouthPos, eyeR, eyeRPos, eyeL, eyeLPos, eblowR, eblowRPos, eblowL, eblowLPos)
//   BoundingRect(top, left [, width, height]) → getCenterX()=left+w/2, getCenterY()=top+h/2
//   getEyeOpenRatio()  — single blink value [0,1]
//   getGaze()          → Gaze::getHorizontal() / getVertical()  [-1,1]
//   Eyeblow(width, height, isLeft)
//   ERACER_COLOR — background erase colour for 1-bit mode
//
// Reference positions (BoundingRect top, left):
//   mouth   168, 131, 64, 40   →  cx=163  cy=188
//   eyeR    103,  80            →  cx= 80  cy=103   (avatar right = screen left)
//   eyeL    106, 240            →  cx=240  cy=106   (avatar left  = screen right)
//   eblowR   67,  96
//   eblowL   72, 230
// ─────────────────────────────────────────────────────────────────────────────

#include <Avatar.h>
#include "conversation_ui.h"
using namespace m5avatar;

// ── Colour helpers ─────────────────────────────────────────────────────────
static const uint16_t FACE_PINK  = 0xFBD6;  // blush / inner-ear rose (RGB 248,122,176)
static const uint16_t FACE_PUPIL = 0x2104;  // near-black pupil

#define _FG(ctx,cp)  ((ctx)->getColorDepth()==1 ? (uint16_t)1           : (cp)->get(COLOR_PRIMARY))
#define _BG(ctx)     ((ctx)->getColorDepth()==1 ? (uint16_t)ERACER_COLOR : (ctx)->getColorPalette()->get(COLOR_BACKGROUND))
#define _DK(ctx,cp)  ((ctx)->getColorDepth()==1 ? (uint16_t)ERACER_COLOR : (cp)->get(COLOR_SECONDARY))
#define _PUP(ctx)    ((ctx)->getColorDepth()==1 ? (uint16_t)ERACER_COLOR : FACE_PUPIL)

// ════════════════════════════════════════════════════════════════════════════
//  CAT FACE — kawaii emoji style
//
//  CatEye   draws:  soft triangle ear  +  pink blush  +  large round eye
//                   with big pupil, double highlight, gaze tracking
//                   blink → cute  ^ ^ shape
//  CatMouth draws:  short stylised whiskers (2/side)  +  tiny pink nose
//                   +  gentle ∪ smile (flips to ∩ on Sad/Angry)
// ════════════════════════════════════════════════════════════════════════════

class CatEye : public Drawable {
public:
    void draw(M5Canvas* spi, BoundingRect rect, DrawContext* ctx) override {
        ColorPalette* cp = ctx->getColorPalette();
        if (!cp) return;
        uint16_t fg  = _FG(ctx, cp);
        uint16_t bg  = _BG(ctx);
        float    eor = ctx->getEyeOpenRatio();
        Gaze     g   = ctx->getGaze();

        int cx = rect.getCenterX();
        int cy = rect.getCenterY();
        int gx = (int)(g.getHorizontal() * 6);
        int gy = (int)(g.getVertical()   * 4);
        bool leftScreen = (cx < 160);   // avatar's right eye is on screen-left side

        // ── Soft triangle ear ─────────────────────────────────────────────────
        //    Wider + rounder proportions than realistic cat ears.
        int ety = cy - 66;                         // tip y
        int eby = cy - 22;                         // base y
        spi->fillTriangle(cx, ety, cx-30, eby, cx+30, eby, fg);
        // Large pink inner ear (prominent for kawaii look)
        spi->fillTriangle(cx, ety+20, cx-18, eby-5, cx+18, eby-5, FACE_PINK);

        // ── Blush: soft pink oval below-outer the eye ─────────────────────────
        int bx = leftScreen ? cx - 22 : cx + 22;
        spi->fillEllipse(bx, cy + 22, 18, 10, FACE_PINK);

        // ── Eye ────────────────────────────────────────────────────────────────
        if (eor < 0.05f) {
            // Blink → kawaii "^" closed-eye shape
            spi->drawLine(cx - 18, cy + 5, cx,     cy - 8, fg);
            spi->drawLine(cx,      cy - 8, cx + 18, cy + 5, fg);
            return;
        }

        int ew = 20;
        int eh = max(1, (int)(20 * eor));

        // White / primary colour eye base
        spi->fillEllipse(cx, cy, ew, eh, fg);

        // Large round pupil with gaze offset
        int pw = max(2, (int)(ew * 0.65f));
        int ph = max(1, (int)(eh * 0.80f));
        spi->fillEllipse(cx + gx, cy + gy, pw, ph, _PUP(ctx));

        // Main specular highlight (big, top-left of pupil)
        spi->fillCircle(cx + gx - 6, cy + gy - 6, 5, bg);
        // Secondary small sparkle
        spi->fillCircle(cx + gx + 6, cy + gy + 4, 2, bg);
    }
};

class CatMouth : public Drawable {
public:
    void draw(M5Canvas* spi, BoundingRect rect, DrawContext* ctx) override {
        ColorPalette* cp = ctx->getColorPalette();
        if (!cp) return;
        uint16_t fg = _FG(ctx, cp);
        float    op = ctx->getMouthOpenRatio();
        Expression ex = ctx->getExpression();

        int cx = rect.getCenterX();   // ≈ 163
        int cy = rect.getCenterY();   // ≈ 188

        // ── Short kawaii whiskers — stay inside the face ────────────────────
        // Two per side, slight fan
        spi->drawLine(cx - 14, cy -  8, cx - 48, cy - 13, fg);
        spi->drawLine(cx - 14, cy -  2, cx - 48, cy -  2, fg);
        spi->drawLine(cx + 14, cy -  8, cx + 48, cy - 13, fg);
        spi->drawLine(cx + 14, cy -  2, cx + 48, cy -  2, fg);

        // ── Tiny pink heart-shaped nose (two circles + triangle) ────────────
        spi->fillCircle(cx - 3, cy - 11, 4, FACE_PINK);
        spi->fillCircle(cx + 3, cy - 11, 4, FACE_PINK);
        spi->fillTriangle(cx, cy - 5, cx - 6, cy - 10, cx + 6, cy - 10, FACE_PINK);

        // ── Gentle ∪ / ∩ mouth with 4 line segments ────────────────────────
        int depth = (int)(8 * max(0.2f, op));
        bool frown = (ex == Expression::Sad || ex == Expression::Angry);

        if (!frown) {
            // Smile ∪
            spi->drawLine(cx - 12, cy,     cx -  6, cy + depth,     fg);
            spi->drawLine(cx -  6, cy + depth, cx,   cy + depth + 2, fg);
            spi->drawLine(cx,    cy + depth + 2, cx + 6, cy + depth, fg);
            spi->drawLine(cx +  6, cy + depth, cx + 12, cy,          fg);
        } else {
            // Frown ∩
            spi->drawLine(cx - 12, cy + depth + 2, cx -  6, cy + 2, fg);
            spi->drawLine(cx -  6, cy + 2,         cx,      cy,     fg);
            spi->drawLine(cx,      cy,             cx +  6, cy + 2, fg);
            spi->drawLine(cx +  6, cy + 2,         cx + 12, cy + depth + 2, fg);
        }
    }
};

class CatFace : public Face {
public:
    CatFace() : Face(
        new CatMouth(),            new BoundingRect(168, 131, 64, 40),
        new CatEye(),              new BoundingRect(103,  80),
        new CatEye(),              new BoundingRect(106, 240),
        new Eyeblow(15, 2, false), new BoundingRect( 67,  96),
        new Eyeblow(15, 2, true),  new BoundingRect( 72, 230)
    ) {}
};

// ════════════════════════════════════════════════════════════════════════════
//  CAPYBARA FACE — kawaii / chill emoji style
//
//  Signature look: small naturally-squinting eyes, HUGE blush, button nose,
//  content wide smile — the universally beloved "chilling capybara" energy.
//
//  CapyEye   draws:  small round ear  +  large blush ellipse
//                    +  naturally squinted oval eye with gaze & highlights
//                    blink → gentle straight line
//  CapyMouth draws:  soft oval snout  +  dark button-nose  +  wide ∪ smile
// ════════════════════════════════════════════════════════════════════════════

class CapyEye : public Drawable {
public:
    void draw(M5Canvas* spi, BoundingRect rect, DrawContext* ctx) override {
        ColorPalette* cp = ctx->getColorPalette();
        if (!cp) return;
        uint16_t fg  = _FG(ctx, cp);
        uint16_t bg  = _BG(ctx);
        float    eor = ctx->getEyeOpenRatio();
        Gaze     g   = ctx->getGaze();

        int cx = rect.getCenterX();
        int cy = rect.getCenterY();
        int gx = (int)(g.getHorizontal() * 4);
        int gy = (int)(g.getVertical()   * 3);
        bool leftScreen = (cx < 160);

        // ── Small round ear offset toward the outside ─────────────────────
        int ex = leftScreen ? cx - 10 : cx + 10;
        int ey = cy - 44;
        spi->fillCircle(ex, ey, 21, fg);
        spi->fillCircle(ex, ey, 12, FACE_PINK);

        // ── Signature huge blush — very prominent for capybara chill vibe ──
        int bx = leftScreen ? cx - 24 : cx + 24;
        spi->fillEllipse(bx, cy + 22, 24, 13, FACE_PINK);

        // ── Naturally squinted eye (height capped at 60 % for sleepy look) ─
        if (eor < 0.05f) {
            spi->drawLine(cx - 14, cy, cx + 14, cy, fg);
            return;
        }
        int ew = 16;
        // Cap openness at 0.65 so eyes always look pleasantly squinted
        int eh = max(1, (int)(ew * min(eor, 0.65f)));

        spi->fillEllipse(cx, cy, ew, eh, fg);

        // Round pupil with gaze
        int pw = max(2, (int)(ew * 0.55f));
        int ph = max(1, min(pw, eh - 1));
        spi->fillEllipse(cx + gx, cy + gy, pw, ph, _PUP(ctx));

        // Double specular highlight (large + tiny)
        spi->fillCircle(cx + gx - 5, cy + gy - 4, 4, bg);
        spi->fillCircle(cx + gx + 5, cy + gy + 3, 2, bg);
    }
};

class CapyMouth : public Drawable {
public:
    void draw(M5Canvas* spi, BoundingRect rect, DrawContext* ctx) override {
        ColorPalette* cp = ctx->getColorPalette();
        if (!cp) return;
        uint16_t fg   = _FG(ctx, cp);
        uint16_t dark = _DK(ctx, cp);
        float    op   = ctx->getMouthOpenRatio();

        int cx = rect.getCenterX();   // ≈ 163
        int cy = rect.getCenterY();   // ≈ 168

        // ── Soft oval snout (two overlapping ellipses for rounder appearance)
        spi->fillEllipse(cx, cy + 4, 44, 24, fg);
        spi->fillEllipse(cx, cy - 4, 38, 18, fg);

        // ── Button nose: rounded dark oval on top of snout ─────────────────
        spi->fillEllipse(cx, cy - 16, 16, 9, dark);
        // Two tiny nostrils cut into the nose
        spi->fillEllipse(cx - 6, cy - 16, 4, 3, fg);
        spi->fillEllipse(cx + 6, cy - 16, 4, 3, fg);

        // ── Wide content ∪ smile — the hallmark capybara expression ─────────
        int smileD = (int)(10 * max(0.2f, op));
        spi->drawLine(cx - 20, cy + 8,          cx - 12, cy + 8 + smileD, dark);
        spi->drawLine(cx - 12, cy + 8 + smileD, cx,      cy + 10 + smileD, dark);
        spi->drawLine(cx,      cy + 10 + smileD, cx + 12, cy + 8 + smileD, dark);
        spi->drawLine(cx + 12, cy + 8 + smileD, cx + 20, cy + 8,           dark);
    }
};

class CapybaraFace : public Face {
public:
    CapybaraFace() : Face(
        new CapyMouth(),           new BoundingRect(148, 131, 64, 40),
        new CapyEye(),             new BoundingRect( 95,  80),
        new CapyEye(),             new BoundingRect( 98, 240),
        new Eyeblow(15, 2, false), new BoundingRect( 67,  96),
        new Eyeblow(15, 2, true),  new BoundingRect( 72, 230)
    ) {}
};

// ─── SubtitleOverlay — word-wrapped text panel in the bottom of the sprite ───
// Wrap the existing Face Mouth with this class in setup() after startAvatar():
//   avatar.getFace()->setMouth(new SubtitleOverlay(avatar.getFace()->getMouth()));
//
// Layout (all coordinates in sprite space, 320×240):
//   Separator:  y = 162 (thin horizontal line)
//   Text area:  y = 166 – 238, full width
//   Font:       Font2 (~16 px), up to 3 lines of word-wrapped text
//   Colours:    taken from the avatar's ColorPalette (matches true-black theme)
//
// With avatar.setScale(0.65) the face elements land at display y ≈ 88–148.
// The separator (sprite y=162) appears at display y ≈ 127, giving a clean gap.
// Text (sprite y=166–238) appears at display y ≈ 130–167 — below the face.
class SubtitleOverlay : public Drawable {
    Drawable* _inner;
public:
    explicit SubtitleOverlay(Drawable* inner) : _inner(inner) {}
    ~SubtitleOverlay() override { delete _inner; }

    void draw(M5Canvas* spi, BoundingRect rect, DrawContext* ctx) override {
        // Draw the wrapped mouth as normal
        _inner->draw(spi, rect, ctx);

        // Text panel
        ColorPalette* cp = ctx->getColorPalette();
        uint16_t fg = cp->get(COLOR_PRIMARY);
        uint16_t bg = cp->get(COLOR_BACKGROUND);

        // Text area: below the mouth (sprite y=193, mouth bottom ~188).
        // Font4 at 0.85× = ~22px. Centered horizontally. 2 lines.
        const int TEXT_Y    = 193;
        const int LINE_H    = 24;
        const int MAX_LINES = 2;
        const int CHARS_PER_LINE = 34;   // Font4*0.85 ~9px/char avg; centered at x=160

        // Clear text area to background
        spi->fillRect(0, TEXT_Y, 320, 240 - TEXT_Y, bg);

        if (g_subtitle_text.isEmpty()) return;

        spi->setFont(&fonts::Font4);
        spi->setTextColor(fg, bg);
        spi->setTextSize(0.85f);
        spi->setTextDatum(TC_DATUM);

        // Character-count word-wrap — no textWidth() calls, safe on every frame.
        String remaining = g_subtitle_text;
        remaining.trim();
        int line = 0;

        while (!remaining.isEmpty() && line < MAX_LINES) {
            String cur = "";
            while (!remaining.isEmpty()) {
                int sp = remaining.indexOf(' ');
                String word = (sp >= 0) ? remaining.substring(0, sp) : remaining;
                int needed = cur.isEmpty() ? (int)word.length()
                                           : (int)cur.length() + 1 + (int)word.length();
                if (!cur.isEmpty() && needed > CHARS_PER_LINE) break;
                cur = cur.isEmpty() ? word : cur + " " + word;
                remaining = (sp >= 0) ? remaining.substring(sp + 1) : "";
                remaining.trim();
            }
            if (!cur.isEmpty()) {
                spi->drawString(cur.c_str(), 160, TEXT_Y + line * LINE_H);
                line++;
            }
        }
    }
};
