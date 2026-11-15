// gen <outdir>: writes small synthetic S3M files covering common effects,
// for the host render harness.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Cell { int ch; int note; int ins; int vol; char cmd; int param; };
struct Row { std::vector<Cell> cells; };
struct Pattern { Row rows[64]; };

struct Sample {
    std::vector<uint8_t> data; // raw bytes as stored
    uint32_t frames = 0, loopBegin = 0, loopEnd = 0;
    bool loop = false, is16 = false;
    uint32_t c2spd = 8363;
    int vol = 64;
};

// Note helper: octave 0-7, semitone 0-11.
int N(int oct, int semi) { return (oct << 4) | semi; }
const int NO = -1;       // no note/instrument
const int OFF = 0xFE;    // player treats 0xFE as a no-op
const int CUT = 0xFF;    // player treats 0xFF as note off

Sample square8(int period, uint32_t frames, uint32_t lb, uint32_t le) {
    Sample s;
    s.frames = frames;
    for (uint32_t i = 0; i < frames; i++) s.data.push_back((i / (period / 2)) % 2 ? 128 + 90 : 128 - 90);
    s.loop = true; s.loopBegin = lb; s.loopEnd = le;
    return s;
}

Sample sine16(int period, uint32_t frames, uint32_t lb, uint32_t le) {
    Sample s;
    s.frames = frames; s.is16 = true;
    for (uint32_t i = 0; i < frames; i++) {
        int v = (int)lround(sin(2 * M_PI * i / period) * 26000) + 32768; // unsigned 16-bit
        s.data.push_back(v & 0xFF);
        s.data.push_back(v >> 8);
    }
    s.loop = true; s.loopBegin = lb; s.loopEnd = le;
    return s;
}

Sample noise8(uint32_t frames) {
    Sample s;
    s.frames = frames;
    uint32_t x = 12345;
    for (uint32_t i = 0; i < frames; i++) {
        x = x * 1103515245u + 12345u;
        int env = 127 - (int)(127.0 * i / frames);
        s.data.push_back((uint8_t)(128 + (int)((int8_t)(x >> 24)) * env / 128));
    }
    return s;
}

void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) { b[at] = v & 0xFF; b[at + 1] = (v >> 8) & 0xFF; }
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF; }
void pad16(std::vector<uint8_t>& b) { while (b.size() % 16) b.push_back(0); }

bool writeS3m(const std::string& path, int channels, int speed, int tempo,
              const std::vector<int>& orders, const std::vector<Pattern>& pats,
              const std::vector<Sample>& samples, const std::vector<int>& pans) {
    std::vector<uint8_t> f(96, 0);
    memcpy(&f[0], "host test", 9);
    f[28] = 0x1A; f[29] = 16;
    int ordNum = (int)orders.size();
    if (ordNum % 2) ordNum++;
    put16(f, 32, ordNum);
    put16(f, 34, (uint32_t)samples.size());
    put16(f, 36, (uint32_t)pats.size());
    put16(f, 40, 0x1320);
    put16(f, 42, 2); // unsigned samples
    memcpy(&f[44], "SCRM", 4);
    f[48] = 64; f[49] = speed; f[50] = tempo; f[51] = 0xB0; f[53] = 0xFC;
    for (int i = 0; i < 32; i++) f[64 + i] = (i < channels) ? (i % 2 ? 8 + i / 2 : i / 2) : 0xFF;
    for (int i = 0; i < ordNum; i++) f.push_back(i < (int)orders.size() ? orders[i] : 0xFF);
    size_t insPP = f.size();
    f.resize(f.size() + samples.size() * 2);
    size_t patPP = f.size();
    f.resize(f.size() + pats.size() * 2);
    for (int i = 0; i < 32; i++) f.push_back(i < (int)pans.size() ? (0x20 | pans[i]) : 0);

    std::vector<size_t> insAt;
    for (size_t i = 0; i < samples.size(); i++) {
        pad16(f);
        put16(f, insPP + i * 2, (uint32_t)(f.size() / 16));
        insAt.push_back(f.size());
        f.resize(f.size() + 80, 0);
    }
    for (size_t p = 0; p < pats.size(); p++) {
        pad16(f);
        put16(f, patPP + p * 2, (uint32_t)(f.size() / 16));
        size_t start = f.size();
        f.push_back(0); f.push_back(0);
        for (int r = 0; r < 64; r++) {
            for (const Cell& c : pats[p].rows[r].cells) {
                uint8_t flag = c.ch & 31;
                if (c.note != NO || c.ins != NO) flag |= 0x20;
                if (c.vol >= 0) flag |= 0x40;
                if (c.cmd) flag |= 0x80;
                f.push_back(flag);
                if (flag & 0x20) { f.push_back(c.note == NO ? 0xFF : c.note); f.push_back(c.ins == NO ? 0 : c.ins); }
                if (flag & 0x40) f.push_back(c.vol);
                if (flag & 0x80) { f.push_back(c.cmd - 'A' + 1); f.push_back(c.param); }
            }
            f.push_back(0);
        }
        put16(f, start, (uint32_t)(f.size() - start));
    }
    for (size_t i = 0; i < samples.size(); i++) {
        pad16(f);
        const Sample& s = samples[i];
        size_t h = insAt[i];
        uint32_t seg = (uint32_t)(f.size() / 16);
        f[h] = 1;
        memcpy(&f[h + 1], "sample.raw", 10);
        f[h + 13] = (seg >> 16) & 0xFF;
        put16(f, h + 14, seg & 0xFFFF);
        put32(f, h + 16, s.frames);
        put32(f, h + 20, s.loopBegin);
        put32(f, h + 24, s.loopEnd);
        f[h + 28] = s.vol;
        f[h + 31] = (s.loop ? 1 : 0) | (s.is16 ? 4 : 0);
        put32(f, h + 32, s.c2spd);
        memcpy(&f[h + 48], "test sample", 11);
        memcpy(&f[h + 76], "SCRS", 4);
        f.insert(f.end(), s.data.begin(), s.data.end());
    }
    FILE* o = fopen(path.c_str(), "wb");
    if (!o) { perror(path.c_str()); return false; }
    fwrite(f.data(), 1, f.size(), o);
    fclose(o);
    return true;
}

