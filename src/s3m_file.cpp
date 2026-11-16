#include "sd_card.h"
#include "s3m_file.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>

namespace {

// ST3's periods for the octave above C-4 (OpenMPT's FreqS3MTable).
const uint16_t S3M_PERIODS[12] = {1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907};

// C2Spd for S2x finetune 0-15.
const uint16_t FINETUNE_C2SPD[16] = {
    7895, 7941, 7985, 8046, 8107, 8169, 8232, 8280, 8363, 8413, 8463, 8529, 8581, 8651, 8723, 8757,
};

// Vibrato, tremolo and panbrello waveforms, 64 steps a cycle.
const int8_t SINE_TABLE[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126,
    127, 126, 125, 122, 117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
    0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12,
};
const int8_t RANDOM_TABLE[64] = {
    98, -127, -43, 88, 102, 41, -65, -94, 125, 20, -71, -86, -70, -32, -16, -96,
    17, 72, 107, -5, 116, -69, -62, -40, 10, -61, 65, 109, -18, -38, -13, -76,
    -23, 88, 21, -94, 8, 106, 21, -112, 6, 109, 20, -88, -30, 9, -127, 118,
    42, -34, 89, -4, -51, -72, 21, -29, 112, 123, 84, -101, -92, 98, -54, -95,
};

// Qxy volume change: multiply by T1/16 when T1 is set, else add T2.
const int8_t RETRIG_MUL[16] = {0, 0, 0, 0, 0, 0, 10, 8, 0, 0, 0, 0, 0, 0, 24, 32};
const int8_t RETRIG_ADD[16] = {0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0};

// Periods are kept in 1/256 units, so the Hz-based pitch of files from
// Impulse Tracker and Schism fits the same slides. A period's rate in
// 1/16 Hz is 8363 * 1712 * 16 * 256 over it, which is CLOCK_LO << 16.
const uint32_t CLOCK_LO = 894841u;
const uint32_t CLOCK_X256 = 8363u * 1712u * 256u; // Hz to a period
const uint32_t TOP_PERIOD = 32767u << 8;  // ST3's upper limit
const uint32_t AMIGA_LOW = (113u * 4) << 8, AMIGA_HIGH = (856u * 4) << 8;

// OpenMPT's LinearSlideUpTable for whole semitones: 65536 * 2^(n/12).
const uint32_t SEMITONE_RATIO[12] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715};

// Effect letters as stored (1 = A).
enum { CMD_A = 1, CMD_B, CMD_C, CMD_D, CMD_E, CMD_F, CMD_G, CMD_H, CMD_I, CMD_J, CMD_K, CMD_L, CMD_M,
       CMD_N, CMD_O, CMD_P, CMD_Q, CMD_R, CMD_S, CMD_T, CMD_U, CMD_V, CMD_W, CMD_X, CMD_Y, CMD_Z };

inline uint32_t paraToOffset(uint32_t para) { return para * 16; }
inline uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

inline int __not_in_flash_func(waveform)(uint8_t type, uint8_t pos) {
    pos &= 63;
    switch (type & 3) {
        case 1: return (pos < 32 ? 0 : 255) - pos * 4;
        case 2: return pos < 32 ? 127 : -127;
        case 3: return RANDOM_TABLE[pos];
        default: return SINE_TABLE[pos];
    }
}

} // namespace

