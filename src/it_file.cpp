// Impulse Tracker .it player, see it_file.h.
//
// Mixing: the base's block mixer (see tracker_base.h) with IT's own gains
// and the resonant filter on voices that have one. A cut note (NNA cut,
// ^^^, SCx, DCA cut, S70) fades out in a free voice with a copy of its
// cache, so the new note doesn't click.
// Surround plays the right side inverted, as OpenMPT does.
//
// Filters: instrument cutoff/resonance, filter envelopes and Zxx through
// the file's MIDI macros (or OpenMPT's defaults: SF0 cutoff, Z80-Z8F
// resonance). Only the F0F000xx and F0F001xx macros do anything.
// Coefficients are worked out in float when they change, the filter itself
// is 32-bit fixed point, about 6 multiplies a frame per filtered voice.
//
// Left out (each plays as noted):
//  - MIDI macros other than filter cutoff and resonance, smooth Zxx (\xx)
//    and the high-pass filter mode: ignored.
//  - Stereo samples are mixed down to mono (in the cache, so they stream
//    like mono ones).
//  - Glissando (S1x), finetune (S2x), S9x other than surround: ignored.
//  - IT's cubic interpolation: plain linear.
//  - Random volume and pan (RV/RP) use their own random numbers, so they
//    vary like IT's but not the same way.
//  - The song stops at the end of the order list, it doesn't loop.
//
// Pitch is held as a period in one of two units, chosen per file:
//  - linear: 1/64 semitone, (120 - note) * 64, so C-5 (note 60) is 3840 and
//    plays at the sample's C5 speed;
//  - Amiga: ST3 period times 256, 256 * PERIOD_K / Hz.
// Slides move it by the same "ST3 units" either way (Exx 4*xx, fine 4*x,
// extra fine x), which is how IT scales its slides in both modes.
//
// Voices: MAX_VOICES (32) is the same count XM mixes at its busiest, which
// runs on the device. Each costs about 1.2 KB (struct plus a 512-frame
// cache), the whole player sets SharedRamArena's size.
//
// Compressed samples are unpacked into a cache file on the card the first
// time a song is opened (progress through progressCallback), then streamed
// like plain PCM, and the half and quarter rate copies (see below) are
// made then too. Unpacking while streaming would need the decoder's state
// at every point a loop or ping-pong turn lands on, which doesn't fit RAM.
#include "it_file.h"
#include "ui_tap.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "sd_card.h"

const char* ItPlayer::cacheDir = "/.itcache";
void (*ItPlayer::progressCallback)(int percent) = nullptr;
#ifdef TRACKER_PROFILE
uint32_t ItPlayer::profileSampleReads[MAX_SAMPLES];
#endif

namespace {

const float PERIOD_K = 14317456.0f; // 8363 * 1712: Hz = PERIOD_K / period
// Amiga periods are kept in 1/256 units so high C5 speeds stay in tune.
const float AMIGA_FRACT = 256.0f;
const int32_t MAX_PERIOD = 1 << 30;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

uint32_t fnv(uint32_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// The copy a voice reads at `ratio` source frames per output frame
// (level 0 the sample, 1 half rate, 2 quarter rate), from the one it reads
// now. It moves up at 2 and 4 and back down a bit over a semitone lower,
// so vibrato around a boundary doesn't move it back and forth.
int levelFor(float ratio, int cur, int maxLevel) {
    int want = ratio >= 4.0f ? 2 : ratio >= 2.0f ? 1 : 0;
    if (want < cur) {
        if (cur == 2 && ratio >= 3.75f) want = 2;
        else if (ratio >= 1.875f) want = 1;
    }
    return want > maxLevel ? maxLevel : want;
}

// Half a sine cycle, 0-255 (the MOD table).
const uint8_t SINE_HALF[32] = {0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                               255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24};

uint32_t g_rand = 0x1234567;
inline uint32_t nextRand() { g_rand = g_rand * 1103515245u + 12345u; return g_rand >> 16; }

// Quarter of IT's 256-step sine, 0-64.
const uint8_t IT_SINE_QUARTER[65] = {0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 16, 17, 19, 20, 22, 23, 24, 26, 27, 29, 30, 32,
    33, 34, 36, 37, 38, 39, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 56, 57, 58, 59, 59, 60,
    60, 61, 61, 62, 62, 62, 63, 63, 63, 64, 64, 64, 64, 64, 64};

int itSine(uint8_t pos) {
    int q = pos & 63;
    int v = (pos & 64) ? IT_SINE_QUARTER[64 - q] : IT_SINE_QUARTER[q];
    return (pos & 128) ? -v : v;
}

// IT's resonance damping, 10^(-r * 24 / 128 / 20), as OpenMPT's ITResonanceTable.
const float RESO_DAMP[128] = {
    1.0f, 0.978644609451294f, 0.9577452540397644f, 0.937292218208313f, 0.9172759056091309f, 0.8976871371269226f, 0.878516674041748f, 0.8597555756568909f,
    0.8413951396942139f, 0.8234267830848694f, 0.8058421611785889f, 0.7886331081390381f, 0.7717915177345276f, 0.7553095817565918f, 0.7391796708106995f, 0.7233941555023193f,
    0.7079457640647888f, 0.6928272843360901f, 0.6780316829681396f, 0.6635520458221436f, 0.6493816375732422f, 0.6355138421058655f, 0.6219421625137329f, 0.6086603403091431f,
    0.5956621170043945f, 0.5829415321350098f, 0.5704925656318665f, 0.5583094954490662f, 0.5463865399360657f, 0.5347182154655457f, 0.5232990980148315f, 0.5121238231658936f,
    0.5011872053146362f, 0.49048417806625366f, 0.48000967502593994f, 0.46975886821746826f, 0.4597269892692566f, 0.4499093294143677f, 0.4403013288974762f, 0.430898517370224f,
    0.4216965138912201f, 0.41269099712371826f, 0.4038778245449066f, 0.39525285363197327f, 0.3868120610713959f, 0.37855151295661926f, 0.3704673945903778f, 0.36255592107772827f,
    0.3548133969306946f, 0.3472362160682678f, 0.33982083201408386f, 0.3325638175010681f, 0.3254617750644684f, 0.318511426448822f, 0.311709463596344f, 0.305052787065506f,
    0.2985382676124573f, 0.29216286540031433f, 0.2859236001968384f, 0.2798175811767578f, 0.2738419771194458f, 0.2679939568042755f, 0.26227083802223206f, 0.25666993856430054f,
    0.25118863582611084f, 0.2458244115114212f, 0.24057473242282867f, 0.23543715476989746f, 0.23040929436683655f, 0.2254888117313385f, 0.22067341208457947f, 0.21596083045005798f,
    0.21134890615940094f, 0.20683546364307404f, 0.20241840183734894f, 0.1980956792831421f, 0.19386526942253113f, 0.18972519040107727f, 0.1856735348701477f, 0.18170839548110962f,
    0.17782793939113617f, 0.1740303486585617f, 0.17031386494636536f, 0.16667674481868744f, 0.16311728954315186f, 0.15963385999202728f, 0.1562248021364212f, 0.15288856625556946f,
    0.1496235728263855f, 0.14642828702926636f, 0.14330126345157623f, 0.14024099707603455f, 0.13724610209465027f, 0.1343151479959488f, 0.13144679367542267f, 0.12863969802856445f,
    0.1258925348520279f, 0.1232040524482727f, 0.12057298421859741f, 0.11799809336662292f, 0.11547819525003433f, 0.11301211267709732f, 0.11059869080781937f, 0.10823681205511093f,
    0.10592537373304367f, 0.10366329550743103f, 0.10144951939582825f, 0.09928302466869354f, 0.09716279804706573f, 0.09508784115314484f, 0.09305720776319504f, 0.09106992930173874f,
    0.08912509679794312f, 0.08722179383039474f, 0.0853591337800026f, 0.08353625237941742f, 0.08175230026245117f, 0.08000645041465759f, 0.07829787582159042f, 0.07662579417228699f,
    0.0749894231557846f, 0.0733879879117012f, 0.07182075828313828f, 0.07028699666261673f, 0.06878598779439926f, 0.06731703877449036f, 0.0658794566988945f, 0.06447257101535797f,
};
// 2^(x / 12288) for x = c * 96 + f, so the filter cutoff needs no powf.
const float EXP2_COARSE[128] = {
    1.0f, 1.0054298639297485f, 1.0108892917633057f, 1.0163782835006714f, 1.0218971967697144f, 1.027445912361145f, 1.0330249071121216f, 1.0386340618133545f,
    1.0442737340927124f, 1.0499440431594849f, 1.055645227432251f, 1.0613771677017212f, 1.0671404600143433f, 1.072934865951538f, 1.0787607431411743f, 1.084618330001831f,
    1.0905077457427979f, 1.0964291095733643f, 1.1023825407028198f, 1.1083683967590332f, 1.114386796951294f, 1.120437741279602f, 1.1265215873718262f, 1.1326385736465454f,
    1.1387885808944702f, 1.1449720859527588f, 1.1511892080307007f, 1.1574400663375854f, 1.1637248992919922f, 1.1700438261032104f, 1.1763969659805298f, 1.1827846765518188f,
    1.1892070770263672f, 1.195664405822754f, 1.2021567821502686f, 1.2086843252182007f, 1.215247392654419f, 1.2218459844589233f, 1.228480577468872f, 1.2351510524749756f,
    1.2418577671051025f, 1.248600959777832f, 1.2553807497024536f, 1.2621973752975464f, 1.2690509557724f, 1.2759417295455933f, 1.2828700542449951f, 1.2898359298706055f,
    1.2968395948410034f, 1.303881287574768f, 1.3109612464904785f, 1.3180795907974243f, 1.3252366781234741f, 1.332432508468628f, 1.339667558670044f, 1.3469418287277222f,
    1.3542555570602417f, 1.3616089820861816f, 1.3690024614334106f, 1.3764359951019287f, 1.3839099407196045f, 1.3914244174957275f, 1.398979663848877f, 1.4065760374069214f,
    1.4142135381698608f, 1.4218926429748535f, 1.4296133518218994f, 1.4373760223388672f, 1.4451807737350464f, 1.4530279636383057f, 1.4609178304672241f, 1.4688503742218018f,
    1.4768261909484863f, 1.4848451614379883f, 1.4929077625274658f, 1.5010141134262085f, 1.5091644525527954f, 1.5173590183258057f, 1.525598168373108f, 1.5338820219039917f,
    1.5422108173370361f, 1.5505849123001099f, 1.5590044260025024f, 1.567469596862793f, 1.5759809017181396f, 1.584538221359253f, 1.5931421518325806f, 1.601792812347412f,
    1.610490322113037f, 1.6192351579666138f, 1.6280274391174316f, 1.6368674039840698f, 1.6457555294036865f, 1.6546918153762817f, 1.6636766195297241f, 1.6727101802825928f,
    1.6817928552627563f, 1.6909247636795044f, 1.7001063823699951f, 1.7093377113342285f, 1.7186193466186523f, 1.7279512882232666f, 1.73733389377594f, 1.7467674016952515f,
    1.7562521696090698f, 1.7657884359359741f, 1.7753764390945435f, 1.785016655921936f, 1.7947090864181519f, 1.8044542074203491f, 1.8142521381378174f, 1.8241033554077148f,
    1.8340080976486206f, 1.8439666032791138f, 1.8539791107177734f, 1.8640460968017578f, 1.8741676807403564f, 1.884344220161438f, 1.8945759534835815f, 1.9048633575439453f,
    1.9152065515518188f, 1.9256058931350708f, 1.9360617399215698f, 1.9465744495391846f, 1.9571441411972046f, 1.9677711725234985f, 1.9784560203552246f, 1.9891988039016724f,
};
const float EXP2_FINE[96] = {
    1.0f, 1.0000563859939575f, 1.000112771987915f, 1.000169277191162f, 1.0002256631851196f, 1.0002820491790771f, 1.0003385543823242f, 1.0003949403762817f,
    1.0004513263702393f, 1.0005078315734863f, 1.0005642175674438f, 1.000620722770691f, 1.0006771087646484f, 1.0007336139678955f, 1.000789999961853f, 1.0008465051651f,
    1.0009028911590576f, 1.0009593963623047f, 1.0010159015655518f, 1.0010722875595093f, 1.0011287927627563f, 1.0011852979660034f, 1.0012418031692505f, 1.001298189163208f,
    1.001354694366455f, 1.0014111995697021f, 1.0014677047729492f, 1.0015242099761963f, 1.0015807151794434f, 1.0016372203826904f, 1.0016937255859375f, 1.0017502307891846f,
    1.0018067359924316f, 1.0018632411956787f, 1.0019197463989258f, 1.0019762516021729f, 1.00203275680542f, 1.002089262008667f, 1.002145767211914f, 1.0022023916244507f,
    1.0022588968276978f, 1.0023154020309448f, 1.002371907234192f, 1.0024285316467285f, 1.0024850368499756f, 1.0025416612625122f, 1.0025981664657593f, 1.0026546716690063f,
    1.002711296081543f, 1.00276780128479f, 1.0028244256973267f, 1.0028809309005737f, 1.0029375553131104f, 1.002994179725647f, 1.003050684928894f, 1.0031073093414307f,
    1.0031638145446777f, 1.0032204389572144f, 1.003277063369751f, 1.0033336877822876f, 1.0033901929855347f, 1.0034468173980713f, 1.003503441810608f, 1.0035600662231445f,
    1.0036166906356812f, 1.0036733150482178f, 1.0037299394607544f, 1.003786563873291f, 1.0038431882858276f, 1.0038998126983643f, 1.0039564371109009f, 1.0040130615234375f,
    1.0040696859359741f, 1.0041263103485107f, 1.0041829347610474f, 1.004239559173584f, 1.0042963027954102f, 1.0043529272079468f, 1.0044095516204834f, 1.00446617603302f,
    1.0045229196548462f, 1.0045795440673828f, 1.0046361684799194f, 1.0046929121017456f, 1.0047495365142822f, 1.0048062801361084f, 1.004862904548645f, 1.0049196481704712f,
    1.0049762725830078f, 1.005033016204834f, 1.0050896406173706f, 1.0051463842391968f, 1.0052030086517334f, 1.0052597522735596f, 1.0053164958953857f, 1.005373239517212f,
};

// LFO value -64..64 for a 0-255 position.
int waveValue(uint8_t wave, uint8_t pos) {
    int idx = pos >> 2; // 0-63
    switch (wave & 3) {
        case 1: return 64 - (pos >> 1);           // ramp down
        case 2: return idx < 32 ? 64 : -64;       // square
        case 3: g_rand = g_rand * 1103515245u + 12345u; return (int)((g_rand >> 16) & 127) - 64;
        default: { int v = SINE_HALF[idx & 31] >> 2; return idx < 32 ? v : -v; }
    }
}

// Value of an envelope at tick `pos`, in 1/256 units so slow slopes
// (pitch especially) move smoothly between whole steps.
int envValue(const int8_t* y, const uint16_t* x, int num, uint16_t pos) {
    if (num <= 0) return 0;
    if (pos >= x[num - 1]) return y[num - 1] * 256;
    int i = 0;
    while (i + 1 < num && x[i + 1] <= pos) i++;
    if (i + 1 >= num || x[i + 1] <= x[i]) return y[i] * 256;
    return y[i] * 256 + (int)(y[i + 1] - y[i]) * 256 * (int)(pos - x[i]) / (int)(x[i + 1] - x[i]);
}

// Bit reader over a file, for the IT 2.14 sample unpacker.
struct BitReader {
    FsFile* f;
    uint8_t* buf;
    int cap, len = 0, pos = 0;
    uint32_t bytesLeft; // in this block
    uint32_t bits = 0;
    int count = 0;
    bool ok = true;
    uint32_t read(int n) {
        while (count < n) {
            if (pos >= len) {
                int want = (int)(bytesLeft < (uint32_t)cap ? bytesLeft : (uint32_t)cap);
                int got = want > 0 ? f->read(buf, want) : 0;
                if (got <= 0) { ok = false; return 0; }
                bytesLeft -= (uint32_t)got;
                len = got;
                pos = 0;
            }
            bits |= (uint32_t)buf[pos++] << count;
            count += 8;
        }
        uint32_t v = bits & ((1u << n) - 1);
        bits >>= n;
        count -= n;
        return v;
    }
};

} // namespace

// ---------------------------------------------------------------- loading

bool ItPlayer::load(const char* path) {
#ifdef TRACKER_PROFILE
    memset(profileSampleReads, 0, sizeof(profileSampleReads));
#endif
    close();
    _cacheFile.close();
    _error[0] = '\0';
    if (!sdOpenPath(_file, path, O_RDONLY)) return loadFail("could not open file");
    if (!sdOpenPath(_patternFile, path, O_RDONLY)) return loadFail("could not open file");

    uint8_t h[0xC0];
    if (_file.read(h, sizeof(h)) != (int)sizeof(h) || memcmp(h, "IMPM", 4) != 0) return loadFail("unsupported format");
    uint16_t ordNum = rd16(h + 0x20), insNum = rd16(h + 0x22), smpNum = rd16(h + 0x24), patNum = rd16(h + 0x26);
    uint16_t cmwt = rd16(h + 0x2A), flags = rd16(h + 0x2C);
    if (ordNum > 1024 || insNum > 4000 || smpNum > 4000 || patNum > 4000) return loadFail("invalid IT file");
    _stereo = (flags & 1) != 0;
    _instrumentMode = (flags & 4) != 0;
    _linear = (flags & 8) != 0;
    _oldEffects = (flags & 16) != 0;
    _linkG = (flags & 32) != 0;
    _extFilterRange = (flags & 0x1000) != 0; // OpenMPT's extended filter range
    _globalVol = h[0x30] > 128 ? 128 : h[0x30];
    _mixVolume = h[0x31] > 128 ? 128 : h[0x31];
    uint8_t initSpeed = h[0x32] ? h[0x32] : 6;
    uint8_t initTempo = h[0x33] >= 32 ? h[0x33] : 125;

    memset(_orderTable, 255, sizeof(_orderTable));
    int ordKeep = ordNum < MAX_ORDERS ? ordNum : MAX_ORDERS;
    if (_file.read(_orderTable, ordKeep) != ordKeep) return loadFail("invalid IT file");
    _songLength = ordKeep;
    for (int i = 0; i < ordKeep; i++) {
        if (_orderTable[i] == 255) { _songLength = i; break; }
    }

    // Offset tables, read through _rawScratch. They live in the voice
    // slots, which are free until play, to keep load()'s stack small.
    uint32_t* insOff = (uint32_t*)&_chunks[MAX_VOICES - 1][0];
    uint32_t* smpOff = insOff + MAX_INSTRUMENTS;
    static_assert((MAX_INSTRUMENTS + MAX_SAMPLES) * 4 <= CHUNK_FRAMES * 2, "load()'s offset tables don't fit a voice slot");
    uint32_t tableAt = 0xC0 + ordNum;
    auto readTable = [&](uint32_t at, int count, uint32_t* out, int keep) -> bool {
        if (!_file.seekSet(at)) return false;
        for (int done = 0; done < count && done < keep;) {
            int n = count - done;
            if (n > RAW_SCRATCH_BYTES / 4) n = RAW_SCRATCH_BYTES / 4;
            if (n > keep - done) n = keep - done;
            if (_file.read(_rawScratch, n * 4) != n * 4) return false;
            for (int k = 0; k < n; k++) out[done + k] = rd32(_rawScratch + k * 4);
            done += n;
        }
        return true;
    };
    int insKeep = insNum < MAX_INSTRUMENTS ? insNum : MAX_INSTRUMENTS;
    int smpKeep = smpNum < MAX_SAMPLES ? smpNum : MAX_SAMPLES;
    int patKeep = patNum < MAX_PATTERNS ? patNum : MAX_PATTERNS;
    if (!readTable(tableAt, insNum, insOff, insKeep)) return loadFail("invalid IT file");
    if (!readTable(tableAt + insNum * 4u, smpNum, smpOff, smpKeep)) return loadFail("invalid IT file");
    for (int i = 0; i < MAX_PATTERNS; i++) { _patternOffsets[i] = 0; _patternRows[i] = 64; }
    if (!readTable(tableAt + (insNum + smpNum) * 4u, patNum, _patternOffsets, patKeep)) return loadFail("invalid IT file");
    // End of the last pattern or sample data, where OpenMPT's extensions start.
    uint32_t dataEnd = 0;
    for (int i = 0; i < patKeep; i++) {
        if (_patternOffsets[i] == 0) continue;
        uint8_t ph[8];
        if (!_file.seekSet(_patternOffsets[i]) || _file.read(ph, 8) != 8) { _patternOffsets[i] = 0; continue; }
        uint16_t rows = rd16(ph + 2);
        if (_patternOffsets[i] + 8 + rd16(ph) > dataEnd) dataEnd = _patternOffsets[i] + 8 + rd16(ph);
        _patternRows[i] = (rows >= 1 && rows <= 256) ? rows : 64;
        _patternOffsets[i] += 8;
    }

    _numSamples = smpKeep;
    uint32_t key = fnv(2166136261u, (const uint8_t*)path, strlen(path));
    key = fnv(key, h, sizeof(h));
    uint32_t fileBytes = (uint32_t)_file.fileSize();
    key = fnv(key, (const uint8_t*)&fileBytes, 4);
    bool anyCached = false, anyLevels = false;
    for (int i = 0; i < smpKeep; i++) {
        if (!loadSample(i, smpOff[i])) _samples[i] = ItSample();
        // The cache key covers every sample header.
        if (_file.seekSet(smpOff[i]) && _file.read(_rawScratch, 0x50) == 0x50) key = fnv(key, _rawScratch, 0x50);
        if (_samples[i].inCache) anyCached = true;
        uint32_t end = sampleDataEnd(_samples[i]);
        if (end > dataEnd) dataEnd = end;
    }
    _numInstruments = _instrumentMode ? insKeep : smpKeep;
    for (int i = 0; i < MAX_INSTRUMENTS; i++) {
        if (!_instrumentMode || i >= insKeep || !loadInstrument(i, insOff[i], cmwt < 0x200)) {
            _instruments[i] = ItInstrument();
            for (int n = 0; n < 120; n++) { _instruments[i].noteMap[n] = (uint8_t)n; _instruments[i].sampleMap[n] = 0; }
        }
    }

    // Half and quarter rate copies for long samples the patterns play at 2
    // or 4 source frames a frame or faster. The voice slots are free until
    // play, so the scan's tables go there.
    uint8_t* maxNote = (uint8_t*)&_chunks[0][0];
    scanChannels(maxNote);
    for (ItSampleView& lv : _levelViews) lv = ItSampleView();
    for (int i = 0; i < smpKeep; i++) {
        ItSample& s = _samples[i];
        s.levels = 0;
        const ItSampleView* w = wrapLoop(s);
        if (s.length >= MIN_LEVEL_FRAMES && maxNote[i] && (!w || w->realLoopEnd() - w->loopStart >= MIN_LEVEL_LOOP)) {
            float ratio = (float)s.c5speed * powf(2.0f, (float)(maxNote[i] - 1 - 60) / 12.0f) / (float)SAMPLE_RATE;
            s.levels = ratio >= 4.0f ? 2 : ratio >= 2.0f ? 1 : 0;
        }
        key = fnv(key, &s.levels, 1);
        if (s.levels) anyLevels = true;
    }

    if ((anyCached || anyLevels) && !openCache(path, key)) {
        if (anyCached) {
            char msg[sizeof(_error)];
            strcpy(msg, _error[0] ? _error : "can't write the sample cache");
            return loadFail(msg);
        }
        // Only the copies needed the cache: play without them.
        for (int i = 0; i < smpKeep; i++) _samples[i].levels = 0;
        _error[0] = 0;
    }
    loadMacros(h[0x2E], tableAt + (insNum + smpNum + patNum) * 4u);
    if (rd16(h + 0x28) < 0x214 && !(h[0x2E] & 8)) {
        // Older IT versions had no filters, as OpenMPT: Zxx does nothing.
        for (FilterMacro& m : _sfxMacro) m = FilterMacro();
        for (FilterMacro& m : _zxxMacro) m = FilterMacro();
    }

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        ItChannel& c = _channels[ch];
        c = ItChannel();
        uint8_t p = h[0x40 + ch];
        c.muted = (p & 128) != 0;
        p &= 127;
        if (p == 100) { c.surround = true; c.pan = 128; }
        else c.pan = (uint16_t)((p > 64 ? 64 : p) * 4);
        c.channelVolume = h[0x80 + ch] > 64 ? 64 : h[0x80 + ch];
    }
    for (int i = 0; i < MAX_VOICES; i++) _voices[i] = ItVoice();
    attachVoices(&_voices[0], sizeof(ItVoice), MAX_VOICES, &_chunks[0][0], CHUNK_FRAMES);
    useSampleFile(_file);
    if (_cacheFile) useSampleFile(_cacheFile);
    _formatGains = true;