void add(Pattern& p, int row, int ch, int note, int ins, int vol, char cmd = 0, int param = 0) {
    p.rows[row].cells.push_back({ch, note, ins, vol, cmd, param});
}

std::vector<Sample> basicSamples() {
    std::vector<Sample> s;
    s.push_back(square8(64, 3000, 37, 2981));      // 1: 8-bit square, loop not on a chunk boundary
    s.push_back(sine16(100, 5000, 1003, 4903));    // 2: 16-bit sine
    s.push_back(noise8(6000));                     // 3: one-shot noise
    Sample hi = square8(16, 2048, 0, 2048);        // 4: high c2spd
    hi.c2spd = 22050;
    s.push_back(hi);
    return s;
}

// Every common effect at least once, plus jumps and loops.
bool genEffects(const std::string& dir) {
    std::vector<Pattern> p(4);
    Pattern& a = p[0];
    add(a, 0, 0, N(4, 0), 1, -1);
    add(a, 0, 1, N(3, 7), 2, 40, 'X', 0x20);
    add(a, 0, 2, N(5, 0), 3, -1);
    add(a, 0, 3, N(4, 4), 4, 30, 'J', 0x37);
    for (int r = 1; r < 8; r++) add(a, r, 3, NO, NO, -1, 'J', 0);
    add(a, 4, 0, NO, NO, -1, 'D', 0x04);
    add(a, 5, 0, NO, NO, -1, 'D', 0x00);
    add(a, 6, 0, NO, NO, -1, 'D', 0x30);
    add(a, 7, 0, NO, NO, -1, 'D', 0xF2);
    add(a, 8, 0, NO, NO, -1, 'D', 0x2F);
    add(a, 8, 1, N(3, 9), 2, -1, 'G', 0x08);
    for (int r = 9; r < 16; r++) add(a, r, 1, NO, NO, -1, 'G', 0);
    add(a, 10, 2, N(4, 0), 3, -1, 'Q', 0x23);
    add(a, 12, 0, NO, NO, -1, 'H', 0x46);
    add(a, 13, 0, NO, NO, -1, 'H', 0x00);
    add(a, 14, 0, NO, NO, -1, 'U', 0x8F);
    add(a, 16, 0, N(4, 2), 1, 64, 'E', 0x10);
    add(a, 17, 0, NO, NO, -1, 'E', 0x00);
    add(a, 18, 0, NO, NO, -1, 'F', 0x20);
    add(a, 19, 0, NO, NO, -1, 'F', 0xF4);
    add(a, 20, 0, NO, NO, -1, 'F', 0xE8);
    add(a, 21, 0, NO, NO, -1, 'E', 0xF3);
    add(a, 22, 0, NO, NO, -1, 'E', 0xE2);
    add(a, 24, 1, N(4, 0), 2, -1, 'R', 0x48);
    add(a, 25, 1, NO, NO, -1, 'R', 0x00);
    add(a, 26, 1, NO, NO, -1, 'K', 0x02);
    add(a, 27, 1, NO, NO, -1, 'L', 0x01);
    add(a, 28, 2, N(5, 5), 3, -1, 'I', 0x21);
    add(a, 30, 2, NO, NO, -1, 'I', 0x00);
    add(a, 32, 3, N(4, 7), 4, -1, 'S', 0xC3);
    add(a, 33, 3, N(4, 9), 4, -1, 'S', 0xD2);
    add(a, 34, 3, NO, 1, -1);                       // instrument change, no note
    add(a, 35, 3, CUT, NO, -1);
    add(a, 36, 3, OFF, NO, -1);
    add(a, 36, 0, NO, NO, -1, 'S', 0x83);
    add(a, 38, 1, NO, NO, -1, 'Y', 0x44);
    add(a, 40, 0, NO, NO, -1, 'V', 0x30);
    add(a, 41, 0, NO, NO, -1, 'W', 0x02);
    add(a, 44, 0, NO, NO, -1, 'W', 0x10);
    add(a, 46, 0, NO, NO, -1, 'V', 0x40);
    add(a, 48, 0, NO, NO, -1, 'A', 0x03);
    add(a, 52, 0, NO, NO, -1, 'T', 0x96);
    add(a, 56, 0, NO, NO, -1, 'S', 0xE2);
    add(a, 60, 0, NO, NO, -1, 'C', 0x08);

    Pattern& b = p[1];
    add(b, 8, 0, N(3, 0), 1, 50, 'S', 0xB0);
    add(b, 8, 1, N(4, 0), 2, 50);
    add(b, 10, 2, N(5, 0), 3, -1);
    add(b, 12, 0, N(3, 3), 1, 50, 'S', 0xB2);
    add(b, 16, 0, N(3, 5), 1, 50, 'A', 0x06);
    add(b, 20, 0, NO, NO, -1, 'T', 0x7D);
    add(b, 24, 1, N(5, 0), 4, -1, 'X', 0xC0);
    add(b, 32, 0, NO, NO, -1, 'B', 0x03);

    Pattern& c = p[2]; // only reached through order 2 (skipped by the jump)
    add(c, 0, 0, N(6, 0), 3, -1);

    Pattern& d = p[3];
    for (int r = 0; r < 64; r += 4) {
        add(d, r, 0, N(2 + (r / 16), (r / 4) % 12), 1 + (r / 4) % 4, -1);
        add(d, r + 2, 1, N(4, (r / 4) % 12), 2, 20 + r / 2);
    }
    add(d, 63, 0, NO, NO, -1, 'B', 0x00);

    return writeS3m(dir + "/fx.s3m", 4, 6, 125, {0, 1, 2, 3}, p, basicSamples(), {2, 13, 5, 10});
}