bool S3mPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!sdOpenPath(_file, path, O_RDONLY)) return loadFail("could not open file");
    if (!sdOpenPath(_patternFile, path, O_RDONLY)) return loadFail("could not open file");
    const uint32_t fileSize = (uint32_t)_file.fileSize();

    // The fixed header is 96 bytes: name, type, counts, flags and tracker
    // version, "SCRM" at 44, volumes, speed, tempo and pan flag at 48-53,
    // the 32 channel settings at 64-95. The order table follows.
    uint8_t hdr[96];
    if (_file.read(hdr, 96) != 96) return loadFail("invalid S3M file");
    if (hdr[29] != 16 || memcmp(hdr + 44, "SCRM", 4) != 0) return loadFail("unsupported format");

    uint16_t ordNumReal = le16(hdr + 32), insNumReal = le16(hdr + 34), patNumReal = le16(hdr + 36);
    uint16_t flags = le16(hdr + 38), cwtv = le16(hdr + 40), ffi = le16(hdr + 42);
    uint8_t globalVol = hdr[48], initialSpeed = hdr[49], initialTempo = hdr[50], masterVol = hdr[51];
    bool panTableFollows = hdr[53] == 0xFC;
    if (ordNumReal > 4096 || insNumReal > 4096 || patNumReal > 4096) return loadFail("invalid S3M file");

    // What saved the file decides some behaviour, as OpenMPT reads it:
    // Imago Orpheus, Impulse Tracker and Schism don't share effect memory
    // or stop periods at the top.
    int tracker = cwtv >> 12;
    bool isST3 = tracker == 1;
    bool nonCompat = tracker == 2 || tracker == 3 || tracker == 4;
    _limitPeriod = false;
    if (cwtv == 0x1320 && le16(hdr + 62) == 0 && hdr[52] == 0) {
        // Says ST3.20 but ST3 always writes ultra-click values: OpenMPT's
        // guesses at ModPlug / Schism, PlayerPRO / Velvet and old IT.
        bool canonical = false;
        if (insNumReal && patNumReal && _file.seekSet(96 + ordNumReal)) {
            uint8_t pp[2];
            uint16_t ins0 = 0, pat0 = 0;
            if (_file.read(pp, 2) == 2) ins0 = le16(pp);
            if (_file.seekSet(96 + ordNumReal + insNumReal * 2) && _file.read(pp, 2) == 2) pat0 = le16(pp);
            canonical = pat0 > ins0;
        }
        if ((ordNumReal & 1) == 0 && (flags & ~0x50) == 0 && panTableFollows && canonical) {
            nonCompat = true;
            _limitPeriod = true;
            isST3 = false;
            if ((ordNumReal & 0x0F) == 0) tracker = 5; // ModPlug, played as stereo
        } else if (!panTableFollows && (flags == 0 || flags == 8)) {
            isST3 = false;
        }
        _file.seekSet(96);
    }
    _st3Memory = !nonCompat;
    _upperLimit = !nonCompat;
    _hertz = tracker == 3 || tracker == 4;
    _minPeriod = _hertz ? 0 : 64u << 8;
    _fastSlides = cwtv == 0x1300 || (flags & 0x40);
    _amigaLimits = (flags & 0x10) != 0;
    _oldVibrato = (flags & 0x01) != 0;

    // Master volume, 48 plays as is; mono files are 8/11 of that.
    uint32_t preamp;
    if (ffi == 1 && masterVol < 8) preamp = min(127, (masterVol + 1) * 16);
    else if (masterVol == 2 || masterVol == 0x12) preamp = 32;
    else if (!(masterVol & 0x7F)) preamp = 48;
    else preamp = max(masterVol & 0x7F, 16);
    bool isStereo = (masterVol & 0x80) || tracker == 5;
    if (!isStereo) preamp = (preamp * 8 + 5) / 11;

    // Channels: PCM channels get a player channel in file order. Unused
    // (0xFF), muted (bit 7) and AdLib (16-29) channels are skipped, so are
    // PCM channels past MAX_CHANNELS.
    _numChannels = 0;
    uint16_t defaultPan[32];
    for (int i = 0; i < 32; i++) {
        uint8_t v = hdr[64 + i], type = v & 0x7F;
        _chanSlot[i] = 0xFF;
        defaultPan[i] = !isStereo ? 128 : (type & 8) ? 0xCC : 0x33;
        if (v == 0xFF || (v & 0x80) || type >= 16 || _numChannels >= MAX_CHANNELS) continue;
        _chanSlot[i] = (uint8_t)_numChannels++;
    }

    int ordNum = (int)(ordNumReal < (uint16_t)MAX_ORDERS ? ordNumReal : (uint16_t)MAX_ORDERS);
    if (_file.read(_orderTable, ordNum) != ordNum) return loadFail("invalid S3M file");
    if (ordNumReal > ordNum) _file.seekCur(ordNumReal - ordNum);
    _songLength = ordNum;

    uint16_t insParapointers[MAX_INSTRUMENTS] = {0};
    int insNum = (int)(insNumReal < (uint16_t)MAX_INSTRUMENTS ? insNumReal : (uint16_t)MAX_INSTRUMENTS);
    for (int i = 0; i < insNum; i++) {
        uint8_t pb[2];
        if (_file.read(pb, 2) != 2) break;
        insParapointers[i] = le16(pb);
    }
    if (insNumReal > insNum) _file.seekCur((uint32_t)(insNumReal - insNum) * 2);

    uint16_t patParapointers[MAX_PATTERNS] = {0};
    int patNum = (int)(patNumReal < (uint16_t)MAX_PATTERNS ? patNumReal : (uint16_t)MAX_PATTERNS);
    for (int i = 0; i < patNum; i++) {
        uint8_t pb[2];
        if (_file.read(pb, 2) != 2) break;
        patParapointers[i] = le16(pb);
    }
    if (patNumReal > patNum) _file.seekCur((uint32_t)(patNumReal - patNum) * 2);
    // A pattern pointing past the end of the file plays as empty.
    for (int i = 0; i < MAX_PATTERNS; i++) {
        uint32_t off = (i < patNum) ? paraToOffset(patParapointers[i]) : 0;
        _patternOffsets[i] = (off + 2 < fileSize) ? off : 0;
    }

    // Pan table: bit 0x20 set means use the low nibble (0-15).
    if (panTableFollows) {
        uint8_t panTable[32];
        if (_file.read(panTable, 32) == 32) {
            for (int i = 0; i < 32; i++) {
                if (panTable[i] & 0x20) defaultPan[i] = (uint16_t)(((panTable[i] & 0x0F) * 256 + 8) / 15);
            }
        }
    }

    // Instrument headers, each found by its own parapointer.
    _numInstruments = 0;
    uint16_t gusAddresses = 0;
    bool anySamples = false;
    for (int i = 0; i < MAX_INSTRUMENTS; i++) {
        S3mSample& s = _samples[i];
        s = S3mSample();
        if (i >= insNum || insParapointers[i] == 0) continue;
        if (!_file.seekSet(paraToOffset(insParapointers[i]))) continue;
        uint8_t ib[42]; // the fields used, offsets 0-41 of the 80-byte record
        if (_file.read(ib, 42) != 42) continue;
        if (ib[0] != 1) continue; // 1 is PCM; 0 is empty and 2+ are AdLib, which stay silent

        uint32_t memSeg = ((uint32_t)ib[13] << 16) | ((uint32_t)ib[14]) | ((uint32_t)ib[15] << 8);
        uint32_t length = le32(ib + 16), loopBegin = le32(ib + 20), loopEnd = le32(ib + 24);
        uint8_t pack = ib[30], sflags = ib[31];
        s.fileOffset = paraToOffset(memSeg);
        s.volume = ib[28] > 64 ? 64 : ib[28];
        s.is16Bit = (sflags & 0x04) != 0;
        s.isSigned = ffi == 1;
        uint32_t c2spd = le32(ib + 32);
        if (isST3 && c2spd > 65535) c2spd = 65535;
        if (c2spd == 0) c2spd = 8363;
        else if (c2spd < 1024) c2spd = 1024;
        else if (c2spd > (1u << 20)) c2spd = 1u << 20;
        s.c2spd = c2spd;
        if (length) {
            anySamples = true;
            gusAddresses |= le16(ib + 40);
        }

        // Keep the sample inside the file; ADPCM (pack 4) isn't decoded.
        uint32_t bytesPerFrame = s.is16Bit ? 2 : 1;
        uint32_t room = s.fileOffset < fileSize ? (fileSize - s.fileOffset) / bytesPerFrame : 0;
        if (pack == 4 && !(sflags & 0x06)) length = 0;
        s.length = length < room ? length : room;
        if (length > 0 && (sflags & 0x01)) {
            // OpenMPT's loop checks, on the length the header gives.
            uint32_t ls = min(loopBegin, length - 1), le = min(loopEnd, length);
            if (le >= 2 && ls < le && le <= s.length) {
                s.loopStart = ls;
                s.loopEnd = le;
            }
        }
        if (s.length > 0) _numInstruments++;
    }

    // ST3 saved with the GUS driver writes a GUS address for each sample;
    // with the SoundBlaster it writes 0 or 1. A file that says ST3 but has
    // no addresses was made by something else.
    _sampleSwap = false;
    _portaKeepsSample = !nonCompat;
    if (isST3 && anySamples && !gusAddresses && cwtv != 0x1300) {
        isST3 = false;
    } else if (isST3) {
        bool useGUS = gusAddresses > 1;
        _sampleSwap = !useGUS;
        _portaKeepsSample = useGUS;
        if (useGUS) preamp = 48;
    }
    _preamp = (uint8_t)(preamp > 127 ? 127 : preamp);

    for (int i = 0; i < 32; i++) {
        if (_chanSlot[i] == 0xFF) continue;
        S3mChannel& c = _channels[_chanSlot[i]];
        c = S3mChannel();
        c.pan = defaultPan[i];
    }
    for (int ch = _numChannels; ch < MAX_CHANNELS; ch++) _channels[ch] = S3mChannel();
    attachVoices(&_channels[0], sizeof(S3mChannel), MAX_CHANNELS, &_chunks[0][0], CHUNK_FRAMES);
    useSampleFile(_file);
    attachTails(_tails, (int)(sizeof(_tails) / sizeof(_tails[0])), _tailPool, TAIL_POOL_FRAMES);

    if (globalVol > 64) globalVol = 64;
    if (globalVol == 0 && cwtv < 0x1320) globalVol = 64;
    _songVolume = globalVol;
    _globalSlide = 0;
    _globalSlideRow = false;
    _rowRepeats = 0;
    _loopRow = _loopCount = 0;
    _loopPending = false;
    _openPatternNum = -1;
    _rowOffsetsValid = 0;
    if (initialSpeed == 0 || (initialSpeed == 255 && isST3)) initialSpeed = 6;
    uint16_t tempo = initialTempo;
    if (tempo < 33) tempo = isST3 ? 125 : 32;
    resetSequencer(initialSpeed, tempo);
    _globalVolume = (uint8_t)((_songVolume * _preamp + 24) / 48);
    // The master volume is in _globalVolume, relative to 48.
    _masterGain = masterGainFor(MIX_COMPATIBLE, 48, _numChannels);
    return true;
}