    _globalVolume = (uint8_t)(_globalVol >> 1);
    _openPattern = -1;
    _baseSpeed = initSpeed;
    _tickFraction = 0;
    _rowsPerBeat = h[0x1E] ? h[0x1E] : 4;
    // OpenMPT's song settings after the last pattern or sample data.
    SongExtensions ext;
    bool xtpm = readSongExtensions(dataEnd, insNum, ext);
    _tempoMode = (uint8_t)(ext.tempoMode < 0 ? 0 : ext.tempoMode);
    // Master level, as OpenMPT: MV is the pre-amp, and files it reads as
    // made by ModPlug play at its old mix levels unless they say otherwise.
    uint16_t cwtv = rd16(h + 0x28);
    uint32_t reserved = rd32(h + 0x3C);
    bool modplug = ((cwtv & 0xF000) == 0x5000 && memcmp(h + 0x3C, "OMPT", 4) == 0) || cwtv == 0x888 || cmwt == 0x888 ||
                   (cwtv == 0x0214 && cmwt == 0x0202 && reserved == 0) ||
                   (cwtv == 0x0300 && cmwt == 0x0300 && reserved == 0 && ordNum == 256 && h[0x34] == 128 && h[0x35] == 0) ||
                   (cwtv == 0x0217 && cmwt == 0x0200 && reserved == 0) || xtpm;
    uint8_t levels = ext.mixLevels >= 0 && ext.mixLevels <= MIX_COMPATIBLE_FT2 ? (uint8_t)ext.mixLevels
                     : (uint8_t)(modplug ? MIX_ORIGINAL : MIX_COMPATIBLE);
    uint32_t preamp = ext.preamp >= 0 ? (uint32_t)ext.preamp : _mixVolume;
    if (ext.preamp < 0 && cwtv == 0x0214 && cmwt == 0x0214 && memcmp(h + 0x3C, "CHBI", 4) == 0) preamp /= 2; // ChibiTracker
    int chans = ext.channels > _numChannels ? ext.channels : _numChannels;
    _masterGain = masterGainFor(levels, preamp, chans);
    resetSequencer(initSpeed, initTempo);
    return true;
}

bool ItPlayer::loadSample(int i, uint32_t offset) {
    ItSample& s = _samples[i];
    s = ItSample();
    uint8_t b[0x50];
    if (offset == 0 || !_file.seekSet(offset) || _file.read(b, 0x50) != 0x50 || memcmp(b, "IMPS", 4) != 0) return false;
    uint8_t flg = b[0x12];
    s.globalVolume = b[0x11] > 64 ? 64 : b[0x11];
    s.volume = b[0x13] > 64 ? 64 : b[0x13];
    uint8_t cvt = b[0x2E];
    s.defaultPan = (b[0x2F] & 127) > 64 ? 64 : (b[0x2F] & 127);
    s.usePan = (b[0x2F] & 128) != 0;
    s.length = (flg & 1) ? rd32(b + 0x30) : 0;
    if (s.length > 0x4000000) s.length = 0;
    s.c5speed = rd32(b + 0x3C);
    if (s.c5speed == 0) s.c5speed = 8363;
    if (s.c5speed > 9999999) s.c5speed = 9999999;
    s.is16Bit = (flg & 2) != 0;
    s.vibSpeed = b[0x4C] > 64 ? 64 : b[0x4C];
    s.vibDepth = b[0x4D] > 64 ? 64 : b[0x4D];
    s.vibRate = b[0x4E];
    s.vibType = b[0x4F] & 7;
    bool stereo = (flg & 4) != 0;
    bool compressed = (flg & 8) != 0;
    uint32_t ptr = rd32(b + 0x48);
    if (compressed) {
        // Unpacked by openCache(); dataOffset holds the packed data until then.
        s.inCache = true;
        s.packed = true;
        s.isSigned = true;
        s.dataOffset = ptr;
        s.rightOffset = stereo ? 1 : 0; // a flag until openCache()
    } else {
        s.isSigned = (cvt & 1) != 0;
        s.dataOffset = ptr;
        uint32_t bpf = s.is16Bit ? 2 : 1;
        s.rightOffset = stereo ? ptr + s.length * bpf : 0;
        // Stereo is mixed down into the cache, so playing it reads once.
        s.inCache = stereo;
    }
    if (s.length == 0) { s.inCache = false; }
    setupViews(s, i, flg, rd32(b + 0x34), rd32(b + 0x38), rd32(b + 0x40), rd32(b + 0x44));
    // Top bit marks IT 2.15 packing (double delta) for openCache().
    if (compressed && (cvt & 4)) s.dataOffset |= 0x80000000u;
    return true;
}

void ItPlayer::setupViews(ItSample& s, int index, uint8_t flg, uint32_t ls, uint32_t le, uint32_t sls, uint32_t sle) {
    auto make = [&](ItSampleView& v, bool on, bool pp, uint32_t a, uint32_t e) {
        v = ItSampleView();
        v.sampleIndex = (uint8_t)index;
        v.length = s.length;
        v.fileOffset = 0;
        if (e > s.length) e = s.length;
        if (on && e > a + 1) {
            v.loopStart = a;
            v.loopEnd = e;
            if (pp) {
                // Frames e..2e-a-1 read the loop backwards.
                v.pingpong = true;
                v.loopEnd = e + (e - a);
                if (v.length < v.loopEnd) v.length = v.loopEnd;
            }
        }
    };
    make(s.normal, (flg & 0x10) != 0, (flg & 0x40) != 0, ls, le);
    s.hasSustain = (flg & 0x20) && sle > sls + 1 && sls < s.length;
    if (s.hasSustain) {
        make(s.sustain, true, (flg & 0x80) != 0, sls, sle);
        s.sustain.sustainView = true;
    } else {
        s.sustain = s.normal;
    }
}

