// envcorr <ours.wav> <ref.wav>: compares two 16-bit stereo WAVs by loudness, since waveforms won't match.
// Prints both lengths, the RMS level ratio in dB (ours over ref), the correlation of the
// 10 ms RMS envelopes (at the best lag within +-250 ms) and the correlation of the onsets
// (rises in the envelope in dB), for the stretch both files cover.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static bool loadWav(const char* path, std::vector<float>& mono, int& rate) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    char id[4]; uint32_t len;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) { fclose(f); return false; }
    fseek(f, 12, SEEK_SET);
    int ch = 2; rate = 44100;
    while (fread(id, 1, 4, f) == 4 && fread(&len, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            uint16_t c; uint32_t r; fseek(f, 2, SEEK_CUR); fread(&c, 2, 1, f); fread(&r, 4, 1, f);
            ch = c; rate = (int)r; fseek(f, (long)len - 8, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            std::vector<int16_t> d(len / 2);
            size_t got = fread(d.data(), 2, d.size(), f);
            for (size_t i = 0; i + ch <= got; i += ch) {
                float s = 0; for (int k = 0; k < ch; k++) s += d[i + k];
                mono.push_back(s / ch);
            }
            fclose(f); return ch == 2 || ch == 1;
        } else fseek(f, (long)len + (len & 1), SEEK_CUR);
    }
    fclose(f); return false;
}

static double corr(const float* a, const float* b, size_t n) {
    double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; i++) { sa += a[i]; sb += b[i]; sab += (double)a[i] * b[i]; saa += (double)a[i] * a[i]; sbb += (double)b[i] * b[i]; }
    double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    return (va > 0 && vb > 0) ? cov / sqrt(va * vb) : 0;
}

static std::vector<float> env(const std::vector<float>& x, size_t win) {
    std::vector<float> e;
    for (size_t i = 0; i + win <= x.size(); i += win) {
        double s = 0; for (size_t k = 0; k < win; k++) s += (double)x[i + k] * x[i + k];
        e.push_back((float)sqrt(s / win));
    }
    return e;
}

static std::vector<float> onsets(const std::vector<float>& e) {
    std::vector<float> o(e.size(), 0.f);
    for (size_t i = 1; i < e.size(); i++) {
        float d = 20 * log10f((e[i] + 1.f) / (e[i - 1] + 1.f));
        o[i] = d > 0 ? d : 0;
    }
    return o;
}

static double rmsDb(const std::vector<float>& x) {
    double s = 0; for (float v : x) s += (double)v * v;
    double r = x.empty() ? 0 : sqrt(s / x.size());
    return r > 0 ? 20 * log10(r / 32768.0) : -99;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: envcorr <ours.wav> <ref.wav>\n"); return 2; }
    std::vector<float> a, b; int ra, rb;
    if (!loadWav(argv[1], a, ra) || !loadWav(argv[2], b, rb)) return 1;
    if (ra != rb) { fprintf(stderr, "rates differ: %d %d\n", ra, rb); return 1; }
    double la = (double)a.size() / ra, lb = (double)b.size() / ra;
    size_t win = ra / 100, n = a.size() < b.size() ? a.size() : b.size();
    a.resize(n); b.resize(n);
    double level = rmsDb(a) - rmsDb(b);
    std::vector<float> ea = env(a, win), eb = env(b, win);
    const int MAXLAG = 25; // 10 ms steps
    int best = 0; double bestR = -2;
    size_t m = ea.size() - 2 * MAXLAG;
    for (int lag = -MAXLAG; lag <= MAXLAG; lag++) {
        double r = corr(&ea[MAXLAG + lag], &eb[MAXLAG], m);
        if (r > bestR) { bestR = r; best = lag; }
    }
    std::vector<float> oa = onsets(ea), ob = onsets(eb);
    double on = corr(&oa[MAXLAG + best], &ob[MAXLAG], m);
    double r0 = corr(&ea[MAXLAG], &eb[MAXLAG], m);
    printf("len %.1f %.1f s  level %+.1f dB  env %.3f (lag %d ms, at 0: %.3f)  onset %.3f\n", la, lb, level, bestR, best * 10, r0, on);
    return 0;
}
