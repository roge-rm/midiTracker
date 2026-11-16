// convert_check run <src.mp3|src.wav> <target 0|1|2|list> [writeBudget] [cancelAfterSteps]
//   Runs Converter on the file the way the file player does (step() until
//   it ends) and prints the state, message, new file name, steps and the
//   longest step (list: just the targets). The new file goes next to the source. writeBudget caps
//   the bytes the card takes (card full), cancelAfterSteps cancels.
// convert_check cmp <ours.wav> <ref.raw> <rate> <channels>
//   Checks ours's header (sizes match the file, format 16-bit PCM at rate
//   and channels) and compares its audio with ref (s16le, same layout):
//   the lag that lines them up, the correlation per channel, the level
//   ratio and the length difference.
// convert_check names
//   Checks the temp name test and the free name picking on a scratch dir.
#include <Arduino.h>
#include <SdFat.h>
#include <sd_card.h>
#include "convert.h"
#include "wav_file.h"
#include "mp3_file.h"
#include <cmath>
#include <vector>
#include <string>

static int run(int argc, char** argv) {
    const char* src = argv[2];
    int target = atoi(argv[3]);
    long budget = argc > 4 ? atol(argv[4]) : -1;
    long cancelAfter = argc > 5 ? atol(argv[5]) : -1;
    static WavPlayer wav;
    static Mp3Player mp3;
    WavConvertSource wavSrc(wav);
    Mp3ConvertSource mp3Src(mp3);
    size_t len = strlen(src);
    bool isMp3 = len > 4 && strcasecmp(src + len - 4, ".mp3") == 0;
    ConvertSource& cs = isMp3 ? (ConvertSource&)mp3Src : (ConvertSource&)wavSrc;

    // The targets the menu would list.
    ConvertFormat fmt;
    bool probed = convertProbe(isMp3 ? FILE_MP3 : FILE_WAV, src, wav, fmt);
    ConvertTarget ts[4];
    int nt = convertTargets(isMp3 ? FILE_MP3 : FILE_WAV, probed ? &fmt : nullptr, ts, 4);
    printf("targets:");
    for (int i = 0; i < nt; i++) printf(" %d(%s)", ts[i], convertTargetLabel(ts[i]));
    printf("\n");
    if (!strcmp(argv[3], "list")) return 0;

    const char* slash = strrchr(src, '/');
    const char* name = slash ? slash + 1 : src;
    static Converter c; // 2 KB buffer
    FsFile::writeBudget = budget;
    if (!c.begin(cs, src, name, (ConvertTarget)target)) {
        printf("begin failed: %s\n", c.message());
        return 1;
    }
    printf("out %s, %lu Hz %d ch\n", c.outputName(), (unsigned long)c.outRate(), c.outChannels());
    long steps = 0;
    uint32_t worst = 0;
    int lastPermille = -1;
    bool permilleBack = false;
    while (c.state() == Converter::STATE_RUNNING) {
        if (steps == cancelAfter) {
            c.cancel();
            break;
        }
        uint32_t t0 = micros();
        c.step();
        uint32_t d = micros() - t0;
        if (d > worst) worst = d;
        if (c.progressPermille() < lastPermille) permilleBack = true;
        lastPermille = c.progressPermille();
        steps++;
    }
    FsFile::writeBudget = -1;
    const char* states[] = {"IDLE", "RUNNING", "DONE", "FAILED", "CANCELLED"};
    printf("state %s, msg '%s', steps %ld, worst step %lu us, permille %d%s, elapsed %lu ms\n", states[c.state()], c.message(), steps,
           (unsigned long)worst, c.progressPermille(), permilleBack ? " (went back)" : "", (unsigned long)c.elapsedMs());
    // No temp file may be left, and the output exists only when done.
    std::string dir(src, slash ? (size_t)(slash - src + 1) : 0);
    std::string tmp = dir + "." + c.outputName() + ".cvt";
    std::string out = dir + c.outputName();
    bool tmpLeft = sdExists(tmp.c_str()), outThere = sdExists(out.c_str());
    printf("temp left: %s, output there: %s\n", tmpLeft ? "yes" : "no", outThere ? "yes" : "no");
    bool ok = !tmpLeft && outThere == (c.state() == Converter::STATE_DONE);
    printf("files %s\n", ok ? "ok" : "WRONG");
    return ok ? 0 : 3;
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static std::vector<int16_t> readAll(const char* path, std::vector<uint8_t>* raw) {
    std::vector<int16_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return v; }
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    if (raw) *raw = b;
    v.resize(b.size() / 2);
    memcpy(v.data(), b.data(), v.size() * 2);
    return v;
}

static double corr(const int16_t* a, const int16_t* b, size_t n, int ch, int c) {
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; i++) {
        double x = a[i * ch + c], y = b[i * ch + c];
        sab += x * y; saa += x * x; sbb += y * y;
    }
    return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : (saa == sbb ? 1.0 : 0.0);
}