bool ItPlayer::loadInstrument(int i, uint32_t offset, bool oldFormat) {
    ItInstrument& in = _instruments[i];
    in = ItInstrument();
    uint8_t b[0x230];
    if (offset == 0 || !_file.seekSet(offset)) return false;
    int want = oldFormat ? 0x22A : 0x226;
    if (_file.read(b, want) != want || memcmp(b, "IMPI", 4) != 0) return false;
    for (int n = 0; n < 120; n++) {
        uint8_t nt = b[0x40 + n * 2], sm = b[0x41 + n * 2];
        in.noteMap[n] = nt < 120 ? nt : (uint8_t)n;
        in.sampleMap[n] = sm;
    }
    if (oldFormat) {
        // IT 1.x: volume envelope only, as (tick, value) byte pairs.
        uint8_t ef = b[0x11];
        in.fadeout = (uint16_t)(rd16(b + 0x18) * 2);
        in.nna = b[0x1A] & 3;
        in.dct = b[0x1B] ? 1 : 0;
        ItEnvelope& e = in.volEnv;
        e.flags = (uint8_t)((ef & 1) | ((ef & 2) ? 2 : 0) | ((ef & 4) ? 4 : 0));
        e.loopStart = b[0x12]; e.loopEnd = b[0x13];
        e.susStart = b[0x14]; e.susEnd = b[0x15];
        int n = 0;
        for (; n < MAX_ENV_POINTS; n++) {
            uint8_t t = b[0x1F8 + n * 2];
            if (t == 0xFF) break;
            e.x[n] = t;
            e.y[n] = (int8_t)(b[0x1F9 + n * 2] > 64 ? 64 : b[0x1F9 + n * 2]);
        }
        e.numPoints = (uint8_t)n;
    } else {
        in.nna = b[0x11] & 3;
        in.dct = b[0x12] & 3;
        in.dca = b[0x13] > 2 ? 0 : b[0x13];
        in.fadeout = rd16(b + 0x14) > 256 ? 256 : rd16(b + 0x14);
        in.pitchPanSep = (int8_t)b[0x16];
        in.pitchPanCentre = b[0x17] < 120 ? b[0x17] : 60;
        in.globalVolume = b[0x18] > 128 ? 128 : b[0x18];
        in.defaultPan = (b[0x19] & 127) > 64 ? 64 : (b[0x19] & 127);
        in.usePan = (b[0x19] & 128) == 0;
        in.randVol = b[0x1A] > 100 ? 100 : b[0x1A];
        in.randPan = b[0x1B] > 64 ? 64 : b[0x1B];
        in.filterCutoff = b[0x3A];
        in.filterReso = b[0x3B];
        ItEnvelope* envs[3] = {&in.volEnv, &in.panEnv, &in.pitchEnv};
        for (int k = 0; k < 3; k++) {
            const uint8_t* r = b + 0x130 + k * 82;
            ItEnvelope& e = *envs[k];
            e.flags = r[0];
            e.numPoints = r[1] > MAX_ENV_POINTS ? MAX_ENV_POINTS : r[1];
            e.loopStart = r[2]; e.loopEnd = r[3]; e.susStart = r[4]; e.susEnd = r[5];
            for (int n = 0; n < MAX_ENV_POINTS; n++) {
                int8_t y = (int8_t)r[6 + n * 3];
                if (k == 0) y = y < 0 ? 0 : (y > 64 ? 64 : y);
                else y = y < -32 ? -32 : (y > 32 ? 32 : y);
                e.y[n] = y;
                e.x[n] = rd16(r + 7 + n * 3);
            }
        }
    }
    // Point indices past the end turn their loop off.
    ItEnvelope* envs[3] = {&in.volEnv, &in.panEnv, &in.pitchEnv};
    for (int k = 0; k < 3; k++) {
        ItEnvelope& e = *envs[k];
        if (e.loopStart >= e.numPoints || e.loopEnd >= e.numPoints || e.loopStart > e.loopEnd) e.flags &= ~2;
        if (e.susStart >= e.numPoints || e.susEnd >= e.numPoints || e.susStart > e.susEnd) e.flags &= ~4;
    }
    return true;
}

// End of a sample's data in the .it file, 0 if it has none. Packed data is
// walked block by block (one per side for stereo).
uint32_t ItPlayer::sampleDataEnd(const ItSample& s) {
    if (s.length == 0) return 0;
    uint32_t at = s.dataOffset & 0x7FFFFFFFu;
    uint32_t sides = s.rightOffset ? 2 : 1;
    if (!s.packed) return at + s.length * (s.is16Bit ? 2 : 1) * sides;
    uint32_t blockFrames = s.is16Bit ? 0x4000 : 0x8000;
    uint32_t blocks = (s.length + blockFrames - 1) / blockFrames * sides;
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t lb[2];
        if (!_file.seekSet(at) || _file.read(lb, 2) != 2) return at;
        at += 2 + rd16(lb);
    }
    return at;
}

void ItPlayer::scanChannels(uint8_t* maxNote) {
    // Highest channel any pattern uses, for the screen, and each sample's
    // highest note (+1, 0 == never played) for openCache(). A note with no
    // instrument takes the channel's last one in file order, near enough.
    int maxCh = 0;
    uint8_t* lastNote = maxNote + MAX_SAMPLES;
    uint8_t* lastInst = lastNote + MAX_CHANNELS;
    uint8_t* chanInst = lastInst + MAX_CHANNELS;
    memset(maxNote, 0, MAX_SAMPLES);
    memset(chanInst, 0, MAX_CHANNELS);
    for (int p = 0; p < MAX_PATTERNS; p++) {
        if (_patternOffsets[p] == 0) continue;
        uint8_t ph[2];
        if (!_file.seekSet(_patternOffsets[p] - 8) || _file.read(ph, 2) != 2) continue;
        uint32_t left = rd16(ph);
        _file.seekSet(_patternOffsets[p]);
        uint8_t mask[MAX_CHANNELS];
        memset(mask, 0, sizeof(mask));
        memset(lastNote, 0xFF, MAX_CHANNELS);
        memset(lastInst, 0, MAX_CHANNELS);
        int len = 0, pos = 0;
        auto next = [&](uint8_t& v) -> bool {
            if (pos >= len) {
                if (left == 0) return false;
                int want = left > (uint32_t)RAW_SCRATCH_BYTES ? RAW_SCRATCH_BYTES : (int)left;
                len = _file.read(_rawScratch, want);
                if (len <= 0) return false;
                left -= (uint32_t)len;
                pos = 0;
            }
            v = _rawScratch[pos++];
            return true;
        };
        uint8_t cv;
        while (next(cv)) {
            if (cv == 0) continue;
            int ch = (cv - 1) & 63;
            uint8_t m;
            if (cv & 0x80) { if (!next(m)) break; mask[ch] = m; }
            else m = mask[ch];
            uint8_t note = 0xFF, inst = 0, d;
            bool ok = true;
            if (m & 1) { ok = next(note); lastNote[ch] = note; }
            if (ok && (m & 2)) { ok = next(inst); lastInst[ch] = inst; }
            int skip = ((m & 4) ? 1 : 0) + ((m & 8) ? 2 : 0);
            for (int k = 0; k < skip && ok; k++) ok = next(d);
            if (!ok) break;
            if (m & 16) note = lastNote[ch];
            if (m & 32) inst = lastInst[ch];
            if (inst) chanInst[ch] = inst;
            if (m & 0xFF) { if (ch + 1 > maxCh) maxCh = ch + 1; }
            if (note >= 120 || chanInst[ch] == 0) continue;
            int smp, real = note, swing = 0;
            if (_instrumentMode) {
                if (chanInst[ch] > _numInstruments) continue;
                const ItInstrument& in = _instruments[chanInst[ch] - 1];
                smp = in.sampleMap[note];
                real = in.noteMap[note];
                const ItEnvelope& e = in.pitchEnv;
                if (e.on() && !(e.flags & 0x80)) {
                    // y is in half semitones.
                    for (int k = 0; k < e.numPoints && k < MAX_ENV_POINTS; k++) if ((e.y[k] + 1) / 2 > swing) swing = (e.y[k] + 1) / 2;
                }
            } else {
                smp = chanInst[ch];
            }
            if (smp < 1 || smp > _numSamples || real >= 120) continue;
            int top = real + swing + 1;
            if (top > maxNote[smp - 1]) maxNote[smp - 1] = (uint8_t)(top > 255 ? 255 : top);
        }
    }
    _numChannels = maxCh > 0 ? maxCh : 1;
}

// ---------------------------------------------------------------- sample cache

bool ItPlayer::openCache(const char* path, uint32_t key) {
    // Layout: 16-byte header ("ITC4", key, data bytes, use count), then
    // for each sample in it, in order: its frames as mono signed PCM if it
    // is cached itself, then its half and quarter rate copies (levels).
    // A packed stereo sample is unpacked side by side and mixed down over
    // its left half.
    uint32_t at = 16;
    uint32_t totalFrames = 0;
    for (int i = 0; i < _numSamples; i++) {
        ItSample& s = _samples[i];
        if (!s.inCache && !s.levels) continue;
        uint32_t bpf = s.is16Bit ? 2 : 1;
        bool stereo = s.rightOffset != 0;
        if (s.inCache) {
            at += s.length * bpf * (s.packed && stereo ? 2 : 1);
            totalFrames += s.length * (stereo ? 2 : 1);
        }
        for (int l = 1; l <= s.levels; l++) {
            at += levelMap(s, l).len * bpf;
            totalFrames += levelMap(s, l - 1).len;
        }
    }
    uint32_t dataBytes = at - 16;
    key = fnv(key, (const uint8_t*)&dataBytes, 4);
    (void)path;

    char name[96];
    int pick = -1;
    uint32_t lowestUse = 0xFFFFFFFFu, highestUse = 0;
    int found = -1;
    for (int slot = 0; slot < CACHE_SLOTS; slot++) {
        snprintf(name, sizeof(name), "%s/%d.bin", cacheDir, slot);
        uint8_t hdr[16];
        FsFile f;
        uint32_t use = 0;
        bool valid = false;
        // Older versions (ITC2, ITC3) are replaced first.
        if (f.open(name, O_RDONLY) && f.read(hdr, 16) == 16 && memcmp(hdr, "ITC4", 4) == 0) {
            valid = true;
            use = rd32(hdr + 12);
            // Kept even if fragmented (it reads through SdFat).
            if (rd32(hdr + 4) == key && rd32(hdr + 8) == dataBytes && f.fileSize() == 16 + (uint64_t)dataBytes) found = slot;
        }
        f.close();
        if (use > highestUse) highestUse = use;
        if (!valid) { if (lowestUse != 0) { lowestUse = 0; pick = slot; } }
        else if (use < lowestUse) { lowestUse = use; pick = slot; }
    }

    uint8_t hdr[16];
    memcpy(hdr, "ITC4", 4);
    wr32(hdr + 4, key);
    wr32(hdr + 8, dataBytes);
    wr32(hdr + 12, highestUse + 1);

    if (found >= 0) {
        snprintf(name, sizeof(name), "%s/%d.bin", cacheDir, found);
        FsFile f;
        if (f.open(name, O_RDWR)) { f.write(hdr, 16); f.close(); } // bump the use count
    } else {
        if (pick < 0) pick = 0;
        if (!sd.exists(cacheDir)) sd.mkdir(cacheDir);
        snprintf(name, sizeof(name), "%s/%d.bin", cacheDir, pick);
        FsFile f;
        if (!f.open(name, O_RDWR | O_CREAT | O_TRUNC)) return false;
        f.preAllocate(16 + (uint64_t)dataBytes); // one run of sectors if the card has room for it
        uint8_t blank[16];
        memset(blank, 0, sizeof(blank));
        bool ok = f.write(blank, 16) == 16 && f.sync();
        uint32_t written = 16, doneFrames = 0;
        bool haveTaps = false;
        for (int i = 0; ok && i < _numSamples; i++) {
            ItSample& s = _samples[i];
            if (!s.inCache && !s.levels) continue;
            uint32_t start = written;
            uint32_t bpf = s.is16Bit ? 2 : 1;
            uint32_t bytes = s.length * bpf;
            if (s.inCache && s.packed) {
                bool it215 = (s.dataOffset & 0x80000000u) != 0;
                ok = unpackSample(f, s, s.dataOffset & 0x7FFFFFFFu, it215, written, totalFrames, doneFrames);
                if (ok && s.rightOffset) ok = mixDown(f, start, f, start, start + bytes, s.length, s.is16Bit, true);
            } else if (s.inCache) {
                ok = mixDown(_file, s.dataOffset, f, start, s.rightOffset, s.length, s.is16Bit, s.isSigned);
                written += bytes;
                doneFrames += s.length * 2;
                if (progressCallback && totalFrames > 0) progressCallback((int)((uint64_t)doneFrames * 100 / totalFrames));
            }
            // Each copy is made from the one above it.
            FsFile* src = s.inCache ? &f : &_file;
            uint32_t srcAt = s.inCache ? start : s.dataOffset;
            bool srcSigned = s.inCache || s.isSigned;
            const ItSampleView* w = wrapLoop(s);
            if (s.levels && !haveTaps) { makeHalfTaps(); haveTaps = true; }
            for (int l = 1; ok && l <= s.levels; l++) {
                LevelMap up = levelMap(s, l - 1), m = levelMap(s, l);
                // This step on its own: from level l - 1's frames to level l's.
                LevelMap step = w ? LevelMap{up.aL, m.aL, m.num, up.num, m.len} : LevelMap{0, 0, 1, 2, m.len};
                ok = writeHalf(*src, srcAt, srcSigned, up.len, f, written, s.is16Bit, step, w ? up.aL + up.num : 0,
                               w != nullptr, w && w->pingpong, totalFrames, doneFrames);
                src = &f;
                srcAt = written;
                srcSigned = true;
                written += m.len * bpf;
            }
            if (ok && !f.seekSet(written)) ok = false;
        }
        if (ok) ok = f.seekSet(0) && f.write(hdr, 16) == 16;
        if (ok) f.sync();
        f.close();
        if (!ok) {
            sd.remove(name);
            snprintf(_error, sizeof(_error), "card full or locked, can't unpack samples");
            return false;
        }
        found = pick;
    }

    // Point the cached samples and copies at the cache.
    at = 16;
    for (int i = 0; i < _numSamples; i++) {
        ItSample& s = _samples[i];
        uint32_t bpf = s.is16Bit ? 2 : 1;
        if (s.inCache) {
            s.dataOffset = at;
            at += s.length * bpf * (s.packed && s.rightOffset ? 2 : 1);
            s.isSigned = true;
        }
        s.rightOffset = 0;
        if (s.levels) {
            s.copyOffset = at;
            for (int l = 1; l <= s.levels; l++) at += levelMap(s, l).len * bpf;
        }
    }
    snprintf(name, sizeof(name), "%s/%d.bin", cacheDir, found);
    return _cacheFile.open(name, O_RDONLY);
}

namespace {
// Modified Bessel function I0, for the Kaiser window.
float besselI0(float x) {
    float sum = 1.0f, term = 1.0f;
    for (int k = 1; k < 40; k++) {
        float t = x / (2.0f * (float)k);
        term *= t * t;
        sum += term;
        if (term < sum * 1e-9f) break;
    }
    return sum;
}

// writeHalf()'s filter: 96 taps, rows for 33 fractional positions, in
// the voice slots (free until play) with its buffers after them.
const int HALF_TAPS = 96, HALF_PHASES = 32;
const uint32_t HALF_BLOCK = 4096; // copy frames a pass
const uint32_t HALF_TABLE = (HALF_PHASES + 1) * HALF_TAPS, HALF_WIN = 2 * HALF_BLOCK + 128;
} // namespace

