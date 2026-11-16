// tail_check <ours.raw> <ref.raw> [frames]: compares the last frames (default
// 22050, or as many as ours has) of two stereo int16 renders, lined up at their ends. Tries lags of
// up to 4 frames and prints the best one, its largest sample difference and
// the longest run of near-silent frames in ours where ref isn't. Exits 1
// unless the lag (frames ref has past ours) is 0 or 1, the difference is under 256 and no run is
// longer than 64 frames.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

static std::vector<int16_t> load(const char* path) {
    std::vector<int16_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    int16_t b[4096];
    size_t n;
    while ((n = fread(b, 2, 4096, f)) > 0) v.insert(v.end(), b, b + n);
    fclose(f);
    v.resize(v.size() & ~(size_t)1);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: tail_check <ours.raw> <ref.raw> [frames]\n"); return 2; }
    std::vector<int16_t> a = load(argv[1]), b = load(argv[2]);
    long na = (long)a.size() / 2, nb = (long)b.size() / 2;
    long n = argc > 3 ? atol(argv[3]) : 22050;
    if (n > na - 8) n = na - 8; // a seek near the end leaves less
    if (n < 64 || nb < n + 8) { printf("FAIL: too short, ours %ld, ref %ld frames\n", na, nb); return 1; }
    // ours[na - 1 - i] against ref[nb - 1 - i - lag], skipping the last 4
    // frames so every lag stays inside both
    int best = 0;
    long bestDiff = -1;
    for (int lag = -4; lag <= 4; lag++) {
        long worst = 0;
        for (long i = 4; i < n + 4; i++) {
            long ia = na - 1 - i, ib = nb - 1 - i - lag;
            for (int c = 0; c < 2; c++) {
                long d = labs((long)a[ia * 2 + c] - b[ib * 2 + c]);
                if (d > worst) worst = d;
            }
        }
        if (bestDiff < 0 || worst < bestDiff) { bestDiff = worst; best = lag; }
    }
    long run = 0, longest = 0;
    for (long i = 4; i < n + 4; i++) {
        long ia = na - 1 - i, ib = nb - 1 - i - best;
        bool quiet = abs(a[ia * 2]) < 8 && abs(a[ia * 2 + 1]) < 8;
        bool loud = abs(b[ib * 2]) >= 256 || abs(b[ib * 2 + 1]) >= 256;
        run = quiet && (loud || run) ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    bool ok = (best == 0 || best == 1) && bestDiff < 256 && longest <= 64;
    printf("%s: lag %d, max diff %ld, silent run %ld (ours %ld, ref %ld frames)\n", ok ? "ok" : "FAIL", best, bestDiff, longest, na, nb);
    return ok ? 0 : 1;
}