static int cmp(int argc, char** argv) {
    uint32_t rate = (uint32_t)atol(argv[4]);
    int ch = atoi(argv[5]);
    std::vector<uint8_t> raw;
    readAll(argv[2], &raw);
    bool hdrOk = raw.size() >= 44 && !memcmp(raw.data(), "RIFF", 4) && !memcmp(raw.data() + 8, "WAVEfmt ", 8) &&
                 le32(&raw[4]) == raw.size() - 8 && le32(&raw[16]) == 16 && le16(&raw[20]) == 1 && le16(&raw[22]) == ch &&
                 le32(&raw[24]) == rate && le32(&raw[28]) == rate * ch * 2 && le16(&raw[32]) == ch * 2 && le16(&raw[34]) == 16 &&
                 !memcmp(&raw[36], "data", 4) && le32(&raw[40]) == raw.size() - 44;
    printf("header %s\n", hdrOk ? "ok" : "WRONG");
    std::vector<int16_t> ours(raw.size() > 44 ? (raw.size() - 44) / 2 : 0);
    memcpy(ours.data(), raw.data() + 44, ours.size() * 2);
    std::vector<int16_t> ref = readAll(argv[3], nullptr);
    size_t na = ours.size() / ch, nb = ref.size() / ch;
    // The lag (ours later by lag frames) over the first 3 s past the first 0.5 s.
    size_t from = rate / 2, win = rate * 3;
    if (from + win + 4000 > na || from + win + 4000 > nb) { from = 0; win = (na < nb ? na : nb) / 2; }
    int bestLag = 0;
    double best = -2;
    for (int lag = -3000; lag <= 3000; lag++) {
        if ((long)from + lag < 0 || from + lag + win > na) continue;
        double r = corr(ours.data() + (from + lag) * ch, ref.data() + from * ch, win, ch, 0);
        if (r > best) { best = r; bestLag = lag; }
    }
    size_t a0 = bestLag > 0 ? bestLag : 0, b0 = bestLag < 0 ? -bestLag : 0;
    size_t n = na - a0 < nb - b0 ? na - a0 : nb - b0;
    printf("lag %d frames, length ours %zu ref %zu (diff %ld)\n", bestLag, na, nb, (long)na - (long)nb);
    double rms[2] = {0, 0};
    for (int c = 0; c < ch; c++) {
        double r = corr(ours.data() + a0 * ch, ref.data() + b0 * ch, n, ch, c);
        double ea = 0, eb = 0;
        for (size_t i = 0; i < n; i++) {
            double x = ours[(a0 + i) * ch + c], y = ref[(b0 + i) * ch + c];
            ea += x * x; eb += y * y;
        }
        rms[c] = eb > 0 ? sqrt(ea / eb) : 0;
        printf("ch %d corr %.5f level %.3f\n", c, r, rms[c]);
    }
    return hdrOk ? 0 : 3;
}

static int names(int argc, char** argv) {
    int bad = 0;
    auto expect = [&](bool v, const char* what) {
        if (!v) { printf("FAIL %s\n", what); bad++; }
    };
    expect(isConvertTempName(".Song.wav.cvt"), "temp name");
    expect(!isConvertTempName("Song.wav.cvt"), "not hidden");
    expect(!isConvertTempName(".Song.wav.part"), "recorder temp");
    expect(!isConvertTempName(".cvt"), "bare");
    // Targets.
    ConvertFormat f;
    ConvertTarget t[4];
    f.rate = 44100; f.channels = 2; f.pcm16 = true;
    expect(convertTargets(FILE_WAV, &f, t, 4) == 1 && t[0] == CONVERT_WAV_44K_MONO, "44.1k stereo wav: mono only");
    f.channels = 1;
    expect(convertTargets(FILE_WAV, &f, t, 4) == 1 && t[0] == CONVERT_WAV_44K_STEREO, "44.1k mono wav: stereo only");
    f.rate = 48000;
    expect(convertTargets(FILE_WAV, &f, t, 4) == 2, "48k mono wav: both");
    f.rate = 44100; f.channels = 2; f.pcm16 = false;
    expect(convertTargets(FILE_WAV, &f, t, 4) == 2, "44.1k 24-bit wav: both");
    expect(convertTargets(FILE_WAV, nullptr, t, 4) == 0, "unreadable wav: none");
    expect(convertTargets(FILE_MP3, nullptr, t, 4) == 2, "mp3: two");
    expect(convertTargets(FILE_MID, nullptr, t, 4) == 0, "mid: none yet");
    printf("names %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "run")) return run(argc, argv);
    if (argc >= 6 && !strcmp(argv[1], "cmp")) return cmp(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "names")) return names(argc, argv);
    fprintf(stderr, "usage: convert_check run <src> <target> [writeBudget] [cancelAfterSteps]\n"
                    "       convert_check cmp <ours.wav> <ref.raw> <rate> <channels>\n"
                    "       convert_check names\n");
    return 2;
}
