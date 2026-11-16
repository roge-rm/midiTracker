#pragma once
// A framebuffer stand-in for TFT_eSPI with the calls ui_look.cpp makes.
// Every draw becomes a clipped fillRect into one shared 320x240 RGB565
// buffer. A sprite is a window onto that buffer at its screen position.
#include <stdint.h>

#define TFT_BLACK 0x0000
#define TL_DATUM 0

extern uint16_t g_hostFrame[240][320];

class TFT_eSPI {
public:
    TFT_eSPI() {}
    virtual ~TFT_eSPI() {}
    int16_t width() const { return 320; }
    int16_t height() const { return 240; }

    // The window this object draws into, in screen pixels.
    int ox = 0, oy = 0, ow = 320, oh = 240;
    // Viewport, as TFT_eSPI's setViewport(x, y, w, h, true).
    int dx = 0, dy = 0, vx = 0, vy = 0, vw = 320, vh = 240;

    void setViewport(int x, int y, int w, int h, bool datum = true) {
        dx = datum ? x : 0;
        dy = datum ? y : 0;
        vx = x < 0 ? 0 : x;
        vy = y < 0 ? 0 : y;
        vw = (x + w > ow ? ow : x + w) - vx;
        vh = (y + h > oh ? oh : y + h) - vy;
    }
    void fillRect(int x, int y, int w, int h, uint16_t c) {
        x += dx;
        y += dy;
        int x0 = x < vx ? vx : x, y0 = y < vy ? vy : y;
        int x1 = x + w > vx + vw ? vx + vw : x + w;
        int y1 = y + h > vy + vh ? vy + vh : y + h;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
                int sx = ox + xx, sy = oy + yy;
                if (sx >= 0 && sx < 320 && sy >= 0 && sy < 240) g_hostFrame[sy][sx] = c;
            }
    }
    void drawFastHLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
    void drawFastVLine(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }
    void drawPixel(int x, int y, uint16_t c) { fillRect(x, y, 1, 1, c); }
    void drawRect(int x, int y, int w, int h, uint16_t c) {
        fillRect(x, y, w, 1, c);
        fillRect(x, y + h - 1, w, 1, c);
        fillRect(x, y, 1, h, c);
        fillRect(x + w - 1, y, 1, h, c);
    }
    void fillScreen(uint16_t c) { fillRect(-dx, -dy, 320, 240, c); }
    void startWrite() {}
    void endWrite() {}
};

class TFT_eSprite : public TFT_eSPI {
public:
    explicit TFT_eSprite(TFT_eSPI*) {}
};
