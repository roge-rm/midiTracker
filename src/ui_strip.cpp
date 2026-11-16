#include "ui_strip.h"
#include "synth.h"

namespace {

// A sprite drawn straight into a fixed buffer instead of one from the heap,
// so its 10 KB shows in the static RAM figure and can never fail to
// allocate. attach() sets up what createSprite() would, for any w x h that
// fits. The sprite is never deleted, so its destructor never frees the
// buffer.
class StripCanvas : public TFT_eSprite {
public:
    explicit StripCanvas(TFT_eSPI* tft) : TFT_eSprite(tft) {}

    void attach(uint16_t* buf, int w, int h) {
        _iwidth = _dwidth = _bitwidth = w;
        _iheight = _dheight = h;
        cursor_x = 0;
        cursor_y = 0;
        _sx = 0;
        _sy = 0;
        _sw = w;
        _sh = h;
        _scolor = TFT_BLACK;
        _img8 = (uint8_t*)buf;
        _img8_1 = _img8;
        _img8_2 = _img8;
        _img = buf;
        _img4 = _img8;
        _created = true;
        rotation = 0;
        setViewport(0, 0, w, h);
        setPivot(w / 2, h / 2);
    }
};

// Word aligned for the DMA's 16-bit reads.
uint16_t g_stripBuf[Ui::STRIP_PIXELS] __attribute__((aligned(4)));

TFT_eSPI* g_tft = nullptr;
StripCanvas* g_canvas = nullptr;
bool g_dma = false;
bool g_open = false;
int g_x = 0, g_y = 0, g_w = 0, g_h = 0;

void (*g_pump)() = nullptr;
int g_drawPass = 0;
bool g_inPump = false;

// The draw clip, x0 y0 inclusive, x1 y1 exclusive.
int g_clipX0 = 0, g_clipY0 = 0, g_clipX1 = 320, g_clipY1 = 240;

uint32_t g_sliceSpentUs = 0;
bool g_forceMeters = false;

} // namespace

namespace Ui {

void stripInit(TFT_eSPI& tft) {
    g_tft = &tft;
    static StripCanvas canvas(&tft);
    g_canvas = &canvas;
    g_canvas->attach(g_stripBuf, STRIP_W, STRIP_H);
    // Without a free DMA channel stripSend() falls back to a blocking push.
    g_dma = tft.initDMA();
}

TFT_eSprite& stripBegin(int x, int y, int w, int h, uint16_t bg) {
    if (w < 1) w = 1;
    if (w > STRIP_PIXELS) w = STRIP_PIXELS;
    int maxRows = STRIP_PIXELS / w;
    if (h < 1) h = 1;
    if (h > maxRows) h = maxRows;
    g_x = x;
    g_y = y;
    g_w = w;
    g_h = h;
    g_canvas->attach(g_stripBuf, w, h);
    g_canvas->fillSprite(bg);
    g_canvas->setTextFont(2);
    g_canvas->setTextSize(1);
    g_canvas->setTextDatum(TL_DATUM);
    g_open = true;
    return *g_canvas;
}

void stripSend() {
    if (!g_open) return;
    g_open = false;
    uint32_t t0 = micros();
    if (!g_dma) {
        g_canvas->pushSprite(g_x, g_y);
        sliceCharge(micros() - t0);
        return;
    }
    // The sprite holds its pixels byte swapped; with swap bytes off,
    // pushImageDMA() has the DMA swap them back on the way out, without a
    // copy. The transaction keeps CS low until the last pixel has gone.
    bool oldSwap = g_tft->getSwapBytes();
    g_tft->setSwapBytes(false);
    g_tft->startWrite();
    g_tft->pushImageDMA(g_x, g_y, g_w, g_h, g_stripBuf);
    uint32_t pumpUs = 0;
    while (g_tft->dmaBusy()) {
        if (g_pump && !g_inPump) {
            uint32_t p0 = micros();
            g_inPump = true;
            g_pump();
            g_inPump = false;
            pumpUs += micros() - p0;
        }
    }
    g_tft->endWrite();
    g_tft->setSwapBytes(oldSwap);
    // Only the drawing counts against the slice, not the pump's work.
    sliceCharge(micros() - t0 - pumpUs);
}

void setDrawPump(void (*pump)()) { g_pump = pump; }

int drawPass() { return g_drawPass; }
void setDrawPass(int pass) { g_drawPass = pass; }
bool hasDrawPump() { return g_pump != nullptr; }

void pumpOnce() {
    if (!g_pump || g_inPump) return;
    g_inPump = true;
    g_pump();
    g_inPump = false;
}

void setDrawClip(int x, int y, int w, int h) {
    g_clipX0 = x;
    g_clipY0 = y;
    g_clipX1 = x + w;
    g_clipY1 = y + h;
    // The tft viewport clips direct draws and the strip's DMA push.
    g_tft->setViewport(x, y, w, h, false);
}

void clearDrawClip() {
    g_clipX0 = 0;
    g_clipY0 = 0;
    g_clipX1 = g_tft->width();
    g_clipY1 = g_tft->height();
    g_tft->resetViewport();
}

bool clipRect(int& x, int& y, int& w, int& h) {
    int x1 = x + w, y1 = y + h;
    if (x < g_clipX0) x = g_clipX0;
    if (y < g_clipY0) y = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    w = x1 - x;
    h = y1 - y;
    return w > 0 && h > 0;
}

void sliceBeginLoop() { g_sliceSpentUs = 0; }

void sliceCharge(uint32_t us) { g_sliceSpentUs += us; }

void sliceForceMeters(bool on) { g_forceMeters = on; }

bool ringLow() {
    if (!Synth::wavStreamIsActive() || Synth::wavStreamIsEnded()) return false;
    size_t cap = Synth::wavStreamCapacity();
    size_t buffered = cap - Synth::wavStreamFree();
    return buffered < cap / 4;
}

bool sliceAllows(DrawKind kind) {
    if (kind == DRAW_ESSENTIAL || g_forceMeters) return true;
    if (ringLow()) return false;
    return g_sliceSpentUs < UI_SLICE_US;
}

} // namespace Ui