// Q15 rows of a Kaiser windowed sinc (beta 7.3) cut off at a quarter of
// the source rate: flat to 0.225 and 75 dB down from 0.275, so what it
// takes out is above 90% of the copy's own Nyquist. Row p is the filter
// for a copy frame p / 32 of a source frame past the window's middle.
void ItPlayer::makeHalfTaps() {
    int16_t* table = &_chunks[0][0];
    const float fc = 0.25f, beta = 7.3f, i0b = besselI0(beta), reach = HALF_TAPS / 2;
    float* row = (float*)_rawScratch;
    static_assert(HALF_TAPS * sizeof(float) <= sizeof(_rawScratch), "writeHalf's taps don't fit _rawScratch");
    for (int p = 0; p <= HALF_PHASES; p++) {
        float sum = 0;
        for (int n = 0; n < HALF_TAPS; n++) {
            float t = (float)(n - (HALF_TAPS / 2 - 1)) - (float)p / HALF_PHASES;
            float x = 2.0f * fc * t * 3.14159265f;
            float sinc = x == 0 ? 2.0f * fc : 2.0f * fc * sinf(x) / x;
            float r = t / reach;
            row[n] = r * r < 1.0f ? sinc * besselI0(beta * sqrtf(1.0f - r * r)) / i0b : 0.0f;
            sum += row[n];
        }
        int32_t total = 0;
        int16_t* out = table + p * HALF_TAPS;
        for (int n = 0; n < HALF_TAPS; n++) { out[n] = (int16_t)lroundf(row[n] / sum * 32768.0f); total += out[n]; }
        // DC gain exactly 1, keeping the half way row symmetric.
        if (p == HALF_PHASES / 2) {
            out[HALF_TAPS / 2 - 1] = (int16_t)(out[HALF_TAPS / 2 - 1] + (32768 - total) / 2);
            out[HALF_TAPS / 2] = (int16_t)(out[HALF_TAPS / 2] + (32768 - total) / 2);
        } else {
            out[HALF_TAPS / 2] = (int16_t)(out[HALF_TAPS / 2] + (32768 - total));
        }
    }
}

// Writes the next level's copy of srcFrames frames at srcAt in src (signed
// or not, 8 or 16 bit) to dst at dstAt as signed PCM of the same width:
// m.len frames, copy frame j taken at source frame
// m.a - 0.5 + (j - m.aL + 0.5) * m.den / m.num through makeHalfTaps()'s
// filter. Frames inside the loop (copy frames m.aL.. m.num of them) filter
// across the loop's turn the way it plays, so the copy's loop is seamless;
// the rest filter the plain data.
bool ItPlayer::writeHalf(FsFile& src, uint32_t srcAt, bool srcSigned, uint32_t srcFrames, FsFile& dst, uint32_t dstAt, bool is16,
                         const LevelMap& m, uint32_t e, bool loop, bool pingpong, uint32_t totalFrames, uint32_t& doneFrames) {
    const int16_t* table = &_chunks[0][0];
    int16_t* taps = &_chunks[0][0] + HALF_TABLE;
    int16_t* win = taps + HALF_TAPS;
    uint8_t* out = (uint8_t*)(win + HALF_WIN);
    static_assert((HALF_TABLE + HALF_TAPS + HALF_WIN + HALF_BLOCK) * 2 <= sizeof(_chunks), "writeHalf's buffers don't fit the voice slots");
    const uint32_t a = m.a, bpf = is16 ? 2 : 1;

    // count source frames from `from`, decoded into dest, reversed if asked.
    auto readRun = [&](int16_t* dest, uint32_t from, uint32_t count, bool reverse) -> bool {
        if (count == 0) return true;
        uint8_t* raw = (uint8_t*)dest;
        if (!src.seekSet(srcAt + from * bpf) || src.read(raw, count * bpf) != (int)(count * bpf)) return false;
        if (is16) {
            if (!srcSigned) for (uint32_t k = 0; k < count; k++) dest[k] = (int16_t)((uint16_t)dest[k] ^ 0x8000);
        } else {
            // In place from the top, each int16 lands at or past its byte.
            for (uint32_t k = count; k-- > 0;) dest[k] = (int16_t)((int16_t)(int8_t)(raw[k] ^ (srcSigned ? 0 : 0x80)) << 8);
        }
        if (reverse) for (uint32_t i = 0, j = count - 1; i < j; i++, j--) { int16_t t = dest[i]; dest[i] = dest[j]; dest[j] = t; }
        return true;
    };
    // Source frames lo..hi-1 into win: across the loop's turn (and, for a
    // loop from 0, before the start) if wrap, else plain with zeros past
    // the ends.
    auto fill = [&](int32_t lo, int32_t hi, bool wrap) -> bool {
        int16_t* d = win;
        int32_t lim = wrap ? (int32_t)e : (int32_t)srcFrames;
        int32_t negEnd = hi < 0 ? hi : 0;
        if (lo < negEnd) {
            uint32_t n = (uint32_t)(negEnd - lo);
            if (wrap && a == 0) {
                bool ok = pingpong ? readRun(d, (uint32_t)(-negEnd), n, true) : readRun(d, (uint32_t)((int32_t)e + lo), n, false);
                if (!ok) return false;
            } else {
                memset(d, 0, n * 2);
            }
            d += n;
        }
        int32_t from = lo > 0 ? lo : 0, to = hi < lim ? hi : lim;
        if (from < to) {
            if (!readRun(d, (uint32_t)from, (uint32_t)(to - from), false)) return false;
            d += to - from;
        }
        if (from < lim) from = lim;
        if (from < hi) {
            uint32_t n = (uint32_t)(hi - from);
            if (wrap) {
                bool ok = pingpong ? readRun(d, 2 * e - (uint32_t)hi, n, true) : readRun(d, a + ((uint32_t)from - e), n, false);
                if (!ok) return false;
            } else {
                memset(d, 0, n * 2);
            }
        }
        return true;
    };
    // Copy frame j sits at source frame T(j) / (2 * num), whole part ip and
    // fraction (16 bits) frac.
    const int64_t num2 = 2 * (int64_t)m.num;
    auto centre = [&](uint32_t j, int32_t& ip, uint32_t& frac) {
        int64_t t = (2 * (int64_t)a - 1) * m.num + (2 * ((int64_t)j - m.aL) + 1) * (int64_t)m.den;
        int64_t q = t >= 0 ? t / num2 : -((-t + num2 - 1) / num2);
        ip = (int32_t)q;
        frac = (uint32_t)(((t - q * num2) << 16) / num2);
    };

    uint32_t outFrames = m.len;
    uint32_t jA = loop ? m.aL : 0, jE = loop ? m.aL + m.num : 0;
    if (jE > outFrames) jE = outFrames;
    if (jA > jE) jA = jE;
    uint32_t pct = 0xFFFFFFFFu, done0 = doneFrames, lastFrac = 0xFFFFFFFFu;
    for (uint32_t j0 = 0; j0 < outFrames;) {
        bool wrap = loop && j0 >= jA && j0 < jE;
        uint32_t segEnd = !loop ? outFrames : j0 < jA ? jA : j0 < jE ? jE : outFrames;
        uint32_t n = segEnd - j0 < HALF_BLOCK ? segEnd - j0 : HALF_BLOCK;
        int32_t ip0, ip1;
        uint32_t frac;
        centre(j0, ip0, frac);
        centre(j0 + n - 1, ip1, frac);
        int32_t lo = ip0 - (HALF_TAPS / 2 - 1), hi = ip1 + HALF_TAPS / 2 + 1;
        if (hi - lo > (int32_t)HALF_WIN) return false; // can't happen: num / den is at least a half
        if (!fill(lo, hi, wrap)) return false;
        for (uint32_t k = 0; k < n; k++) {
            int32_t ip;
            centre(j0 + k, ip, frac);
            if (frac != lastFrac) {
                // Taps for this fraction, between two of the table's rows.
                lastFrac = frac;
                uint32_t p = frac >> 11, w = frac & 2047;
                const int16_t* r0 = table + p * HALF_TAPS;
                for (int t = 0; t < HALF_TAPS; t++) taps[t] = (int16_t)(r0[t] + (((r0[t + HALF_TAPS] - r0[t]) * (int32_t)w) >> 11));
            }
            const int16_t* x = win + (ip - lo) - (HALF_TAPS / 2 - 1);
            int32_t acc = 0;
            if (frac == 0x8000) {
                // Half way between two source frames (always so for an even loop or none): the taps are symmetric.
                for (int t = 0; t < HALF_TAPS / 2; t++) acc += (int32_t)taps[t] * ((int32_t)x[t] + x[HALF_TAPS - 1 - t]);
            } else {
                for (int t = 0; t < HALF_TAPS; t++) acc += (int32_t)taps[t] * x[t];
            }
            int32_t y = (acc + 16384) >> 15;
            if (is16) {
                if (y > 32767) y = 32767;
                if (y < -32768) y = -32768;
                out[k * 2] = (uint8_t)y;
                out[k * 2 + 1] = (uint8_t)((uint32_t)y >> 8);
            } else {
                y = (y + 128) >> 8;
                if (y > 127) y = 127;
                if (y < -128) y = -128;
                out[k] = (uint8_t)(int8_t)y;
            }
        }
        if (!dst.seekSet(dstAt + j0 * bpf) || dst.write(out, n * bpf) != n * bpf) return false;
        j0 += n;
        doneFrames = done0 + (uint32_t)((uint64_t)j0 * srcFrames / outFrames);
        if (progressCallback && totalFrames > 0) {
            uint32_t pc = (uint32_t)((uint64_t)doneFrames * 100 / totalFrames);
            if (pc != pct) { pct = pc; progressCallback((int)pc); }
        }
    }
    doneFrames = done0 + srcFrames;
    return true;
}

// Writes the mono mix of `frames` frames (left at srcL, right at srcR in
// src) to dst at dstAt. src and dst may be the same file.
bool ItPlayer::mixDown(FsFile& src, uint32_t srcL, FsFile& dst, uint32_t dstAt, uint32_t srcR, uint32_t frames,
                       bool is16, bool isSigned) {
    uint32_t bpf = is16 ? 2 : 1;
    const uint32_t STEP = 256; // frames a pass: left and right halves of _rawScratch
    uint8_t* l = _rawScratch;
    uint8_t* r = _rawScratch + 512;
    for (uint32_t done = 0; done < frames; done += STEP) {
        uint32_t n = frames - done < STEP ? frames - done : STEP;
        uint32_t len = n * bpf;
        if (!src.seekSet(srcL + done * bpf) || src.read(l, len) != (int)len) return false;
        if (!src.seekSet(srcR + done * bpf) || src.read(r, len) != (int)len) return false;
        for (uint32_t k = 0; k < n; k++) {
            if (is16) {
                uint16_t a = (uint16_t)(l[k * 2] | (l[k * 2 + 1] << 8)), b = (uint16_t)(r[k * 2] | (r[k * 2 + 1] << 8));
                if (!isSigned) { a ^= 0x8000; b ^= 0x8000; }
                int16_t m = (int16_t)(((int32_t)(int16_t)a + (int16_t)b) >> 1);
                l[k * 2] = (uint8_t)m;
                l[k * 2 + 1] = (uint8_t)((uint16_t)m >> 8);
            } else {
                uint8_t a = l[k], b = r[k];
                if (!isSigned) { a ^= 0x80; b ^= 0x80; }
                l[k] = (uint8_t)(int8_t)(((int)(int8_t)a + (int8_t)b) >> 1);
            }
        }
        if (!dst.seekSet(dstAt + done * bpf) || dst.write(l, len) != len) return false;
    }
    return true;
}