// Portamento up until the period hits its floor, and very high notes.
bool genFloor(const std::string& dir) {
    std::vector<Pattern> p(1);
    Pattern& a = p[0];
    add(a, 0, 0, N(7, 11), 4, -1, 'F', 0xFF);
    for (int r = 1; r < 32; r++) add(a, r, 0, NO, NO, -1, 'F', 0xFF);
    add(a, 0, 1, N(4, 0), 1, 30);
    add(a, 32, 0, N(7, 11), 4, -1);
    add(a, 40, 0, N(4, 0), 1, -1, 'F', 0x40);
    for (int r = 41; r < 64; r++) add(a, r, 0, NO, NO, -1, 'F', 0x40);
    add(a, 63, 0, NO, NO, -1, 'B', 0x00);
    return writeS3m(dir + "/floor.s3m", 2, 6, 125, {0}, p, basicSamples(), {7, 8});
}

// 16 channels at once to push the mix into the soft clamp.
bool genMany(const std::string& dir) {
    std::vector<Pattern> p(1);
    Pattern& a = p[0];
    for (int ch = 0; ch < 16; ch++) {
        add(a, (ch % 4), ch, N(3 + ch % 3, (ch * 5) % 12), 1 + ch % 4, 64);
        add(a, 16 + ch, ch, NO, NO, -1, 'D', 0x01);
        add(a, 32, ch, N(4, (ch * 7) % 12), 2 + ch % 2, 64, 'H', 0x24);
    }
    add(a, 63, 0, NO, NO, -1, 'B', 0x00);
    std::vector<int> pans;
    for (int ch = 0; ch < 16; ch++) pans.push_back(ch % 16);
    return writeS3m(dir + "/many.s3m", 16, 4, 140, {0}, p, basicSamples(), pans);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: gen <outdir>\n"); return 2; }
    std::string dir = argv[1];
    bool ok = genEffects(dir) && genFloor(dir) && genMany(dir);
    return ok ? 0 : 1;
}