uint32_t __not_in_flash_func(S3mPlayer::periodForNote)(int note, uint32_t c2spd) const {
    if (note < 0) note = 0;
    if (note > 131) note = 131;
    int oct = note / 12 + 1; // OpenMPT's octave, one up from ST3's
    if (_hertz) {
        // c2spd * ratio * 2^oct / 2^21 Hz, the product split so it fits 32 bits.
        uint32_t r = SEMITONE_RATIO[note % 12];
        uint32_t hi = c2spd * (r >> 8), lo = c2spd * (r & 255);
        uint32_t hz = (hi + (lo >> 8)) >> (13 - oct);
        return hz ? CLOCK_X256 / hz : 0;
    }
    // 8363 * (table << 5) / (c2spd << octave), as ST3 rounds it.
    return ((8363u * ((uint32_t)S3M_PERIODS[note % 12] << 5)) / (c2spd << oct)) << 8;
}

int __not_in_flash_func(S3mPlayer::noteForPeriod)(uint32_t period, uint32_t c2spd) const {
    // Periods fall as notes rise, so binary search for the first one at or below.
    int lo = 0, hi = 107;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (periodForNote(mid, c2spd) <= period) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

void __not_in_flash_func(S3mPlayer::recomputeStep)(S3mChannel& c, uint32_t period) {
    c.stepPeriod = period;
    if (period && period < _minPeriod) period = _minPeriod;
    if (period > (1u << 24)) period = 1u << 24; // below 14 Hz; keeps the shifts below in 32 bits
    // freq16 = (CLOCK_LO << 16) / period, 8 bits at a time.
    uint32_t q1 = period ? CLOCK_LO / period : 0;
    uint32_t step23;
    if (period == 0) {
        step23 = 0;
    } else if (q1 >= 128) {
        step23 = MAX_STEP16 << 7; // freq16 << 9 would wrap; past the maximum anyway
    } else {
        uint32_t t = (CLOCK_LO % period) << 8;
        uint32_t q2 = t / period;
        t = (t % period) << 8;
        uint32_t freq16 = (q1 << 16) + (q2 << 8) + t / period;
        // freq16 * 2^19 / SAMPLE_RATE in two 32-bit steps (freq16 < 2^23).
        uint32_t a = freq16 << 9;
        uint32_t q = a / SAMPLE_RATE, r = a % SAMPLE_RATE;
        step23 = (q << 10) + (r << 10) / SAMPLE_RATE;
    }
    if (step23 > (MAX_STEP16 << 7)) step23 = MAX_STEP16 << 7;
    c.step23 = step23;
    c.sampleStep16 = step23 >> 7;
}

// Reads the CHUNK_FRAMES-aligned block holding `index` and converts it to
// signed 16-bit.
int16_t __not_in_flash_func(S3mPlayer::refillChunk)(TrackerVoice& v, uint32_t index) {
    const S3mSample& s = static_cast<const S3mSample&>(*v.smp);
    uint32_t bytesPerSample = s.is16Bit ? 2 : 1;
    uint32_t start = index & ~(uint32_t)(CHUNK_FRAMES - 1);
    uint32_t len = CHUNK_FRAMES;
    // A loop that fits in the cache is cached whole, so a wrap doesn't refill.
    if (v.looping && index >= s.loopStart && index < s.loopEnd && s.loopEnd - s.loopStart <= (uint32_t)CHUNK_FRAMES) {
        start = s.loopStart;
        len = s.loopEnd - s.loopStart;
    }
    if (start + len > s.length) len = s.length - start;
    v.chunkSmp = v.smp;
    v.chunkStart = start;
    // Frames past a short read or a failed seek stay zero and cached, so a
    // truncated sample doesn't hit the SD card on every frame.
    v.chunkLen = len;
    int16_t* out = v.chunkBuf;
    uint32_t got = 0;
    while (got < len) {
        uint32_t at = s.fileOffset + (start + got) * bytesPerSample;
        uint32_t piece = (len - got) * bytesPerSample;
        if (piece > rawSpan(at)) piece = rawSpan(at) & ~(bytesPerSample - 1);
        int n = 0;
        const uint8_t* raw = readSampleBytes(_file, at, piece, n);
        uint32_t frames = n > 0 ? (uint32_t)n / bytesPerSample : 0;
        int16_t* o = out + got;
        if (s.is16Bit) {
            for (uint32_t i = 0; i < frames; i++) {
                // Unbias in a wide type before narrowing.
                uint16_t u = (uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8);
                o[i] = s.isSigned ? (int16_t)u : (int16_t)((int32_t)u - 32768);
            }
        } else {
            for (uint32_t i = 0; i < frames; i++) {
                uint8_t b = raw[i];
                int16_t centred = s.isSigned ? (int8_t)b : (int16_t)((int)b - 128);
                o[i] = (int16_t)(centred << 8);
            }
        }
        got += frames;
        if (frames * bytesPerSample < piece) break;
    }
    for (uint32_t i = got; i < len; i++) out[i] = 0;
    return out[index - start];
}

void __not_in_flash_func(S3mPlayer::setSample)(S3mChannel& c, uint8_t sample) {
    c.sample = sample;
    c.smp = &_samples[sample - 1];
    c.looping = c.smp->loopEnd > c.smp->loopStart;
}

void __not_in_flash_func(S3mPlayer::startNote)(S3mChannel& c, uint32_t pos) {
    // The cache is kept: a retrigger of the same sample reuses it.
    if (c.smp) c.looping = c.smp->loopEnd > c.smp->loopStart;
    startVoiceAt(c, pos);
    // An offset past the end silences the note (ST3).
    if (!c.smp || pos >= c.smp->length) c.havePair = false;
}

void S3mPlayer::openPatternForScan(int num) {
    _openPatternNum = num;
    _rowOffsetsValid = 0;
    _scanCursor = (num >= 0 && num < MAX_PATTERNS) ? _patternOffsets[num] + 2 : 0; // +2 skips the pattern's own length prefix
}

void S3mPlayer::advanceToRow(int targetRow) {
    if (targetRow < 0 || targetRow > 63) return;

    if (targetRow < _rowOffsetsValid) {
        _patternFile.seekSet(_rowOffsets[targetRow]);
        decodeRow(true);
        return;
    }

    while (_rowOffsetsValid <= targetRow && _rowOffsetsValid < 64) {
        _rowOffsets[_rowOffsetsValid] = _scanCursor;
        _patternFile.seekSet(_scanCursor);
        bool isTarget = (_rowOffsetsValid == targetRow);
        decodeRow(isTarget); // earlier rows: parsed only, to find their length; the target row: applied
        _scanCursor = _patternFile.curPosition();
        _rowOffsetsValid++;
    }
}

void S3mPlayer::decodeRow(bool apply) {
    if (_openPatternNum < 0 || _openPatternNum >= MAX_PATTERNS || _patternOffsets[_openPatternNum] == 0) {
        return; // empty or missing pattern, a silent row
    }
    // A row has at most one entry per channel (32) before its terminator,
    // so a corrupt file without a 0x00 can't make this read on and on.
    for (int guard = 0; guard <= 32; guard++) {
        uint8_t flag;
        if (_patternFile.read(&flag, 1) != 1) return;
        if (flag == 0) return;

        uint8_t note = 0xFF, sample = 0, volume = 0xFF, command = 0, param = 0;
        if (flag & 0x20) {
            uint8_t nb[2];
            if (_patternFile.read(nb, 2) != 2) return;
            note = nb[0];
            sample = nb[1];
        }
        if (flag & 0x40) {
            if (_patternFile.read(&volume, 1) != 1) return;
        }
        if (flag & 0x80) {
            uint8_t cb[2];
            if (_patternFile.read(cb, 2) != 2) return;
            command = cb[0];
            param = cb[1];
        }
        uint8_t slot = _chanSlot[flag & 0x1F];
        if (apply && slot != 0xFF) applyCell(slot, note, sample, volume, command, param);
    }
}

// Instrument, note and volume column of one cell, on its row or SDx's tick.
// sample 0, note 0xFF and volume 0xFF are none.
void __not_in_flash_func(S3mPlayer::applyNoteColumns)(S3mChannel& c, uint8_t note, uint8_t sample, uint8_t volume, bool porta, bool hasOffset) {
    bool isNote = note < 0xF0;
    bool playing = c.smp && c.havePair && c.period && !c.cut;
    if (!isNote && note != 0xFE) note = 0xFF;
    if (sample > MAX_INSTRUMENTS) sample = 0;
    uint8_t rowSample = sample;
    // A lone instrument (without the SB swap) gives the next note without
    // one its sample and rate, but not its volume, as in OpenMPT.
    bool fromPending = false;
    if (isNote && !sample && c.pendIns) {
        sample = c.pendIns;
        fromPending = true;
    }
    if (isNote) c.pendIns = 0;
    if (sample) {
        const S3mSample& ns = _samples[sample - 1];
        bool hasData = ns.length > 0;
        if (note == 0xFF) {
            // An instrument alone resets the volume; on a SoundBlaster ST3 it
            // also swaps the sample under the playing note.
            if (hasData) c.volume = ns.volume * 4;
            if (_sampleSwap && c.smp != &ns) {
                uint32_t pos = c.srcIndex;
                setSample(c, sample);
                if (pos >= ns.length) c.havePair = false;
            } else if (!_sampleSwap) {
                c.pendIns = sample;
            }
        } else if (note == 0xFE) {
            // nothing: a cut ignores the instrument
        } else {
            bool changed = c.smp != &ns;
            if (hasData && !fromPending) c.volume = ns.volume * 4;
            if (porta && playing && (!changed || _portaKeepsSample)) {
                // The playing sample carries on; a new one gives only its rate.
                if (changed && hasData) c.c2spd = ns.c2spd;
            } else {
                if (porta && changed && playing) {
                    // Swap under the slide, keeping the position.
                    uint32_t pos = c.srcIndex;
                    setSample(c, sample);
                    if (pos >= ns.length) c.havePair = false;
                } else {
                    setSample(c, sample);
                }
                c.c2spd = ns.c2spd;
                c.portaTarget = 0;
            }
        }
    }
    if (note == 0xFE) {
        // ^^: silent until the next note.
        c.volume = 0;
        c.cut = true;
    } else if (isNote) {
        // Semitones 12-15 count on into the next octave, as in OpenMPT.
        int idx = (note >> 4) * 12 + (note & 0x0F);
        if (idx > 107) idx = 107;
        uint32_t period = periodForNote(idx, c.c2spd);
        c.note = (uint8_t)idx;
        if (porta && c.period && c.smp) {
            c.portaTarget = period;
        } else if (c.smp) {
            c.period = period;
            c.portaTarget = period;
            c.cut = false;
            c.vibPos = c.tremPos = 0;
            c.retrigCount = c.tremorCount = 0;
            c.panbDelta = 0;
            // A note with no instrument starts where the last offset left it (ST3).
            if (rowSample) c.prevOffset = 0;
            uint32_t pos = c.prevOffset;
            if (c.smp->length && pos >= c.smp->length) pos = c.smp->length - 1;
            if (!hasOffset) startNote(c, pos);
            recomputeStep(c, c.period);
        }
    }
    if (volume != 0xFF) {
        if (volume >= 128 && volume <= 192) c.pan = (uint16_t)((volume - 128) * 4);
        else c.volume = (uint16_t)((volume > 64 ? 64 : volume) * 4);
    }
}

// Oxx with SAx's high part, past a loop end wrapped into the loop.
void S3mPlayer::sampleOffset(S3mChannel& c) {
    uint32_t off = ((uint32_t)c.offset << 8) + ((uint32_t)c.hiOffset << 16);
    c.prevOffset = off;
    if (!c.smp) return;
    const TrackerSample& s = *c.smp;
    if (s.loopEnd > s.loopStart && off >= s.loopEnd) off = (off - s.loopStart) % (s.loopEnd - s.loopStart) + s.loopStart;
    startNote(c, off);
}

void S3mPlayer::applyCell(int ch, uint8_t note, uint8_t sample, uint8_t volume, uint8_t command, uint8_t param) {
    S3mChannel& c = _channels[ch];
    uint8_t p = param;
    if (command > CMD_Z) command = 0;

    // ST3 has one memory for most effects: any effect with a value sets it.
    if (_st3Memory && command && p) {
        c.volSlide = c.portaUp = c.portaDown = c.tremorParam = c.arpParam = c.retrigParam = c.cmdEx = p;
        c.tremDepth = (uint8_t)((p & 0x0F) << 2);
        c.tremSpeed = p >> 4;
    }
    switch (command) {
        case CMD_D: case CMD_K: case CMD_L:
            if (p) c.volSlide = p; else p = c.volSlide;
            break;
        case CMD_E:
            if (p) c.portaUp = c.portaDown = p; else p = c.portaDown;
            break;
        case CMD_F:
            if (p) c.portaUp = c.portaDown = p; else p = c.portaUp;
            break;
        case CMD_I:
            if (p) c.tremorParam = p; else p = c.tremorParam;
            break;
        case CMD_J:
            if (p) c.arpParam = p; else p = c.arpParam;
            break;
        case CMD_Q:
            if (p) c.retrigParam = p; else p = c.retrigParam;
            break;
        case CMD_S:
            if (p) c.cmdEx = p; else p = c.cmdEx;
            break;
        default:
            break;
    }

    // SD0 skips the whole cell.
    if (command == CMD_S && (p >> 4) == 0xD && (p & 0x0F) == 0) return;

    c.cmd = command;
    c.param = p;
    c.fineVibrato = command == CMD_U;
    bool porta = (command == CMD_G || command == CMD_L) && note != 0xFF;
    bool isNote = note < 0xF0;
    bool hasOffset = command == CMD_O && isNote && !porta;

    if (command == CMD_S && (p >> 4) == 0xD) {
        // SDx holds the whole cell until tick x.
        c.delayTick = p & 0x0F;
        c.pendNote = note;
        c.pendSample = sample;
        c.pendVolume = volume;
    } else {
        // SAx is read before the offset it adds to.
        if (command == CMD_S && (p >> 4) == 0xA) c.hiOffset = p & 0x0F;
        if (command == CMD_O && p) c.offset = p;
        applyNoteColumns(c, note, sample, volume, porta, hasOffset);
        if (hasOffset && c.smp) sampleOffset(c);
    }

    // Effects set once on the row; the rest run in runTick().
    switch (command) {
        case CMD_A:
            if (p) _speed = p;
            break;
        case CMD_B:
            _positionJumpPending = true;
            _positionJumpTarget = p;
            break;
        case CMD_C: {
            int row = (p >> 4) * 10 + (p & 0x0F);
            if (row < 64) { // ST3 ignores a break past the pattern
                _patternBreakPending = true;
                _patternBreakRow = row;
            }
            break;
        }
        case CMD_G:
            if (p) c.portaSlide = p;
            break;
        case CMD_H: case CMD_U:
            if (p & 0x0F) c.vibDepth = (uint8_t)((p & 0x0F) * 4);
            if (p & 0xF0) c.vibSpeed = p >> 4;
            break;
        case CMD_R:
            if (p & 0x0F) c.tremDepth = (uint8_t)((p & 0x0F) * 4);
            if (p & 0xF0) c.tremSpeed = p >> 4;
            break;
        case CMD_Y:
            if (p & 0x0F) c.panbDepth = p & 0x0F;
            if (p & 0xF0) c.panbSpeed = p >> 4;
            break;
        case CMD_T:
            if (p) c.tempo = p; else p = c.tempo;
            if (p >= 0x20) _tempo = p;
            c.param = p;
            break;
        case CMD_V:
            if (p <= 64) _songVolume = p; // ST3 ignores anything higher
            break;
        case CMD_W:
            _globalSlideRow = true;
            if (p) _globalSlide = p;
            break;
        case CMD_X:
            if (p <= 0x80) c.pan = (uint16_t)(p * 2);
            else if (p == 0xA4) c.pan = 128; // surround plays centred
            break;
        case CMD_S:
            switch (p >> 4) {
                case 0x1: c.glissando = (p & 0x0F) != 0; break;
                case 0x2:
                    c.c2spd = FINETUNE_C2SPD[p & 0x0F];
                    if (c.period) c.period = periodForNote(c.note, c.c2spd);
                    break;
                case 0x3: c.vibType = p & 3; break;
                case 0x4: c.tremType = p & 3; break;
                case 0x5: c.panbType = p & 3; break;
                case 0x8: c.pan = (uint16_t)(((p & 0x0F) * 256 + 8) / 15); break;
                case 0xB: {
                    uint8_t val = p & 0x0F;
                    if (val == 0) {
                        _loopRow = (uint8_t)_row;
                    } else if (_loopCount) {
                        if (--_loopCount == 0) _loopRow = (uint8_t)(_row + 1);
                        else _loopPending = true;
                    } else {
                        _loopCount = val;
                        _loopPending = true;
                    }
                    break;
                }
                case 0xE:
                    // The first SEx on a row wins; SE0 does nothing.
                    if ((p & 0x0F) && !_rowRepeats) _rowRepeats = p & 0x0F;
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

void S3mPlayer::advanceRow() {
    // SEx: run the row again, without its notes.
    if (_rowRepeats) {
        _rowRepeats--;
        runTick(true);
        return;
    }
    _globalSlideRow = false;
    for (int ch = 0; ch < _numChannels; ch++) {
        S3mChannel& c = _channels[ch];
        c.cmd = c.param = 0;
        c.delayTick = 0;
        c.fineVibrato = false;
    }

    // A pattern loop wins over a break or jump on the same row.
    if (_loopPending) {
        _patternBreakPending = true;
        _patternBreakRow = _loopRow;
        _positionJumpPending = true;
        _positionJumpTarget = _orderPos;
        _loopPending = false;
    } else if (_positionJumpPending ? _positionJumpTarget != _orderPos : _patternBreakPending) {
        _loopCount = 0; // leaving the pattern
    }
    stepPosition(64, 63);

    // Order table: 0xFF ends the song, 0xFE is skipped.
    while (true) {
        if (_orderPos < 0 || _orderPos >= _songLength) {
            _state = STATE_DONE;
            return;
        }
        uint8_t marker = _orderTable[_orderPos];
        if (marker == 0xFF) {
            _state = STATE_DONE;
            return;
        }
        if (marker == 0xFE) {
            _orderPos++;
            _row = 0;
            continue;
        }
        break;
    }

    int patternNum = _orderTable[_orderPos];
    if (patternNum != _openPatternNum) openPatternForScan(patternNum);
    advanceToRow(_row);
    runTick(true);
}

void __not_in_flash_func(S3mPlayer::processTickEffects)() {
    runTick(false);
}

// Moves a period as OpenMPT's DoFreqSlide does: negative raises it. Past
// the top the note stops, except for a tone porta.
void __not_in_flash_func(S3mPlayer::slidePeriod)(S3mChannel& c, int32_t amount, bool tonePorta) {
    if (!c.period || !amount) return;
    int32_t p = (int32_t)c.period - amount * 256;
    if (p < 256) {
        p = 256;
        if (!tonePorta) c.cut = true;
    }
    // Hz-based files have no upper limit; stop at the lowest pitch recomputeStep plays.
    if (p > (1 << 24)) p = 1 << 24;
    c.period = (uint32_t)p;
}

// D, K and L. x0 slides up and 0y down on the ticks after the first, y
// first when both are set; xF and Fy are fine slides on the first tick;
// 0F and F0 slide 15 on every tick.
void __not_in_flash_func(S3mPlayer::slideVolume)(S3mChannel& c, bool firstTick) {
    uint8_t p = c.param, hi = p >> 4, lo = p & 0x0F;
    int v = c.volume;
    if (lo == 0x0F) {
        if (hi) {
            if (firstTick) c.volume = (uint16_t)min(256, v + hi * 4);
            return;
        }
        if (firstTick && !_fastSlides) v -= 15 * 4;
    } else if (hi == 0x0F) {
        if (lo) {
            if (firstTick) c.volume = (uint16_t)max(0, v - lo * 4);
            return;
        }
        if (firstTick && !_fastSlides) v += 15 * 4;
    }
    if (!firstTick || _fastSlides) {
        if (lo) v -= lo * 4;
        else v += hi * 4;
    }
    c.volume = (uint16_t)(v < 0 ? 0 : v > 256 ? 256 : v);
}

// Qxy: every y ticks counted across rows, restart the sample and change
// the volume by x.
void __not_in_flash_func(S3mPlayer::retrig)(S3mChannel& c) {
    if (c.cut) return;
    uint8_t speed = c.param & 0x0F;
    if (!speed) speed = 1;
    bool doRetrig = c.retrigCount && (c.retrigCount % speed) == 0;
    c.retrigCount++;
    if (!doRetrig) return;
    uint8_t dv = c.param >> 4;
    if (dv) {
        int v = c.volume;
        if (RETRIG_MUL[dv]) v = v * RETRIG_MUL[dv] / 16;
        else v += RETRIG_ADD[dv] * 4;
        c.volume = (uint16_t)(v < 0 ? 0 : v > 256 ? 256 : v);
    }
    c.prevOffset = 0;
    if (c.smp && c.period) startNote(c, 0);
}

void __not_in_flash_func(S3mPlayer::runTick)(bool firstTick) {
    // W: fine on the first tick, else on the ticks after.
    if (_globalSlideRow) {
        uint8_t p = _globalSlide, hi = p >> 4, lo = p & 0x0F;
        int d = 0;
        if (lo == 0x0F && hi) { if (firstTick) d = hi; }
        else if (hi == 0x0F && lo) { if (firstTick) d = -lo; }
        else if (!firstTick) d = hi ? hi : -lo;
        int g = (int)_songVolume + d;
        _songVolume = (uint8_t)(g < 0 ? 0 : g > 64 ? 64 : g);
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        S3mChannel& c = _channels[ch];
        uint8_t p = c.param;

        if (c.delayTick && _tick == c.delayTick) {
            applyNoteColumns(c, c.pendNote, c.pendSample, c.pendVolume, false, false);
        }

        switch (c.cmd) {
            case CMD_D:
                slideVolume(c, firstTick);
                break;
            case CMD_K:
                slideVolume(c, firstTick);
                break;
            case CMD_E: case CMD_F: {
                int32_t dir = c.cmd == CMD_E ? -1 : 1; // E lowers the pitch (raises the period)
                if (p >= 0xE0) {
                    if ((p & 0x0F) && firstTick && c.period) {
                        slidePeriod(c, dir * (int32_t)((p & 0x0F) * ((p & 0xF0) == 0xF0 ? 4 : 1)), false);
                        if (!_hertz && c.period > (0xFFFFu << 8)) c.period = 0xFFFFu << 8;
                    }
                } else if (!firstTick) {
                    slidePeriod(c, dir * (int32_t)p * 4, false);
                }
                break;
            }
            case CMD_G: case CMD_L:
                if (c.cmd == CMD_L) slideVolume(c, firstTick);
                if (!firstTick && c.period && c.portaTarget) {
                    int32_t delta = (int32_t)c.portaSlide * 4;
                    if (c.period < c.portaTarget) {
                        slidePeriod(c, -delta, true);
                        if (c.period > c.portaTarget) c.period = c.portaTarget;
                    } else if (c.period > c.portaTarget) {
                        slidePeriod(c, delta, true);
                        if (c.period < c.portaTarget) c.period = c.portaTarget;
                    }
                }
                break;
            case CMD_Q:
                retrig(c);
                break;
            case CMD_S:
                // SCx cuts on tick x; SC0 does nothing.
                if ((p >> 4) == 0xC && (p & 0x0F) && _tick == (p & 0x0F)) {
                    c.cut = true;
                }
                break;
            case CMD_T:
                if (!firstTick && p < 0x20) {
                    int t = (int)_tempo + ((p & 0xF0) == 0x10 ? (p & 0x0F) : -(int)(p & 0x0F));
                    _tempo = (uint16_t)(t < 32 ? 32 : t > 255 ? 255 : t);
                }
                break;
            default:
                break;
        }

        // What plays this tick: volume with tremolo and tremor, pitch with
        // arpeggio and vibrato, pan with panbrello.
        if (c.period && _upperLimit && c.period > TOP_PERIOD) c.period = TOP_PERIOD;
        if (c.period && _amigaLimits) c.period = c.period < AMIGA_LOW ? AMIGA_LOW : c.period > AMIGA_HIGH ? AMIGA_HIGH : c.period;

        int vol = c.cut ? 0 : c.volume;
        if (c.cmd == CMD_R) {
            if (vol > 0) vol += waveform(c.tremType, c.tremPos) * (int)c.tremDepth / 64;
            if (!firstTick) c.tremPos = (uint8_t)(c.tremPos + c.tremSpeed);
        }
        if (c.cmd == CMD_I) {
            uint8_t on = (c.tremorParam >> 4) + 1, n = (uint8_t)(on + (c.tremorParam & 0x0F) + 1);
            uint8_t count = c.tremorCount;
            if (count >= n) count = 0;
            if (count >= on) vol = 0;
            c.tremorCount = (uint8_t)(count + 1);
        }
        c.mixVolume = (uint8_t)((vol < 0 ? 0 : vol > 256 ? 256 : vol) >> 2);

        if (c.cmd == CMD_Y) {
            c.panbDelta = (int16_t)((waveform(c.panbType, (uint8_t)((c.panbPos + 0x10) >> 2)) * (int)c.panbDepth + 2) / 8);
            c.panbPos = (uint8_t)(c.panbPos + c.panbSpeed);
        } else {
            c.panbDelta = 0;
        }

        if (!c.period) continue;
        uint32_t period = c.period;
        if (c.glissando && (c.cmd == CMD_G || c.cmd == CMD_L)) period = periodForNote(noteForPeriod(period, c.c2spd), c.c2spd);
        if (c.cmd == CMD_J) {
            uint32_t semis = 0;
            switch (_tick % 3) {
                case 1: semis = c.param >> 4; break;
                case 2: semis = c.param & 0x0F; break;
            }
            if (semis) period = periodForNote(c.note + (int)semis, c.c2spd);
            if (_amigaLimits) period = period < AMIGA_LOW ? AMIGA_LOW : period > AMIGA_HIGH ? AMIGA_HIGH : period;
        }
        if (c.cmd == CMD_H || c.cmd == CMD_K || c.cmd == CMD_U) {
            int shift = (_oldVibrato ? 5 : 6) + (c.fineVibrato ? 2 : 0);
            int32_t delta = (-waveform(c.vibType, c.vibPos) * (int32_t)c.vibDepth) / (1 << shift);
            int32_t pp = (int32_t)period - delta * 256;
            if (delta && pp < 256) {
                pp = 256;
                c.cut = true;
            }
            period = (uint32_t)pp;
            if (!firstTick) c.vibPos = (uint8_t)(c.vibPos + c.vibSpeed);
        }
        if (_limitPeriod && period <= _minPeriod) c.cut = true;
        if (period != c.stepPeriod) recomputeStep(c, period);
    }
}

void __not_in_flash_func(S3mPlayer::tickEnd)() {
    _globalVolume = (uint8_t)((_songVolume * _preamp + 24) / 48);
    for (int ch = 0; ch < _numChannels; ch++) {
        S3mChannel& c = _channels[ch];
        if (c.cut) {
            c.mixVolume = 0;
            c.havePair = false;
        }
        int p = (int)c.pan + c.panbDelta;
        c.mixPan = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
        // The step's 7 bits below 16.16 are spread over ticks, so a long
        // note keeps in phase with OpenMPT's finer step.
        if (c.step23) {
            uint32_t acc = c.stepAcc + (c.step23 & 127);
            c.sampleStep16 = (c.step23 >> 7) + (acc >> 7);
            c.stepAcc = (uint8_t)(acc & 127);
        }
    }
}