// Unpacks one IT 2.14/2.15 compressed sample (both channels for stereo)
// and appends it to the cache as signed PCM.
bool ItPlayer::unpackSample(FsFile& cache, ItSample& s, uint32_t srcOffset, bool it215, uint32_t& written,
                            uint32_t totalFrames, uint32_t& doneFrames) {
    int sides = s.rightOffset ? 2 : 1;
    bool is16 = s.is16Bit;
    uint8_t* outBuf = _rawScratch + 512; // first half is the bit reader's buffer
    uint32_t blockAt = srcOffset;
    int lastPct = -1;
    for (int side = 0; side < sides; side++) {
        uint32_t left = s.length;
        while (left > 0) {
            uint8_t lb[2];
            bool haveBlock = _file.seekSet(blockAt) && _file.read(lb, 2) == 2;
            uint32_t blockLen = haveBlock ? rd16(lb) : 0;
            uint32_t blockFrames = is16 ? 0x4000 : 0x8000;
            if (blockFrames > left) blockFrames = left;
            BitReader br{&_file, _rawScratch, 512};
            br.bytesLeft = blockLen;
            br.ok = haveBlock;
            int width = is16 ? 17 : 9;
            int32_t d1 = 0, d2 = 0;
            int outLen = 0;
            for (uint32_t n = 0; n < blockFrames;) {
                int32_t out = 0;
                if (br.ok) {
                    uint32_t v = br.read(width);
                    if (!br.ok) v = 0;
                    if (width < 7) {
                        if (v == (1u << (width - 1))) {
                            uint32_t w = br.read(is16 ? 4 : 3) + 1;
                            width = (int)(w < (uint32_t)width ? w : w + 1);
                            continue;
                        }
                    } else if (width < (is16 ? 17 : 9)) {
                        uint32_t border = (is16 ? (0xFFFFu >> (17 - width)) - 8 : (0xFFu >> (9 - width)) - 4);
                        if (v > border && v <= border + (is16 ? 16u : 8u)) {
                            uint32_t w = v - border;
                            width = (int)(w < (uint32_t)width ? w : w + 1);
                            continue;
                        }
                    } else if (width == (is16 ? 17 : 9)) {
                        if (v & (is16 ? 0x10000u : 0x100u)) {
                            width = (int)((v + 1) & 0xFF);
                            if (width < 1 || width > (is16 ? 17 : 9)) br.ok = false; // bad width, the rest of the block is silence
                            continue;
                        }
                    } else {
                        br.ok = false; // bad width, the rest of the block is silence
                        v = 0;
                    }
                    int32_t sv;
                    if (is16) {
                        sv = (width < 16) ? ((int32_t)(v << (32 - width)) >> (32 - width)) : (int16_t)v;
                        d1 = (int16_t)(d1 + sv);
                        d2 = (int16_t)(d2 + d1);
                    } else {
                        sv = (width < 8) ? ((int32_t)(v << (32 - width)) >> (32 - width)) : (int8_t)v;
                        d1 = (int8_t)(d1 + sv);
                        d2 = (int8_t)(d2 + d1);
                    }
                    out = it215 ? d2 : d1;
                    if (!br.ok) out = 0;
                }
                if (is16) { outBuf[outLen++] = (uint8_t)out; outBuf[outLen++] = (uint8_t)(out >> 8); }
                else outBuf[outLen++] = (uint8_t)out;
                n++;
                if (outLen >= 512) {
                    if (cache.write(outBuf, outLen) != (size_t)outLen) return false;
                    written += outLen;
                    outLen = 0;
                }
            }
            if (outLen > 0) {
                if (cache.write(outBuf, outLen) != (size_t)outLen) return false;
                written += outLen;
            }
            blockAt += 2 + blockLen;
            left -= blockFrames;
            doneFrames += blockFrames;
            if (progressCallback && totalFrames > 0) {
                int pct = (int)((uint64_t)doneFrames * 100 / totalFrames);
                if (pct != lastPct) { lastPct = pct; progressCallback(pct); }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- streaming

namespace {
// Decodes the frames in `got` raw bytes into dest as int16.
inline __attribute__((always_inline)) int decodeFrames(const uint8_t* raw, int got, bool is16, bool isSigned, int16_t* dest) {
    uint32_t bpf = is16 ? 2 : 1;
    if (got <= 0) return 0;
    uint32_t frames = (uint32_t)got / bpf;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v;
        if (is16) {
            uint16_t u = (uint16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8));
            if (!isSigned) u ^= 0x8000;
            v = (int16_t)u;
        } else {
            uint8_t u = raw[i];
            if (!isSigned) u ^= 0x80;
            v = (int16_t)((int16_t)(int8_t)u << 8);
        }
        dest[i] = v;
    }
    return (int)frames;
}
} // namespace

// n frames from frame `start`, read in pieces through _rawScratch.
int __not_in_flash_func(ItPlayer::readFrames)(FsFile& f, uint32_t offset, uint32_t start, uint32_t n, bool is16, bool isSigned, int16_t* dest) {
    uint32_t bpf = is16 ? 2 : 1, done = 0;
    while (done < n) {
        uint32_t at = offset + (start + done) * bpf;
        uint32_t piece = (n - done) * bpf;
        if (piece > rawSpan(at)) piece = rawSpan(at) & ~(bpf - 1);
        int got = 0;
        const uint8_t* raw = readSampleBytes(f, at, piece, got);
        int frames = decodeFrames(raw, got, is16, isSigned, dest + done);
        done += (uint32_t)frames;
        if ((uint32_t)frames * bpf < piece) break;
    }
    return (int)done;
}

// A refill fills the voice's slot and the free slots right above it (up to
// MAX_RUN in all), so a voice with free neighbours reads less often. A
// borrowed slot's own cache is dropped; claimSlot() shortens a borrower's
// cache when the slot's voice starts.
int16_t __not_in_flash_func(ItPlayer::refillChunk)(TrackerVoice& tv, uint32_t index) {
    const ItSampleView& view = static_cast<const ItSampleView&>(*tv.smp);
    const ItSample& s = _samples[view.sampleIndex];
    uint32_t limit;
    const uint32_t realLoopEnd = view.realLoopEnd();
    const uint32_t dataFrames = view.level ? view.length : s.length; // a copy's view only runs past it for a ping-pong loop
    bool mirrored = view.pingpong && index >= realLoopEnd;
    if (mirrored) limit = view.loopEnd;
    else if (tv.looping && view.loopEnd > view.loopStart && index < realLoopEnd) limit = realLoopEnd;
    else limit = dataFrames;
    if (index >= limit) { tv.chunkLen = 0; return 0; }
    int k = (int)(static_cast<ItVoice*>(&tv) - &_voices[0]);
    int run = 1;
    while (run < MAX_RUN && k + run < MAX_VOICES && _voices[k + run].channel == -1) {
        _voices[k + run].chunkSmp = nullptr;
        run++;
    }
    uint32_t frames = limit - index;
    if (frames > (uint32_t)(CHUNK_FRAMES * run)) frames = (uint32_t)(CHUNK_FRAMES * run);
    uint32_t realStart = mirrored ? (2 * realLoopEnd - index - frames) : index;
    FsFile& f = s.inCache || view.level ? _cacheFile : _file;
#ifdef TRACKER_PROFILE
    uint32_t reads0 = profile.reads;
    struct Attribute {
        ItVoice& v; uint32_t r0;
        ~Attribute() {
            uint32_t d = profile.reads - r0;
            if (v.background) profile.readsBg += d;
            if ((v.background ? v.loudness << 1 : v.loudness) < 16) profile.readsQ48 += d; // a under 16, see processVoices()
            if (v.sampleStep16 > (1u << 16)) profile.readsUp1 += d;
            if (v.sampleStep16 > (2u << 16)) profile.readsUp2 += d;
            profileSampleReads[v.sampleIndex] += d;
        }
    } attribute{static_cast<ItVoice&>(tv), reads0};
#endif
    // Every sample is mono once the cache is open, see openCache().
    uint32_t at = s.dataOffset;
    bool isSigned = s.isSigned;
    if (view.level) {
        at = view.fileOffset;
        isSigned = true;
    }
    int got = readFrames(f, at, realStart, frames, s.is16Bit, isSigned, tv.chunkBuf);
    if (got <= 0) { tv.chunkLen = 0; return 0; }
    if (mirrored) {
        // Read forwards, played backwards. A short read keeps the frames
        // nearest the turn.
        int16_t* b = tv.chunkBuf;
        if ((uint32_t)got < frames) { tv.chunkLen = 0; return 0; }
        for (int i = 0, j = got - 1; i < j; i++, j--) { int16_t t = b[i]; b[i] = b[j]; b[j] = t; }
    }
    tv.chunkStart = index;
    tv.chunkLen = (uint32_t)got;
    tv.chunkSmp = tv.smp;
    return tv.chunkBuf[0];
}

// ---------------------------------------------------------------- patterns

bool ItPlayer::patByte(uint8_t& b) {
    if (_patBufPos >= _patBufLen) {
        int n = _patternFile.read(_patBuf, sizeof(_patBuf));
        if (n <= 0) return false;
        _patBufLen = n;
        _patBufPos = 0;
    }
    b = _patBuf[_patBufPos++];
    return true;
}

void ItPlayer::openPattern(int num) {
    _openPattern = num;
    _patStart = (num >= 0 && num < MAX_PATTERNS) ? _patternOffsets[num] : 0;
    _patRowCount = (num >= 0 && num < MAX_PATTERNS) ? _patternRows[num] : 64;
    _patRow = 0;
    _patBufLen = _patBufPos = 0;
    memset(_lastMask, 0, sizeof(_lastMask));
    memset(_lastNote, 0, sizeof(_lastNote));
    memset(_lastInst, 0, sizeof(_lastInst));
    memset(_lastVol, 0, sizeof(_lastVol));
    memset(_lastCmd, 0, sizeof(_lastCmd));
    memset(_lastParam, 0, sizeof(_lastParam));
    if (_patStart) _patternFile.seekSet(_patStart);
}

void ItPlayer::readRowTo(int row) {
    if (row < _patRow) openPattern(_openPattern);
    while (_patRow < row) readRow(false);
    readRow(true);
}

void ItPlayer::readRow(bool apply) {
    _patRow++;
    if (_patStart == 0) return;
    for (;;) {
        uint8_t cv;
        if (!patByte(cv) || cv == 0) return;
        int ch = (cv - 1) & 63;
        uint8_t mask;
        if (cv & 0x80) { if (!patByte(mask)) return; _lastMask[ch] = mask; }
        else mask = _lastMask[ch];
        uint8_t rawNote = 0, inst = 0, vol = 0xFF, cmd = 0, param = 0;
        bool hasNote = false;
        if (mask & 1) { if (!patByte(rawNote)) return; _lastNote[ch] = rawNote; hasNote = true; }
        if (mask & 2) { if (!patByte(inst)) return; _lastInst[ch] = inst; }
        if (mask & 4) { if (!patByte(vol)) return; _lastVol[ch] = vol; }
        if (mask & 8) { if (!patByte(cmd) || !patByte(param)) return; _lastCmd[ch] = cmd; _lastParam[ch] = param; }
        if (mask & 16) { rawNote = _lastNote[ch]; hasNote = true; }
        if (mask & 32) inst = _lastInst[ch];
        if (mask & 64) vol = _lastVol[ch];
        if (mask & 128) { cmd = _lastCmd[ch]; param = _lastParam[ch]; }
        if (apply) {
            if (ch < _numChannels) {
                uint8_t uiNote = !hasNote ? UiTap::NOTE_NONE
                               : rawNote < 120 ? (uint8_t)(rawNote + 1)
                               : rawNote == 255 ? UiTap::NOTE_OFF
                               : rawNote == 254 ? UiTap::NOTE_CUT : UiTap::NOTE_FADE;
                UiTap::cell(ch, uiNote, inst);
            }
            handleCell(ch, hasNote ? rawNote : NO_NOTE, inst, vol, cmd, param);
        }
    }
}

// ---------------------------------------------------------------- pitch

int32_t ItPlayer::noteToPeriod(int note, uint32_t c5) const {
    if (_linear) return (120 - note) * 64;
    // Equal temperament from C-5 at the C5 speed, as OpenMPT plays IT.
    float p = PERIOD_K * AMIGA_FRACT / ((float)c5 * powf(2.0f, (float)(note - 60) / 12.0f));
    if (p < 1.0f) p = 1.0f;
    if (p > (float)MAX_PERIOD) p = (float)MAX_PERIOD;
    return (int32_t)(p + 0.5f);
}

float ItPlayer::periodToHz(int32_t period, uint32_t c5) const {
    if (_linear) return (float)c5 * powf(2.0f, (float)(3840 - period) / 768.0f);
    if (period < 1) period = 1;
    return PERIOD_K * AMIGA_FRACT / (float)period;
}

void ItPlayer::slidePitch(ItChannel& c, int32_t amount) {
    if (c.period == 0) return;
    int32_t p = c.period - (_linear ? amount : amount * (int32_t)AMIGA_FRACT);
    if (_linear) {
        if (p < -7680) p = -7680;
        if (p > 15360) p = 15360;
    } else {
        if (p < 1) p = 1;
        if (p > MAX_PERIOD) p = MAX_PERIOD;
    }
    c.period = p;
}

// ---------------------------------------------------------------- voices

// Free voices in 5-bit bit-reversed order, so the voices in use sit apart
// and each has free slots above it for its cache.
static const uint8_t SPREAD[32] = {0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
                                   1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31};
static_assert(ItPlayer::MAX_VOICES == 32, "SPREAD is for 32 voices");

// Voice k is starting: a lower voice's cache that ran into k's slot ends
// where the slot starts.
void ItPlayer::claimSlot(int k) {
    for (int i = k - (MAX_RUN - 1); i < k; i++) {
        if (i < 0) continue;
        ItVoice& v = _voices[i];
        if (v.chunkBuf + v.chunkLen > _voices[k].chunkBuf) v.chunkLen = (uint32_t)(_voices[k].chunkBuf - v.chunkBuf);
    }
}

// ---------------------------------------------------------------- half and quarter rate copies
//
// A voice stepping 2 or more source frames a frame reads the sample's half
// rate copy, at 4 or more the quarter rate one, so it reads half or a
// quarter of the bytes off the card. The copies are low-pass filtered
// before they're decimated (see writeHalf()), so they only lose what the
// mixer would alias at that pitch anyway. Each level halves the one above
// it, rounding the loop it is filtered around (wrapLoop()) up to whole
// frames: the copy's rate is num / den of the sample's, a hair off half
// for an odd loop, so the loop stays seamless and in tune.

ItPlayer::LevelMap ItPlayer::levelMap(const ItSample& s, int level) {
    const ItSampleView* w = wrapLoop(s);
    if (!w) return LevelMap{0, 0, 1, 1u << level, s.length >> level};
    uint32_t a = w->loopStart, loopLen = w->realLoopEnd() - a;
    uint32_t aL = a, num = loopLen, len = s.length;
    for (int k = 0; k < level; k++) {
        uint32_t half = (num + 1) >> 1;
        uint32_t aNext = (uint32_t)((uint64_t)aL * half / num);
        len = len > aL ? aNext + (uint32_t)((uint64_t)(len - aL) * half / num) : aNext;
        aL = aNext;
        num = half;
    }
    return LevelMap{a, aL, num, loopLen, len};
}

// x * n / d rounded down, without overflow for |x| < 2^47 and n, d < 2^26.
static int64_t mulDivFloor(int64_t x, uint32_t n, uint32_t d) {
    int64_t q = x / d, r = x % d;
    if (r < 0) { r += d; q--; }
    return q * n + r * (int64_t)n / d;
}

// Positions in 1/65536 frames, rounded down.
int64_t ItPlayer::toCopy(const LevelMap& m, int64_t x) {
    return ((int64_t)m.aL << 16) - 32768 + mulDivFloor(x - ((int64_t)m.a << 16) + 32768, m.num, m.den);
}
int64_t ItPlayer::toSample(const LevelMap& m, int64_t q) {
    return ((int64_t)m.a << 16) - 32768 + mulDivFloor(q - ((int64_t)m.aL << 16) + 32768, m.den, m.num);
}

// A view of a loop plays from a copy if its loop lands on whole copy
// frames (always so for the loop the copies are made around), or is long
// enough that rounding it doesn't matter.
bool ItPlayer::levelFits(const ItSample& s, const ItSampleView& base, int level) {
    if (level == 0) return true;
    if (level > s.levels) return false;
    if (base.loopEnd <= base.loopStart) return true;
    const ItSampleView* w = wrapLoop(s);
    if (w && w->loopStart == base.loopStart && w->realLoopEnd() == base.realLoopEnd()) return true;
    LevelMap m = levelMap(s, level);
    int64_t da = ((int64_t)base.loopStart - m.a) * m.num, de = ((int64_t)base.realLoopEnd() - m.a) * m.num;
    if (m.aL + da / (int64_t)m.den < 0) return false;
    if ((de - da) / (int64_t)m.den < 2) return false;
    return (da % (int64_t)m.den == 0 && de % (int64_t)m.den == 0) || (de - da) / (int64_t)m.den >= 8192;
}

// The view of sample si's normal or sustain loop at `level`, made in
// _levelViews the first time; nullptr if the copy can't play it or every
// view is in use.
const ItPlayer::ItSampleView* ItPlayer::levelView(int si, bool sustainView, int level) {
    const ItSample& s = _samples[si];
    const ItSampleView& base = sustainView ? s.sustain : s.normal;
    if (level == 0) return &base;
    if (!levelFits(s, base, level)) return nullptr;
    for (ItSampleView& lv : _levelViews) {
        if (lv.level == level && lv.sampleIndex == si && lv.sustainView == sustainView) return &lv;
    }
    ItSampleView* pick = nullptr;
    for (ItSampleView& lv : _levelViews) {
        if (lv.level == 0) { pick = &lv; break; }
    }
    for (int k = 0; !pick && k < LEVEL_VIEWS; k++) {
        bool used = false;
        for (const ItVoice& v : _voices) {
            if (v.channel != -1 && v.smp == &_levelViews[k]) { used = true; break; }
        }
        if (!used) pick = &_levelViews[k];
    }
    if (!pick) return nullptr;
    for (ItVoice& v : _voices) {
        if (v.chunkSmp == pick) v.chunkSmp = nullptr;
        if (v.smp == pick) { v.smp = nullptr; v.havePair = false; } // only free voices are left on it
    }
    LevelMap m = levelMap(s, level);
    ItSampleView& lv = *pick;
    lv = ItSampleView();
    lv.sampleIndex = (uint8_t)si;
    lv.pingpong = base.pingpong;
    lv.level = (uint8_t)level;
    lv.sustainView = sustainView;
    lv.length = m.len;
    lv.fileOffset = s.copyOffset + (level == 2 ? levelMap(s, 1).len * (s.is16Bit ? 2 : 1) : 0);
    if (base.loopEnd > base.loopStart) {
        // Nearest whole frames, exact for the loop the copies are made around.
        auto at = [&](uint32_t x) {
            int64_t t = ((int64_t)x - m.a) * m.num + m.den / 2, d = m.den;
            return (uint32_t)((int64_t)m.aL + (t >= 0 ? t / d : -((-t + d - 1) / d)));
        };
        uint32_t a = at(base.loopStart), e = at(base.realLoopEnd());
        lv.loopStart = a;
        lv.loopEnd = base.pingpong ? 2 * e - a : e;
        if (lv.length < lv.loopEnd) lv.length = lv.loopEnd;
    }
    return &lv;
}

// Moves v to another view of the same sample at the same point of the
// sound, keeping the fraction. With keepDirection a voice in the backwards
// half of a ping-pong loop stays in it.
void ItPlayer::moveVoice(ItVoice& v, const ItSampleView* to, bool keepDirection) {
    const ItSampleView& from = *static_cast<const ItSampleView*>(v.smp);
    const ItSample& s = _samples[from.sampleIndex];
    // Positions here are in 1/65536 frames.
    int64_t p = ((int64_t)v.srcIndex << 16) | v.samplePos16;
    uint32_t endF = from.realLoopEnd();
    bool mirrored = from.pingpong && v.srcIndex >= endF;
    if (mirrored) p = ((int64_t)(2 * endF - 1) << 16) - p; // the frame it reads, playing backwards
    int64_t q = toCopy(levelMap(s, to->level), toSample(levelMap(s, from.level), p));
    if (q < 0) q = 0;
    uint32_t endT = to->realLoopEnd();
    int64_t endQ = (int64_t)endT << 16;
    if (to->pingpong && keepDirection && mirrored) {
        q = ((int64_t)(2 * endT - 1) << 16) - q;
        if (q < endQ) q = endQ;
    } else if (to->pingpong && endT > 0 && q >= endQ) {
        q = endQ - 1;
    }
    int64_t loopEndQ = (int64_t)to->loopEnd << 16;
    if (v.looping && to->loopEnd > to->loopStart && q >= loopEndQ) {
        q = ((int64_t)to->loopStart << 16) + (q - loopEndQ) % ((int64_t)(to->loopEnd - to->loopStart) << 16);
    }
    v.smp = to;
    v.lastHz = -1; // the step depends on the level
    uint32_t idx = (uint32_t)(q >> 16);
    if (!v.looping && idx >= to->length) { v.havePair = false; return; }
    startVoiceAt(v, idx);
    v.samplePos16 = (uint32_t)(q & 0xFFFF);
}

// startVoiceAt() for a frame of the sample, on whichever copy v reads.
void ItPlayer::startVoiceAtSampleFrame(ItVoice& v, uint32_t frame) {
    const ItSampleView& view = *static_cast<const ItSampleView*>(v.smp);
    if (view.level == 0) { startVoiceAt(v, frame); return; }
    int64_t q = toCopy(levelMap(_samples[view.sampleIndex], view.level), (int64_t)frame << 16);
    if (q < 0) q = 0;
    uint32_t end = view.pingpong ? view.realLoopEnd() : view.length;
    uint32_t idx = (uint32_t)(q >> 16);
    uint32_t frac = (uint32_t)(q & 0xFFFF);
    if (idx >= end) { idx = end ? end - 1 : 0; frac = 0; }
    startVoiceAt(v, idx);
    v.samplePos16 = frac;
}

int ItPlayer::allocVoice(int forChannel) {
    for (int n = 0; n < MAX_VOICES; n++) {
        int i = SPREAD[n];
        if (_voices[i].channel == -1) { claimSlot(i); return i; }
    }
    // Then the cut note's tail nearest the end of its fade.
    int tail = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (_voices[i].channel < 0 && (tail < 0 || _voices[i].rampLeft < _voices[tail].rampLeft)) tail = i;
    }
    if (tail >= 0) { releaseVoice(_voices[tail]); return tail; }
    // Steal the quietest background voice, else the quietest other channel's.
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        uint16_t lowest = 0xFFFF;
        for (int i = 0; i < MAX_VOICES; i++) {
            const ItVoice& v = _voices[i];
            if (pass == 0 && !v.background) continue;
            if (pass == 1 && v.channel == forChannel) continue;
            if (v.loudness < lowest || best < 0) { lowest = v.loudness; best = i; }
        }
    }
    if (best < 0) best = 0;
    releaseVoice(_voices[best]);
    return best;
}

