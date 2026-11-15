// corr <ours.raw> <ref.raw>: compares two stereo int16 44.1 kHz renders.
// Prints the lag (frames) that lines them up (found on the left channel, so
// surround's out-of-phase parts don't cancel), the waveform correlation of
// the left and right channels and the mono mix at that lag, the correlation of the 23 ms loudness envelope,
// of the left/right balance envelope, and the level ratio.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstdlib>

static std::vector<float> loadMono(const char* path, std::vector<float>* side,
                                   std::vector<float>* left, std::vector<float>* right) {
    std::vector<float> m;
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return m; }
    int16_t fr[2];
    while (fread(fr, 2, 2, f) == 2) {
        m.push_back((fr[0] + fr[1]) * 0.5f);
        side->push_back((fr[0] - fr[1]) * 0.5f);
        left->push_back(fr[0]);
        right->push_back(fr[1]);
    }
    fclose(f);
    return m;
}

static double corr(const float* a, const float* b, size_t n) {
    double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; i++) { sa += a[i]; sb += b[i]; sab += (double)a[i] * b[i]; saa += (double)a[i] * a[i]; sbb += (double)b[i] * b[i]; }
    double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    return (va > 0 && vb > 0) ? cov / sqrt(va * vb) : 0;
}

static std::vector<float> envelope(const std::vector<float>& x, size_t from, size_t n, size_t win) {
    std::vector<float> e;
    for (size_t i = from; i + win <= from + n; i += win) {
        double s = 0;
        for (size_t k = 0; k < win; k++) s += (double)x[i + k] * x[i + k];
        e.push_back((float)sqrt(s / win));
    }
    return e;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: corr <ours.raw> <ref.raw>\n"); return 2; }
    std::vector<float> sa, sb, la, lb, ra_, rb_;
    std::vector<float> a = loadMono(argv[1], &sa, &la, &ra_), b = loadMono(argv[2], &sb, &lb, &rb_);
    size_t n = a.size() < b.size() ? a.size() : b.size();
    if (n < 44100 * 4) { fprintf(stderr, "too short\n"); return 1; }
    // Lag search on a 4 s stretch from 2 s in, ours shifted by -2048..2048.
    const int MAXLAG = 2048;
    size_t s0 = getenv("CORR_AT") ? (size_t)(atof(getenv("CORR_AT")) * 44100) : 88200, len = 44100 * 4;
    if (getenv("CORR_LEN")) len = (size_t)(atof(getenv("CORR_LEN")) * 44100);
    int best = 0; double bestR = -2;
    for (int lag = -MAXLAG; lag <= MAXLAG; lag++) {
        double r = corr(&la[s0 + lag], &lb[s0], len);
        if (r > bestR) { bestR = r; best = lag; }
    }
    size_t from = MAXLAG, cnt = n - 2 * MAXLAG;
    double wave = corr(&a[from + best], &b[from], cnt);
    double waveL = corr(&la[from + best], &lb[from], cnt);
    double waveR = corr(&ra_[from + best], &rb_[from], cnt);
    std::vector<float> ea = envelope(a, from + best, cnt, 1024), eb = envelope(b, from, cnt, 1024);
    double env = corr(ea.data(), eb.data(), ea.size());
    std::vector<float> pa = envelope(sa, from + best, cnt, 1024), pb = envelope(sb, from, cnt, 1024);
    double side = corr(pa.data(), pb.data(), pa.size());
    double ra = 0, rb = 0, qa = 0, qb = 0;
    for (size_t i = 0; i < ea.size(); i++) { ra += ea[i]; rb += eb[i]; qa += pa[i]; qb += pb[i]; }
    if (getenv("CORR_SECONDS")) {
        // One digit a second: waveform correlation x10 at the global lag.
        fprintf(stderr, "per second: ");
        for (size_t t = from; t + 44100 <= from + cnt; t += 44100) {
            double r = corr(&a[t + best], &b[t], 44100);
            fprintf(stderr, "%c", r < 0 ? '-' : (char)('0' + (int)(r * 9.99)));
        }
        fprintf(stderr, "\n");
    }
    // width: side level over mid level, ours then the reference.
    printf("lag %5d  L %.3f  R %.3f  wave %.3f  env %.3f  side %.3f  level %.2f  width %.2f %.2f\n", best, waveL, waveR, wave, env, side, rb > 0 ? ra / rb : 0,
           ra > 0 ? qa / ra : 0, rb > 0 ? qb / rb : 0);
    return 0;
}