void ItPlayer::releaseVoice(ItVoice& v) {
    int idx = (int)(&v - &_voices[0]);
    if (v.channel >= 0 && v.channel < MAX_CHANNELS && _channels[v.channel].voice == idx) _channels[v.channel].voice = -1;
    v.channel = -1;
    v.background = false;
    v.havePair = false;
    v.loudness = 0;
    v.fadeOut = false;
    v.rampLeft = 0;
    v.curL = v.curR = 0;
}

void ItPlayer::cutVoice(ItVoice& v) {
    if (v.havePair && v.smp && v.sampleStep16 && (v.curL || v.curR || v.rampLeft)) {
        for (int i = 0; i < MAX_VOICES; i++) {
            ItVoice& g = _voices[i];
            if (g.channel != -1 || &g == &v) continue;
            // The tail keeps v's position and the cached frames from there on
            // that fit its slot, so it reads no SD.
            claimSlot(i);
            int16_t* buf = g.chunkBuf;
            uint32_t off = v.srcIndex - v.chunkStart;
            if (off >= v.chunkLen) off = 0;
            uint32_t len = v.chunkLen - off;
            if (len > (uint32_t)CHUNK_FRAMES) len = CHUNK_FRAMES;
            g = v;
            g.chunkBuf = buf;
            g.chunkStart = v.chunkStart + off;
            g.chunkLen = len;
            memcpy(buf, v.chunkBuf + off, len * sizeof(int16_t));
            g.channel = -2;
            g.background = true;
            g.fadeOut = true;
            g.loudness = 0;
            g.stepL = -g.curL / RAMP_DOWN;
            g.stepR = -g.curR / RAMP_DOWN;
            g.rampLeft = RAMP_DOWN;
            break;
        }
    }
    releaseVoice(v);
}

void ItPlayer::voiceNoteOff(ItVoice& v) {
    if (v.channel < 0) return;
    const ItSample& s = _samples[v.sampleIndex];
    const ItSampleView* old = static_cast<const ItSampleView*>(v.smp);
    if (!v.keyOff && old && old->sustainView) {
        // Leave the sustain loop: carry on from the same frame of the sample
        // (or copy), else from the same point of the sample.
        const ItSampleView* to = levelView(v.sampleIndex, false, old->level);
        v.looping = s.normal.loopEnd > s.normal.loopStart;
        if (to) {
            uint32_t pos = v.srcIndex;
            uint32_t end = old->realLoopEnd();
            if (old->pingpong && pos >= end) pos = 2 * end - 1 - pos;
            v.smp = to;
            v.srcIndex = pos;
            if (!v.looping && pos >= to->length) v.havePair = false;
        } else {
            moveVoice(v, &s.normal, false);
        }
    }
    v.keyOff = true;
    if (_instrumentMode && v.instrument >= 1 && v.instrument <= MAX_INSTRUMENTS) {
        const ItInstrument& in = _instruments[v.instrument - 1];
        if (!v.volEnvOn || in.volEnv.numPoints == 0) v.fading = true;
        else if ((in.volEnv.flags & 2) && in.fadeout) v.fading = true;
    }
}

// Duplicate check and new note action for channel ch's sounding voices,
// before a new note starts.
void ItPlayer::applyNna(int ch, uint8_t newInst, uint8_t newSample, uint8_t newNote) {
    ItChannel& c = _channels[ch];
    if (!_instrumentMode) {
        return; // the foreground voice is reused (cut)
    }
    if (newInst >= 1 && newInst <= MAX_INSTRUMENTS) {
        const ItInstrument& in = _instruments[newInst - 1];
        if (in.dct) {
            for (int i = 0; i < MAX_VOICES; i++) {
                ItVoice& v = _voices[i];
                if (v.channel != ch || v.instrument != newInst) continue;
                bool dup = (in.dct == 1 && v.note == newNote) || (in.dct == 2 && v.sampleIndex == newSample) || in.dct == 3;
                if (!dup) continue;
                if (in.dca == 0) cutVoice(v);
                else if (in.dca == 1) voiceNoteOff(v);
                else v.fading = true;
            }
        }
    }
    if (c.voice < 0) return;
    ItVoice& v = _voices[c.voice];
    uint8_t nna = c.nnaOverride != 0xFF ? c.nnaOverride
                  : (v.instrument >= 1 && v.instrument <= MAX_INSTRUMENTS ? _instruments[v.instrument - 1].nna : 0);
    if (nna == 0) return; // cut: the voice is reused
    if (v.mixVolume == 0 && v.loudness == 0) return; // silent already, reuse it
    v.background = true;
    c.voice = -1;
    if (nna == 2) voiceNoteOff(v);
    else if (nna == 3) v.fading = true;
}

void ItPlayer::triggerNote(int ch, uint8_t note, uint8_t instArg, bool tonePorta) {
    ItChannel& c = _channels[ch];
    uint8_t sampleNum, realNote;
    if (_instrumentMode) {
        if (c.instrument < 1 || c.instrument > MAX_INSTRUMENTS) return;
        const ItInstrument& in = _instruments[c.instrument - 1];
        sampleNum = in.sampleMap[note];
        realNote = in.noteMap[note];
    } else {
        sampleNum = c.instrument;
        realNote = note;
    }
    if (sampleNum < 1 || sampleNum > _numSamples) return;
    uint8_t si = sampleNum - 1;
    const ItSample& s = _samples[si];
    if (s.length == 0) return;

    if (tonePorta && c.voice >= 0 && c.period != 0) {
        c.portaTarget = noteToPeriod(realNote, _samples[_voices[c.voice].sampleIndex].c5speed);
        c.note = note;
        return;
    }

    applyNna(ch, c.instrument, si, note);
    if (c.voice >= 0) {
        // NNA cut: the old note fades out in another voice.
        int8_t keep = c.voice;
        cutVoice(_voices[keep]);
        c.voice = keep;
    } else {
        c.voice = (int8_t)allocVoice(ch);
    }
    ItVoice& v = _voices[c.voice];
    v.channel = (int8_t)ch;
    v.background = false;
    v.instrument = _instrumentMode ? c.instrument : 0;
    v.sampleIndex = si;
    v.note = note;
    v.keyOff = false;
    v.fading = false;
    v.fadeVolume = 65536;
    v.volEnvPos = v.panEnvPos = v.pitchEnvPos = 0;
    const ItInstrument* in = _instrumentMode ? &_instruments[c.instrument - 1] : nullptr;
    v.volEnvOn = in && in->volEnv.on();
    v.panEnvOn = in && in->panEnv.on();
    v.pitchEnvOn = in && in->pitchEnv.on() && !(in->pitchEnv.flags & 0x80);
    v.filterEnvOn = in && in->pitchEnv.on() && (in->pitchEnv.flags & 0x80);
    v.lastHz = -1;
    v.fadeOut = false;
    v.curL = v.curR = 0;
    v.rampLeft = 0;
    v.autoVibDepth = 0;
    v.autoVibPos = 0;
    // Random volume and pan (RV/RP), as OpenMPT plays IT: the volume swing
    // is a share of the sample and instrument volume, the pan swing up to 4 * RP.
    uint32_t insVol = (uint32_t)s.globalVolume * (in ? in->globalVolume : 128u);
    v.panSwing = 0;
    if (in && in->randVol) {
        float d = (float)nextRand() / 32767.5f - 1.0f;
        int sw = (int)floorf(d * (float)insVol * (float)in->randVol / 100.0f);
        int iv = (int)insVol + sw;
        insVol = (uint32_t)(iv < 0 ? 0 : (iv > 8192 ? 8192 : iv));
    }
    if (in && in->randPan) {
        float d = (float)nextRand() / 32767.5f - 1.0f;
        v.panSwing = (int16_t)floorf(d * 4.0f * (float)in->randPan);
    }
    v.insVol = (uint16_t)insVol;
    if (in && (in->filterCutoff & 0x80)) c.cutoff = in->filterCutoff & 0x7F;
    if (in && (in->filterReso & 0x80)) c.resonance = in->filterReso & 0x7F;
    v.cutoff = c.cutoff;
    v.resonance = c.resonance;
    v.filterOn = false;
    v.filt.y1 = v.filt.y2 = 0;
    setupFilter(v, 256, true);
    v.loudness = 64;
    c.period = noteToPeriod(realNote, s.c5speed);
    v.smp = &s.sustain;
    if (s.levels) {
        // Start on the copy the note's pitch reads, so the first read is from it.
        int level = levelFor(periodToHz(c.period, s.c5speed) / (float)SAMPLE_RATE, 0, s.levels);
        for (; level > 0; level--) {
            const ItSampleView* lv = levelView(si, s.sustain.sustainView, level);
            if (lv) { v.smp = lv; break; }
        }
    }
    v.looping = s.sustain.loopEnd > s.sustain.loopStart;
    startVoiceAt(v, 0);

    c.sampleIndex = si;
    c.note = note;
    c.portaTarget = c.period;
    c.nnaOverride = 0xFF;
    if (in && in->usePan) { c.pan = (uint16_t)(in->defaultPan * 4); c.surround = false; }
    if (s.usePan) { c.pan = (uint16_t)(s.defaultPan * 4); c.surround = false; }
    if (in && in->pitchPanSep) {
        int p = (int)c.pan + ((int)realNote - (int)in->pitchPanCentre) * in->pitchPanSep / 2;
        c.pan = (uint16_t)(p < 0 ? 0 : (p > 256 ? 256 : p));
    }
    (void)instArg;
}

// ---------------------------------------------------------------- filters

// A macro string from the file, e.g. "F0F000z". Only the filter cutoff and
// resonance ones are kept.
ItPlayer::FilterMacro ItPlayer::parseMacro(const uint8_t* text) {
    FilterMacro m;
    int bytes[4], nb = 0, nib = -1;
    for (int i = 0; i < 32 && text[i]; i++) {
        char ch = (char)text[i];
        int val;
        if (ch == ' ') continue;
        if (ch >= '0' && ch <= '9') val = ch - '0';
        else if (ch >= 'A' && ch <= 'F') val = ch - 'A' + 10;
        else if (ch == 'z' && nib < 0) { if (nb >= 4) return m; bytes[nb++] = 0x100; continue; }
        else return m;
        if (nib < 0) { nib = val; continue; }
        if (nb >= 4) return m;
        bytes[nb++] = (nib << 4) | val;
        nib = -1;
    }
    if (nib >= 0 || nb != 4 || bytes[0] != 0xF0 || bytes[1] != 0xF0 || (bytes[2] != 0 && bytes[2] != 1)) return m;
    m.kind = (uint8_t)(bytes[2] + 1);
    m.value = bytes[3] == 0x100 ? 0xFF : (uint8_t)(bytes[3] & 0x7F);
    return m;
}

// The file's MIDI macros if it has them, else OpenMPT's defaults: SF0 sets
// the cutoff, Z80-Z8F the resonance in steps of 8.
void ItPlayer::loadMacros(uint8_t special, uint32_t at) {
    for (FilterMacro& m : _sfxMacro) m = FilterMacro();
    for (FilterMacro& m : _zxxMacro) m = FilterMacro();
    if (special & 8) {
        if (special & 2) { // edit history comes first
            uint8_t n[2];
            if (!_file.seekSet(at) || _file.read(n, 2) != 2) return;
            at += 2 + rd16(n) * 8u;
        }
        at += 9 * 32; // global macros
        if (!_file.seekSet(at)) return;
        for (int i = 0; i < 16 + 128; i += 32) {
            int n = (16 + 128 - i) < 32 ? (16 + 128 - i) : 32;
            if (_file.read(_rawScratch, n * 32) != n * 32) return;
            for (int k = 0; k < n; k++) {
                int idx = i + k;
                FilterMacro m = parseMacro(_rawScratch + k * 32);
                if (idx < 16) _sfxMacro[idx] = m;
                else _zxxMacro[idx - 16] = m;
            }
        }
        return;
    }
    _sfxMacro[0].kind = 1;
    _sfxMacro[0].value = 0xFF;
    for (int i = 0; i < 16; i++) { _zxxMacro[i].kind = 2; _zxxMacro[i].value = (uint8_t)(i * 8); }
}

// Zxx: Z00-Z7F run the channel's SFx macro with z = xx, Z80-ZFF the fixed ones.
void ItPlayer::runMacro(int ch, uint8_t param, bool noteRow) {
    ItChannel& c = _channels[ch];
    const FilterMacro& m = param < 0x80 ? _sfxMacro[c.macro & 15] : _zxxMacro[param & 0x7F];
    if (!m.kind) return;
    uint8_t val = m.value == 0xFF ? (uint8_t)(param & 0x7F) : m.value;
    ItVoice* v = c.voice >= 0 ? &_voices[c.voice] : nullptr;
    if (m.kind == 1) {
        int change = (int)c.cutoff - (int)val;
        c.cutoff = val;
        if (!v) return;
        v->cutoff = val;
        // As IT: a big cutoff change next to a note at volume 0 waits for the next tick's filter update.
        if (c.volume > 0 || (change < 0 ? -change : change) < 0x10 || !v->filterOn || !noteRow) setupFilter(*v, 256, noteRow);
    } else {
        c.resonance = val;
        if (!v) return;
        v->resonance = val;
        setupFilter(*v, 256, noteRow);
    }
}

// Coefficients for v's cutoff and resonance, IT's filter as OpenMPT works
// it out. envMod is the filter envelope, -256..256, 256 without one. Float,
// but only when the settings change, never per sample.
void ItPlayer::setupFilter(ItVoice& v, int envMod, bool noteRow) {
    int cutoff = v.cutoff, reso = v.resonance;
    int scaled = cutoff * (envMod + 256); // 0-127*512
    // Full cutoff and no resonance turns the filter off next to a note,
    // and otherwise leaves it as it was.
    if (reso == 0 && scaled / 256 >= 254) {
        if (noteRow) v.filterOn = false;
        return;
    }
    float fc;
    if (!_extFilterRange) {
        // 110 * 2^(0.25 + scaled / 12288), from the tables.
        int oct = scaled / 12288, rem = scaled % 12288;
        fc = 130.8127827f * (float)(1 << oct) * EXP2_COARSE[rem / 96] * EXP2_FINE[rem % 96];
    } else {
        fc = 110.0f * powf(2.0f, 0.25f + (float)scaled / (20.0f * 512.0f));
    }
    int freq = (int)(fc + 0.5f);
    if (freq < 120) freq = 120;
    if (freq > 20000) freq = 20000;
    if (freq * 2 > SAMPLE_RATE) freq = SAMPLE_RATE / 2;
    // The coefficients only depend on the whole-Hz cutoff and the resonance.
    if (v.filterOn && v.filterCut == (uint16_t)freq && v.filterReso == reso) return;
    const float fs = (float)SAMPLE_RATE, pi = 3.14159265f;
    float dmp = RESO_DAMP[reso & 127];
    float d, e;
    if (!_extFilterRange) {
        float r = fs / (2.0f * pi * (float)freq);
        d = dmp * r + dmp - 1.0f;
        e = r * r;
    } else {
        float w = 2.0f * pi * (float)freq / fs;
        d = (1.0f - 2.0f * dmp) * w;
        if (d > 2.0f) d = 2.0f;
        d = (2.0f * dmp - d) / w;
        e = 1.0f / (w * w);
    }
    float inv = 1.0f / (1.0f + d + e);
    auto q24 = [](float x) -> int32_t {
        float y = x * 16777216.0f;
        const float lim = 33554431.0f; // the mixer's split multiply needs |coef| < 2^25
        if (y > lim) y = lim;
        if (y < -lim) y = -lim;
        return (int32_t)(y < 0 ? y - 0.5f : y + 0.5f);
    };
    v.filt.a0 = q24(inv);
    v.filt.b0 = q24((d + e + e) * inv);
    v.filt.b1 = q24(-e * inv);
    if (!v.filterOn) v.filt.y1 = v.filt.y2 = 0;
    v.filterOn = true;
    v.filterCut = (uint16_t)freq;
    v.filterReso = (uint8_t)reso;
}

// ---------------------------------------------------------------- mixing

const int16_t* __not_in_flash_func(ItPlayer::mixBlock)(uint32_t n) {
    int32_t* acc = _mix.acc;
    memset(acc, 0, n * 2 * sizeof(int32_t));
    for (int i = 0; i < MAX_VOICES; i++) {
        ItVoice& v = _voices[i];
        if (!v.havePair || v.sampleStep16 == 0 || !v.smp) continue;
        mixVoiceBlock(v, v.filterOn ? &v.filt : nullptr, acc, n, v.fadeOut);
        if (v.fadeOut && (v.rampLeft == 0 || !v.havePair)) releaseVoice(v);
    }
    // In place: out[i] only overwrites acc entries already read.
    const int32_t gain = outputGain(), limit = mixLimit(gain);
    for (uint32_t i = 0; i < n * 2; i++) _mix.out[i] = finishMix(_mix.acc[i], gain, limit);
    return _mix.out;
}

// ---------------------------------------------------------------- rows

void ItPlayer::handleCell(int ch, int note, uint8_t inst, uint8_t vol, uint8_t cmd, uint8_t param) {
    ItChannel& c = _channels[ch];
    // SDx holds the cell until tick x.
    if (cmd == 19 && (param >> 4) == 0xD && c.noteDelayTick == 0xFF) {
        uint8_t t = param & 15;
        c.noteDelayTick = t ? t : 1;
        c.pendNote = note; c.pendInst = inst; c.pendVol = vol;
        c.effect = cmd; c.param = param;
        return;
    }
    c.effect = cmd;
    c.param = param;
    c.volCol = vol;

    bool porta = cmd == 7 || cmd == 12 || (vol >= 193 && vol <= 202);
    _noteRow = note >= 0 && note < 120 && !porta;
    if (inst != 0) {
        bool changed = inst != c.instrument;
        c.instrument = inst;
        // An instrument number resets the note volume to the sample's default.
        uint8_t si = c.sampleIndex;
        if (note >= 0 && note < 120) {
            if (_instrumentMode && inst <= MAX_INSTRUMENTS) si = (uint8_t)(_instruments[inst - 1].sampleMap[note] - 1);
            else if (!_instrumentMode) si = (uint8_t)(inst - 1);
        } else if (!_instrumentMode) {
            si = (uint8_t)(inst - 1);
        }
        if (si < _numSamples) c.volume = _samples[si].volume;
        (void)changed;
        // An instrument with no note picks a held note back up, as IT.
        if ((note < 0 || note >= 120) && c.voice >= 0) {
            ItVoice& v = _voices[c.voice];
            if (v.fading || v.keyOff) {
                v.fading = false;
                v.fadeVolume = 65536;
            }
        }
    }
    if (note >= 0 && note < 120) {
        triggerNote(ch, (uint8_t)note, inst, porta);
        if (cmd == 15 && !porta && c.voice >= 0) {
            if (param) c.offsetMem = param;
            uint32_t off = ((uint32_t)c.offsetHigh << 16) | ((uint32_t)c.offsetMem << 8);
            ItVoice& v = _voices[c.voice];
            const ItSample& s = _samples[v.sampleIndex];
            // The current loop, in the sample's own frames.
            const ItSampleView* view = static_cast<const ItSampleView*>(v.smp)->sustainView ? &s.sustain : &s.normal;
            uint32_t end = view->realLoopEnd() > 0 && v.looping ? view->realLoopEnd() : s.length;
            // Past the end (or the loop end), as OpenMPT: new effects play
            // from the start, old effects clip to the end.
            if (off < end) startVoiceAtSampleFrame(v, off);
            else if (_oldEffects) {
                if (v.looping) startVoiceAtSampleFrame(v, view->loopStart);
                else releaseVoice(v);
            }
        }
    } else if (note == 255) {
        if (c.voice >= 0) voiceNoteOff(_voices[c.voice]);
    } else if (note == 254) {
        if (c.voice >= 0) cutVoice(_voices[c.voice]);
        c.period = 0;
    } else if (note >= 120) {
        if (c.voice >= 0) {
            if (_instrumentMode) _voices[c.voice].fading = true;
            else cutVoice(_voices[c.voice]);
        }
    }

    volumeColumnRow(ch);
    rowEffect(ch);
}

void ItPlayer::doVolSlide(uint8_t& v, uint8_t p, uint8_t max, bool firstTick) {
    uint8_t x = p >> 4, y = p & 15;
    int nv = v;
    if (firstTick) {
        if (y == 0xF && x) nv += x;
        else if (x == 0xF && y) nv -= y;
    } else {
        if (x == 0 && y) nv -= y;
        else if (y == 0 && x) nv += x;
    }
    if (nv < 0) nv = 0;
    if (nv > max) nv = max;
    v = (uint8_t)nv;
}

void ItPlayer::volumeColumnRow(int ch) {
    ItChannel& c = _channels[ch];
    uint8_t v = c.volCol;
    if (v == 0xFF) return;
    if (v <= 64) { c.volume = v; c.tremorMute = false; }
    else if (v <= 74) { uint8_t a = v - 65; if (a) c.volColSlide = a; else a = c.volColSlide; c.volume = (uint8_t)min(64, c.volume + a); }
    else if (v <= 84) { uint8_t a = v - 75; if (a) c.volColSlide = a; else a = c.volColSlide; c.volume = (uint8_t)max(0, c.volume - a); }
    else if (v <= 94) { if (v - 85) c.volColSlide = v - 85; }
    else if (v <= 104) { if (v - 95) c.volColSlide = v - 95; }
    else if (v <= 124) {
        uint8_t a = (uint8_t)(((v - 105) % 10) * 4);
        if (a) { c.portaMem = a; if (_linkG) c.tonePortaMem = a; }
    } else if (v >= 128 && v <= 192) { c.pan = (uint16_t)((v - 128) * 4); c.surround = false; }
    else if (v >= 193 && v <= 202) {
        static const uint8_t TABLE[10] = {0, 1, 4, 8, 16, 32, 64, 96, 128, 255};
        uint8_t a = TABLE[v - 193];
        if (a) { c.tonePortaMem = a; if (_linkG) c.portaMem = a; }
    } else if (v >= 203 && v <= 212) {
        if (v - 203) c.vibDepth = (uint8_t)(v - 203);
    }
}

void ItPlayer::volumeColumnTick(int ch) {
    ItChannel& c = _channels[ch];
    uint8_t v = c.volCol;
    if (v == 0xFF) return;
    if (v >= 85 && v <= 94) c.volume = (uint8_t)min(64, c.volume + c.volColSlide);
    else if (v >= 95 && v <= 104) c.volume = (uint8_t)max(0, c.volume - c.volColSlide);
    else if (v >= 105 && v <= 114) slidePitch(c, -(int32_t)c.portaMem * 4);
    else if (v >= 115 && v <= 124) slidePitch(c, (int32_t)c.portaMem * 4);
}

void ItPlayer::rowEffect(int ch) {
    ItChannel& c = _channels[ch];
    uint8_t p = c.param;
    switch (c.effect) {
        case 1: if (p) { _speed = p; _baseSpeed = p; } break;
        case 2: _positionJumpPending = true; _positionJumpTarget = p; if (!_patternBreakPending) _patternBreakRow = 0; break;
        case 3: _patternBreakPending = true; _patternBreakRow = p; break;
        case 4: case 11: case 12:
            if (p) c.volSlide = p;
            doVolSlide(c.volume, c.volSlide, 64, true);
            break;
        case 5: case 6: {
            if (p) { c.portaMem = p; if (_linkG) c.tonePortaMem = p; }
            uint8_t m = c.portaMem;
            int32_t dir = c.effect == 6 ? 1 : -1;
            if (m >= 0xF0) slidePitch(c, dir * (int32_t)(m & 15) * 4);
            else if (m >= 0xE0) slidePitch(c, dir * (int32_t)(m & 15));
            break;
        }
        case 7: if (p) { c.tonePortaMem = p; if (_linkG) c.portaMem = p; } break;
        case 8: case 21:
            if (p >> 4) c.vibSpeed = p >> 4;
            if (p & 15) c.vibDepth = p & 15;
            c.vibFine = c.effect == 21;
            break;
        case 9: if (p) c.tremorParam = p; break;
        case 10: if (p) c.arpParam = p; break;
        case 13: if (p <= 64) c.channelVolume = p; break;
        case 14: if (p) c.chanVolSlide = p; doVolSlide(c.channelVolume, c.chanVolSlide, 64, true); break;
        case 15: if (p) c.offsetMem = p; break;
        case 16: {
            if (p) c.panSlide = p;
            uint8_t q = c.panSlide, x = q >> 4, y = q & 15;
            int pan = c.pan;
            if (y == 0xF && x) pan -= x * 4;
            else if (x == 0xF && y) pan += y * 4;
            c.pan = (uint16_t)(pan < 0 ? 0 : (pan > 256 ? 256 : pan));
            break;
        }
        case 17: if (p) { c.retrigParam = p; } break;
        case 18: if (p >> 4) c.tremSpeed = p >> 4; if (p & 15) c.tremDepth = p & 15; break;
        case 19: specialRow(ch, p); break;
        case 20: if (p >= 0x20) _tempo = p; else if (p) c.tempoMem = p; break;
        case 22: if (p <= 128) _globalVol = p; break;
        case 23: {
            if (p) c.globalSlide = p;
            doVolSlide(_globalVol, c.globalSlide, 128, true);
            break;
        }
        case 24: c.pan = p; c.surround = false; break;
        case 25: if (p >> 4) c.panbSpeed = p >> 4; if (p & 15) c.panbDepth = p & 15; break;
        case 26: runMacro(ch, p, _noteRow); break;
        default: break;
    }
}

void ItPlayer::specialRow(int ch, uint8_t p) {
    ItChannel& c = _channels[ch];
    uint8_t x = p >> 4, y = p & 15;
    switch (x) {
        case 3: c.vibWave = y & 3; break;
        case 4: c.tremWave = y & 3; break;
        case 5: c.panbWave = y & 3; break;
        case 6: _speed = (uint8_t)(_speed + y); break;
        case 7:
            if (y <= 2) {
                for (int i = 0; i < MAX_VOICES; i++) {
                    ItVoice& v = _voices[i];
                    if (v.channel != ch || !v.background) continue;
                    if (y == 0) cutVoice(v);
                    else if (y == 1) voiceNoteOff(v);
                    else v.fading = true;
                }
            } else if (y <= 6) {
                c.nnaOverride = y - 3;
            } else if (c.voice >= 0) {
                ItVoice& v = _voices[c.voice];
                const ItInstrument* in = (v.instrument >= 1 && v.instrument <= MAX_INSTRUMENTS) ? &_instruments[v.instrument - 1] : nullptr;
                if (y == 7) v.volEnvOn = false;
                else if (y == 8) v.volEnvOn = in && in->volEnv.numPoints > 0;
                else if (y == 9) v.panEnvOn = false;
                else if (y == 10) v.panEnvOn = in && in->panEnv.numPoints > 0;
                else if (y == 11) v.pitchEnvOn = v.filterEnvOn = false;
                else if (y == 12) {
                    v.pitchEnvOn = in && in->pitchEnv.numPoints > 0 && !(in->pitchEnv.flags & 0x80);
                    v.filterEnvOn = in && in->pitchEnv.numPoints > 0 && (in->pitchEnv.flags & 0x80);
                }
            }
            break;
        case 8: c.pan = (uint16_t)(y * 256 / 15); c.surround = false; break;
        case 9: if (y == 1) c.surround = true; break;
        case 10: c.offsetHigh = y; break;
        case 11: patternLoop(c.loopRow, c.loopCount, y); break;
        case 12: c.noteCutTick = y ? y : 1; break;
        case 14: if (_patternDelayRepeatsLeft == 0 && !_rowDelaySet) { _patternDelayRepeatsLeft = y; _rowDelaySet = true; } break;
        case 15: c.macro = y; break;
        default: break;
    }
}

void ItPlayer::tickEffect(int ch) {
    ItChannel& c = _channels[ch];
    if (c.noteDelayTick != 0xFF && _tick == c.noteDelayTick) {
        c.noteDelayTick = 0xFE; // done
        handleCell(ch, c.pendNote, c.pendInst, c.pendVol, 0, 0);
        c.effect = 19; c.param = 0xD0;
    }
    if (c.noteCutTick != 0xFF && _tick == c.noteCutTick) {
        if (c.voice >= 0) cutVoice(_voices[c.voice]);
        c.noteCutTick = 0xFF;
    }
    volumeColumnTick(ch);
    uint8_t p = c.param;
    switch (c.effect) {
        case 4: case 11: case 12: doVolSlide(c.volume, c.volSlide, 64, false); break;
        case 5: if (c.portaMem < 0xE0) slidePitch(c, -(int32_t)c.portaMem * 4); break;
        case 6: if (c.portaMem < 0xE0) slidePitch(c, (int32_t)c.portaMem * 4); break;
        case 14: doVolSlide(c.channelVolume, c.chanVolSlide, 64, false); break;
        case 16: {
            uint8_t q = c.panSlide, x = q >> 4, y = q & 15;
            int pan = c.pan;
            if (x == 0 && y) pan += y * 4;
            else if (y == 0 && x) pan -= x * 4;
            c.pan = (uint16_t)(pan < 0 ? 0 : (pan > 256 ? 256 : pan));
            break;
        }
        case 20:
            if (p < 0x20) {
                uint8_t m = c.tempoMem;
                int t = _tempo;
                if ((m >> 4) == 0) t -= m & 15;
                else if ((m >> 4) == 1) t += m & 15;
                _tempo = (uint16_t)(t < 32 ? 32 : (t > 255 ? 255 : t));
            }
            break;
        case 23: doVolSlide(_globalVol, c.globalSlide, 128, false); break;
        default: break;
    }
    // Tone portamento (G, L, volume column g).
    bool porta = c.effect == 7 || c.effect == 12 || (c.volCol >= 193 && c.volCol <= 202);
    if (porta && c.period != 0 && c.portaTarget != 0) {
        int32_t speed = (int32_t)c.tonePortaMem * 4 * (_linear ? 1 : (int32_t)AMIGA_FRACT);
        if (c.period < c.portaTarget) c.period = min(c.portaTarget, c.period + speed);
        else if (c.period > c.portaTarget) c.period = max(c.portaTarget, c.period - speed);
    }
}

// Vibrato, tremolo, panbrello, tremor, arpeggio and retrigger, on every
// tick of the row including the first.
void ItPlayer::updateChannelVoice(int ch) {
    ItChannel& c = _channels[ch];
    bool first = _tick == 0;
    c.playPeriod = c.period;
    c.arpeggioSemis = 0;
    c.tremoloDelta = 0;
    c.panbrelloDelta = 0;
    uint8_t e = c.effect;
    if (e == 8 || e == 11 || e == 21 || (c.volCol >= 203 && c.volCol <= 212)) {
        int d = waveValue(c.vibWave, c.vibPos) * c.vibDepth;
        int shift = (e == 21 ? 6 : 4) - (_oldEffects ? 1 : 0);
        d = d >> shift;
        c.playPeriod = c.period + (_linear ? d : d * (int)AMIGA_FRACT);
        if (!first || !_oldEffects) c.vibPos = (uint8_t)(c.vibPos + c.vibSpeed * 4);
    }
    if (e == 18) {
        c.tremoloDelta = (int8_t)((waveValue(c.tremWave, c.tremPos) * c.tremDepth) >> 5);
        c.tremPos = (uint8_t)(c.tremPos + c.tremSpeed * 4);
    }
    if (e == 25) {
        c.panbrelloDelta = (int16_t)((waveValue(c.panbWave, c.panbPos) * c.panbDepth) >> 1);
        c.panbPos = (uint8_t)(c.panbPos + c.panbSpeed);
    }
    if (e == 9) {
        uint8_t on = c.tremorParam >> 4, off = c.tremorParam & 15;
        if (_oldEffects) { on++; off++; }
        if (!on) on = 1;
        if (!off) off = 1;
        c.tremorMute = c.tremorCount >= on;
        c.tremorCount++;
        if (c.tremorCount >= on + off) c.tremorCount = 0;
    } else if (c.tremorMute && e != 9) {
        c.tremorMute = false;
    }
    if (e == 10) {
        int t = _tick % 3;
        c.arpeggioSemis = (int8_t)(t == 1 ? (c.arpParam >> 4) : (t == 2 ? (c.arpParam & 15) : 0));
    }
    if (e == 17 && c.voice >= 0) {
        uint8_t y = c.retrigParam & 15;
        if (y == 0) y = 1;
        c.retrigCount++;
        if (c.retrigCount >= y) {
            c.retrigCount = 0;
            int v = c.volume;
            switch (c.retrigParam >> 4) {
                case 1: v -= 1; break; case 2: v -= 2; break; case 3: v -= 4; break; case 4: v -= 8; break;
                case 5: v -= 16; break; case 6: v = v * 2 / 3; break; case 7: v >>= 1; break;
                case 9: v += 1; break; case 10: v += 2; break; case 11: v += 4; break; case 12: v += 8; break;
                case 13: v += 16; break; case 14: v = v * 3 / 2; break; case 15: v *= 2; break;
                default: break;
            }
            c.volume = (uint8_t)(v < 0 ? 0 : (v > 64 ? 64 : v));
            if (!first) startVoiceAt(_voices[c.voice], 0);
        }
    }

    if (c.voice < 0) return;
    ItVoice& v = _voices[c.voice];
    const ItSample& s = _samples[v.sampleIndex];
    if (c.period != 0) {
        float hz = periodToHz(c.playPeriod, s.c5speed);
        if (c.arpeggioSemis) hz *= powf(2.0f, (float)c.arpeggioSemis / 12.0f);
        v.baseHz = hz;
    } else {
        v.baseHz = 0;
    }
    int vol = (int)c.volume + c.tremoloDelta;
    if (vol < 0) vol = 0;
    if (vol > 64) vol = 64;
    if (c.tremorMute || c.muted) vol = 0; // muted channels still run their effects, as OpenMPT
    uint32_t a = ((uint32_t)vol * v.insVol) >> 7;
    a = (a * c.channelVolume) >> 6;
    v.baseVolume = (uint16_t)a;
    v.surround = c.surround;
    int pan = c.surround ? 128 : (int)c.pan + c.panbrelloDelta + v.panSwing;
    v.basePan = (uint16_t)(pan < 0 ? 0 : (pan > 256 ? 256 : pan));
}

void ItPlayer::advanceRow() {
    _speed = _baseSpeed;
    _rowDelaySet = false;
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        ItChannel& c = _channels[ch];
        c.effect = 0;
        c.param = 0;
        c.volCol = 0xFF;
        c.noteCutTick = 0xFF;
        c.noteDelayTick = 0xFF;
    }

    stepPosition(_patRowCount, 0x7FFFFFFF);
    // 254 entries are skipped, 255 ends the song.
    int guard = 0;
    while (_orderPos < _songLength && _orderTable[_orderPos] == 254 && guard++ < MAX_ORDERS) { _orderPos++; }
    if (_orderPos < 0 || _orderPos >= _songLength || _orderTable[_orderPos] == 255) {
        _state = STATE_DONE;
        return;
    }
    int num = _orderTable[_orderPos];
    if (num != _openPattern) openPattern(num);
    if (_row >= _patRowCount) _row = 0;
    readRowTo(_row);

    for (int ch = 0; ch < _numChannels && ch < MAX_CHANNELS; ch++) updateChannelVoice(ch);
}

void ItPlayer::processTickEffects() {
    for (int ch = 0; ch < _numChannels && ch < MAX_CHANNELS; ch++) {
        tickEffect(ch);
        updateChannelVoice(ch);
    }
}

// Frames in the next tick. Files OpenMPT saved in its "modern" tempo mode
// carry the fraction so they keep time with it. 32-bit only: this runs
// inside update().
uint32_t ItPlayer::tickLength() {
    uint32_t tempo = _tempo ? _tempo : 125;
    // Alternative: tempo is ticks a second.
    if (_tempoMode == 1) return (uint32_t)SAMPLE_RATE / tempo;
    if (_tempoMode != 2) return ticksToSamples((uint16_t)tempo); // truncated, as OpenMPT plays these
    // Modern: tempo is beats a minute, a beat is _rowsPerBeat rows. The
    // speed leaves out S6x's extra ticks, so they lengthen the row.
    uint32_t div = tempo * _rowsPerBeat * (_baseSpeed ? _baseSpeed : 1);
    uint32_t num = (uint32_t)SAMPLE_RATE * 60u;
    uint32_t whole = num / div;
    uint32_t rem = num % div;
    // rem / div as a 16-bit fraction; div is at most 255 * 255 * 255, so
    // shift rem down with it to stay in 32 bits.
    int sh = 0;
    while ((rem >> sh) > 0xFFFF) sh++;
    _tickFraction += ((rem >> sh) << 16) / ((div >> sh) ? (div >> sh) : 1);
    whole += _tickFraction >> 16;
    _tickFraction &= 0xFFFF;
    return whole;
}

// Envelopes, fadeout and the mix values for every sounding voice, once a tick.
void ItPlayer::processVoices() {
    _globalVolume = (uint8_t)(_globalVol >> 1);
#ifdef TRACKER_PROFILE
    uint32_t sounding = 0;
    profile.ticks++;
#endif
    for (int i = 0; i < MAX_VOICES; i++) {
        ItVoice& v = _voices[i];
        if (v.channel < 0) continue;
        if (!v.havePair || !v.smp) { releaseVoice(v); continue; }
        const ItInstrument* in = (_instrumentMode && v.instrument >= 1 && v.instrument <= MAX_INSTRUMENTS) ? &_instruments[v.instrument - 1] : nullptr;

        int envVol = 64 * 256;
        if (in && v.volEnvOn && in->volEnv.numPoints > 0) {
            const ItEnvelope& e = in->volEnv;
            envVol = envValue(e.y, e.x, e.numPoints, v.volEnvPos);
            bool ended = false;
            uint16_t p = v.volEnvPos + 1;
            if ((e.flags & 4) && !v.keyOff) { if (p > e.x[e.susEnd]) p = e.x[e.susStart]; }
            else if (e.flags & 2) { if (p > e.x[e.loopEnd]) p = e.x[e.loopStart]; }
            if (p > e.x[e.numPoints - 1]) { p = e.x[e.numPoints - 1]; ended = true; }
            v.volEnvPos = p;
            if (ended) {
                v.fading = true;
                if (e.y[e.numPoints - 1] == 0 && envVol == 0) { releaseVoice(v); continue; }
            }
        }
        if (v.fading) {
            if (!in) { releaseVoice(v); continue; }
            uint32_t step = (uint32_t)in->fadeout * 64;
            v.fadeVolume = v.fadeVolume > step ? v.fadeVolume - step : 0;
            if (v.fadeVolume == 0) { releaseVoice(v); continue; }
        }

        uint32_t a = v.baseVolume; // 0-4096
        a = (a * (uint32_t)envVol) >> 14;
        a = (a * (v.fadeVolume >> 4)) >> 12;
        uint32_t mv = (a + 32) >> 6;
        v.mixVolume = (uint8_t)(mv > 64 ? 64 : mv);
        v.loudness = (uint16_t)(v.background ? a >> 1 : a);
        if (a > 4096) a = 4096;
#ifdef TRACKER_PROFILE
        // a is 4096 at full volume: under 16 is below -48 dB.
        sounding++;
        if (v.background) profile.bgTicks++;
        if (a < 16) { profile.q48Ticks++; if (v.background) profile.q48BgTicks++; }
        if (a == 0) profile.zeroTicks++;
        if (v.sampleStep16 > (1u << 16)) profile.up1Ticks++;
        if (v.sampleStep16 > (2u << 16)) profile.up2Ticks++;
#endif

        int pan = v.basePan;
        if (in && v.panEnvOn && in->panEnv.numPoints > 0) {
            const ItEnvelope& e = in->panEnv;
            int ev = envValue(e.y, e.x, e.numPoints, v.panEnvPos);
            uint16_t p = v.panEnvPos + 1;
            if ((e.flags & 4) && !v.keyOff) { if (p > e.x[e.susEnd]) p = e.x[e.susStart]; }
            else if (e.flags & 2) { if (p > e.x[e.loopEnd]) p = e.x[e.loopStart]; }
            if (p > e.x[e.numPoints - 1]) p = e.x[e.numPoints - 1];
            v.panEnvPos = p;
            int range = pan < 128 ? pan : 256 - pan;
            pan += ev * range / (32 * 256);
        }
        // The header's mono flag is ignored, as OpenMPT plays such files in stereo.
        v.mixPan = (uint8_t)(pan < 0 ? 0 : (pan > 255 ? 255 : pan));
        // Gains for the mixer, s * g >> 15; surround plays the right side inverted, as OpenMPT.
        int32_t gl = (int32_t)((a * (255u - v.mixPan)) >> 5), gr = (int32_t)((a * v.mixPan) >> 5);
        if (v.surround) gr = -gr;
        setRamp(v, gl, gr);

        float hz = v.baseHz;
        float octaves = 0;
        if (in && (v.pitchEnvOn || v.filterEnvOn) && in->pitchEnv.numPoints > 0) {
            const ItEnvelope& e = in->pitchEnv;
            int ev = envValue(e.y, e.x, e.numPoints, v.pitchEnvPos);
            uint16_t p = v.pitchEnvPos + 1;
            if ((e.flags & 4) && !v.keyOff) { if (p > e.x[e.susEnd]) p = e.x[e.susStart]; }
            else if (e.flags & 2) { if (p > e.x[e.loopEnd]) p = e.x[e.loopStart]; }
            if (p > e.x[e.numPoints - 1]) p = e.x[e.numPoints - 1];
            v.pitchEnvPos = p;
            if (v.filterEnvOn) setupFilter(v, ev / 32, false); // -256..256
            else octaves = (float)ev / (24.0f * 256.0f);
        }
        // Sample auto-vibrato, IT's way: the depth grows by the rate each
        // tick up to depth * 256, and the swing is in 1/64 semitones, + is up.
        const ItSample& smp = _samples[v.sampleIndex];
        if (smp.vibDepth && smp.vibRate) {
            uint32_t depth = (uint32_t)v.autoVibDepth + smp.vibRate;
            if (depth > (uint32_t)smp.vibDepth * 256u) depth = (uint32_t)smp.vibDepth * 256u;
            v.autoVibDepth = (uint16_t)depth;
            uint8_t pos = v.autoVibPos;
            v.autoVibPos = (uint8_t)(pos + smp.vibSpeed);
            int w;
            switch (smp.vibType) {
                case 1: w = 64 - (pos + 1) / 2; break;       // ramp down
                case 2: w = pos < 128 ? 64 : 0; break;        // square
                case 3: w = (int)(nextRand() & 127) - 64; break; // random
                case 4: w = (pos + 1) / 2 - 64; break;        // ramp up
                default: w = itSine(pos); break;
            }
            int delta = w * (int)(depth >> 8) / 64;
            octaves += (float)delta / 768.0f;
        }
        if (octaves != 0) hz *= powf(2.0f, octaves);
        if (hz != v.lastHz) {
            v.lastHz = hz;
            int level = static_cast<const ItSampleView*>(v.smp)->level;
            if (smp.levels && hz > 0) {
                // Move to the copy this pitch reads, nearest it if that one can't play.
                int cur = level;
                int want = levelFor(hz / (float)SAMPLE_RATE, cur, smp.levels);
                bool sus = static_cast<const ItSampleView*>(v.smp)->sustainView;
                for (; want != cur; want += want > cur ? -1 : 1) {
                    const ItSampleView* to = levelView(v.sampleIndex, sus, want);
                    if (to) { moveVoice(v, to, true); level = want; break; }
                }
            }
            float rate = hz;
            if (level) {
                // A copy plays num / den frames per sample frame.
                LevelMap m = levelMap(smp, level);
                rate = hz * (float)m.num / (float)m.den;
            }
            v.sampleStep16 = hz > 0 ? stepForHz(rate) : 0;
        }
    }
#ifdef TRACKER_PROFILE
    profile.voiceTicks += sounding;
    if (sounding > profile.peakVoices) profile.peakVoices = sounding;
#endif
}
