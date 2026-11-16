#include "sd_card.h"
#include "mod_file.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>

namespace {

// ProTracker's periods for 6 octaves, finetune 0.
const uint16_t PERIOD_TABLE[72] = {
    1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907,
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
    107, 101, 95, 90, 85, 80, 75, 71, 67, 63, 60, 56,
    53, 50, 47, 45, 42, 40, 37, 35, 33, 31, 30, 28,
};

// ProTracker's lowest octave for each finetune (0..7, then -8..-1), as
// OpenMPT has it. Higher octaves halve it in quarter periods.
const uint16_t TUNED_PERIODS[16][12] = {
    {1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907},
    {1700, 1604, 1514, 1430, 1348, 1274, 1202, 1134, 1070, 1010, 954, 900},
    {1688, 1592, 1504, 1418, 1340, 1264, 1194, 1126, 1064, 1004, 948, 894},
    {1676, 1582, 1492, 1408, 1330, 1256, 1184, 1118, 1056, 996, 940, 888},
    {1664, 1570, 1482, 1398, 1320, 1246, 1176, 1110, 1048, 990, 934, 882},
    {1652, 1558, 1472, 1388, 1310, 1238, 1168, 1102, 1040, 982, 926, 874},
    {1640, 1548, 1460, 1378, 1302, 1228, 1160, 1094, 1032, 974, 920, 868},
    {1628, 1536, 1450, 1368, 1292, 1220, 1150, 1086, 1026, 968, 914, 862},
    {1814, 1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960},
    {1800, 1700, 1604, 1514, 1430, 1350, 1272, 1202, 1134, 1070, 1010, 954},
    {1788, 1688, 1592, 1504, 1418, 1340, 1264, 1194, 1126, 1064, 1004, 948},
    {1774, 1676, 1582, 1492, 1408, 1330, 1256, 1184, 1118, 1056, 996, 940},
    {1762, 1664, 1570, 1482, 1398, 1320, 1246, 1176, 1110, 1048, 988, 934},
    {1750, 1652, 1558, 1472, 1388, 1310, 1238, 1168, 1102, 1040, 982, 926},
    {1736, 1640, 1548, 1460, 1378, 1302, 1228, 1160, 1094, 1032, 974, 920},
    {1724, 1628, 1536, 1450, 1368, 1292, 1220, 1150, 1086, 1026, 968, 914},
};

// ProTracker's vibrato and tremolo sine, half a cycle.
const uint8_t SINE_TABLE[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24,
};

// PAL Amiga period clock times 4 (quarter periods) times 16, for
// OpenMPT's sample rate in 1/16 Hz.
const uint32_t CLOCK_X64 = 3546895u * 64u;

// Lowest and highest periods a finetune can reach (OpenMPT's Amiga
// limits); the Amiga can't play above period 113.
inline uint16_t limitLow(uint8_t ft) {
    uint16_t lo = TUNED_PERIODS[ft][11] >> 1;
    return lo < 113 * 4 ? 113 * 4 : lo;
}
inline uint16_t limitHigh(uint8_t ft) { return (uint16_t)(TUNED_PERIODS[ft][0] << 1); }

bool matchesFormatTag(const char* tag, int& outChannels, bool& flt8) {
    flt8 = false;
    static const char* const four[] = {"M.K.", "M!K!", "M&K!", "FLT4", "N.T.", "FEST", "NSMS", "LARD", "PATT"};
    for (const char* t : four) {
        if (strncmp(tag, t, 4) == 0) { outChannels = 4; return true; }
    }
    if (strncmp(tag, "8CHN", 4) == 0 || strncmp(tag, "CD81", 4) == 0 ||
        strncmp(tag, "OKTA", 4) == 0 || strncmp(tag, "OCTA", 4) == 0) {
        outChannels = 8;
        return true;
    }
    if (strncmp(tag, "FLT8", 4) == 0) {
        outChannels = 8;
        flt8 = true;
        return true;
    }
    if (strncmp(tag, "TDZ", 3) == 0 && tag[3] >= '1' && tag[3] <= '3') {
        outChannels = tag[3] - '0';
        return true;
    }
    // Generic "xCHN" (1 digit) and "xxCH"/"xxCN" (2 digit) numeric conventions.
    if (tag[0] >= '0' && tag[0] <= '9') {
        if (tag[1] >= '0' && tag[1] <= '9' && tag[2] == 'C' && (tag[3] == 'H' || tag[3] == 'N')) {
            outChannels = (tag[0] - '0') * 10 + (tag[1] - '0');
            return outChannels > 0 && outChannels <= ModPlayer::MAX_MOD_CHANNELS;
        }
        if (tag[1] == 'C' && tag[2] == 'H' && tag[3] == 'N') {
            outChannels = tag[0] - '0';
            return outChannels > 0;
        }
    }
    return false;
}

} // namespace

bool ModPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!sdOpenPath(_file, path, O_RDONLY)) return loadFail("could not open file");

    // A 31-sample MOD has a format tag at 1080. Without one it's read as
    // a 15-sample Soundtracker file: no tag, 4 channels, patterns at 600.
    char tag[4];
    bool tagged = _file.seekSet(1080) && _file.read(tag, 4) == 4 && matchesFormatTag(tag, _numChannels, _flt8);
    if (!tagged) {
        _numChannels = 4;
        _flt8 = false;
    }
    int numSamples = tagged ? MAX_SAMPLES : 15;

    _numInstruments = 0;
    bool repLen0 = false; // a sample with a loop length of 0, which ProTracker doesn't write
    _file.seekSet(20); // skip song name
    for (int i = 0; i < MAX_SAMPLES; i++) {
        ModSample& s = _samples[i];
        if (i >= numSamples) {
            s = ModSample();
            continue;
        }
        uint8_t hdr[30];
        if (_file.read(hdr, 30) != 30) return loadFail("invalid MOD file");
        s.length = (((uint32_t)hdr[22] << 8) | hdr[23]) * 2;
        s.finetune = hdr[24] & 0x0F;
        s.volume = hdr[25] > 64 ? 64 : hdr[25];
        // "No loop" is a loop length of 1 word (or 0), checked on the raw
        // word value before the *2 byte conversion.
        uint16_t rawLoopStart = ((uint16_t)hdr[26] << 8) | hdr[27];
        uint16_t rawLoopLen = ((uint16_t)hdr[28] << 8) | hdr[29];
        if (s.length > 0 && rawLoopLen == 0) repLen0 = true;
        if (rawLoopLen <= 1) {
            s.loopStart = 0;
            s.loopEnd = 0;
        } else {
            s.loopStart = tagged ? (uint32_t)rawLoopStart * 2 : rawLoopStart; // Soundtracker's is in bytes
            s.loopEnd = s.loopStart + (uint32_t)rawLoopLen * 2;
            // A loop running past the end is cut to the sample.
            if (s.loopEnd > s.length) s.loopEnd = s.length;
            if (s.loopStart + 2 > s.loopEnd) s.loopStart = s.loopEnd = 0;
        }
        if (s.length > 0) _numInstruments++;
    }

    uint8_t songLenByte, restartByte;
    if (_file.read(&songLenByte, 1) != 1 || _file.read(&restartByte, 1) != 1) return loadFail("invalid MOD file");
    _songLength = songLenByte > 128 ? 128 : songLenByte;
    _restartPos = tagged ? restartByte : 0; // Soundtracker uses this byte for tempo

    if (_file.read(_orderTable, 128) != 128) return loadFail("invalid MOD file");

    // Patterns past the song length are still stored, so the highest
    // pattern in all 128 orders sets where the sample data starts.
    int maxPattern = 0;
    for (int i = 0; i < 128; i++) {
        if (_orderTable[i] > maxPattern) maxPattern = _orderTable[i];
    }
    _numPatterns = maxPattern + 1;
    if (_flt8) {
        // FLT8 orders count 4-channel halves, two to a pattern.
        for (int i = 0; i < 128; i++) _orderTable[i] >>= 1;
        _numPatterns = maxPattern / 2 + 1;
    }

    _patternDataStart = 20 + (uint32_t)numSamples * 30 + 2 + 128 + (tagged ? 4 : 0);
    uint32_t sampleDataStart = _patternDataStart + (uint32_t)_numPatterns * _numChannels * 64 * 4;

    // ProTracker mode, as OpenMPT: M.K. and M!K! files with 4 channels,
    // notes that all fit ProTracker's 3 octaves and no loop length of 0.
    _ptMode = tagged && !repLen0 && _numChannels == 4 && (strncmp(tag, "M.K.", 4) == 0 || strncmp(tag, "M!K!", 4) == 0);
    _startrekker = tagged && strncmp(tag, "FLT", 3) == 0;
    for (uint32_t pos = _patternDataStart; _ptMode && pos < sampleDataStart;) {
        uint32_t n = sampleDataStart - pos;
        if (n > RAW_SCRATCH_BYTES) n = RAW_SCRATCH_BYTES;
        if (!_file.seekSet(pos) || _file.read(_rawScratch, n) != (int)n) break;
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            uint16_t p = (uint16_t)(((_rawScratch[i] & 0x0F) << 8) | _rawScratch[i + 1]);
            int note = p ? noteForPeriod(p) : 12;
            if (note < 12 || note > 47) _ptMode = false;
        }
        pos += n;
    }
    // Checks an untagged file looks like Soundtracker before playing it.
    if (!tagged && (songLenByte == 0 || songLenByte > 128 || maxPattern >= 64 || sampleDataStart > _file.fileSize())) {
        return loadFail("unsupported format");
    }
    uint32_t offset = sampleDataStart;
    for (int i = 0; i < MAX_SAMPLES; i++) {
        _samples[i].fileOffset = offset;
        offset += _samples[i].length;
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        c = ModChannel();
        // Channels go L,R,R,L at OpenMPT's MOD default, 75% to one side.
        c.pan = ((ch & 3) == 0 || (ch & 3) == 3) ? 64 : 192;
        c.mixPan = c.pan;
    }
    attachVoices(&_channels[0], sizeof(ModChannel), _numChannels, &_chunks[0][0], CHUNK_FRAMES);
    useSampleFile(_file);
    attachTails(_tails, (int)(sizeof(_tails) / sizeof(_tails[0])), _tailPool, TAIL_POOL_FRAMES);

    _globalVolume = 64;
    // OpenMPT's pre-amp for MODs, 256 / channels within 32-128: the most
    // that can't clip with every channel at full volume, up to 8 channels.
    int pre = 256 / (_numChannels > 0 ? _numChannels : 4);
    _masterGain = masterGainFor(MIX_COMPATIBLE, (uint32_t)(pre < 32 ? 32 : pre > 128 ? 128 : pre), _numChannels);
    _nextTempo = 0;
    resetSequencer(6, 125);
    return true;
}

int ModPlayer::noteForPeriod(uint16_t rawPeriod) {
    int best = 0, bestDiff = 0xFFFF;
    for (int i = 0; i < NUM_NOTES; i++) {
        int diff = abs((int)rawPeriod - (int)PERIOD_TABLE[i]);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

uint16_t __not_in_flash_func(ModPlayer::periodForNote)(int note, uint8_t finetune) {
    if (note < 0) note = 0;
    if (note >= NUM_NOTES) note = NUM_NOTES - 1;
    if (finetune == 0) return (uint16_t)(PERIOD_TABLE[note] << 2);
    return (uint16_t)(((uint32_t)TUNED_PERIODS[finetune][note % 12] << 2) >> (note / 12));
}

int __not_in_flash_func(ModPlayer::noteAtOrAbove)(uint16_t period, uint8_t finetune) {
    // Periods fall as notes rise, so binary search for the first one at or below.
    int lo = 0, hi = NUM_NOTES - 1;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (periodForNote(mid, finetune) <= period) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

uint16_t __not_in_flash_func(ModPlayer::clampPeriod)(uint16_t period, uint8_t finetune) const {
    if (_ptMode) {
        uint16_t lo = limitLow(finetune), hi = limitHigh(finetune);
        return period < lo ? lo : period > hi ? hi : period;
    }
    return period < 1 ? 1 : period;
}

void __not_in_flash_func(ModPlayer::recomputeStep)(ModChannel& c, uint16_t period) {
    c.stepPeriod = period;
    uint32_t step23 = 0;
    if (period == 0) {
        step23 = 0;
    } else if (period < 28) {
        // freq16 << 9 would wrap; the step is past the maximum anyway.
        step23 = MAX_STEP16 << 7;
    } else {
        // freq16 * 2^19 / SAMPLE_RATE in two 32-bit steps (freq16 < 2^23).
        uint32_t freq16 = CLOCK_X64 / period;
        uint32_t a = freq16 << 9;
        uint32_t q = a / SAMPLE_RATE, r = a % SAMPLE_RATE;
        step23 = (q << 10) + (r << 10) / SAMPLE_RATE;
    }
    if (step23 > (MAX_STEP16 << 7)) step23 = MAX_STEP16 << 7;
    c.step23 = step23;
    c.sampleStep16 = step23 >> 7;
}

// Reads the CHUNK_FRAMES-aligned block holding `index`.
int16_t __not_in_flash_func(ModPlayer::refillChunk)(TrackerVoice& v, uint32_t index) {
    const TrackerSample& s = *v.smp;
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
    uint32_t got = 0;
    while (got < len) {
        uint32_t at = s.fileOffset + start + got, piece = len - got;
        if (piece > rawSpan(at)) piece = rawSpan(at);
        int n = 0;
        const uint8_t* raw = readSampleBytes(_file, at, piece, n);
        for (int i = 0; i < n; i++) v.chunkBuf[got + i] = (int16_t)((int8_t)raw[i]) << 8;
        got += (uint32_t)n;
        if ((uint32_t)n < piece) break;
    }
    for (uint32_t i = got; i < len; i++) v.chunkBuf[i] = 0;
    return v.chunkBuf[index - start];
}

void ModPlayer::triggerNote(int ch, bool newNote) {
    ModChannel& c = _channels[ch];
    c.smp = c.sample ? &_samples[c.sample - 1] : nullptr;
    c.swapTo = nullptr;
    recomputeStep(c);
    if (c.smp) c.looping = (c.smp->loopEnd - c.smp->loopStart) > 1;
    if (newNote) {
        if (!(c.vibratoWave & 4)) c.vibratoPos = 0;
        if (!(c.tremoloWave & 4)) c.tremoloPos = 0;
    }
    startAt(c, c.startOffset);
}

// ProTracker's sample swap: a sample number without a note (or with tone
// portamento) takes over when the playing sample reaches its loop end or
// its end, from the new sample's loop. Checked once a tick.
void __not_in_flash_func(ModPlayer::swapSample)(ModChannel& c) {
    const TrackerSample& old = *c.smp;
    bool wrapped = c.looping && c.srcIndex < c.swapLastPos;
    c.swapLastPos = c.srcIndex;
    if (c.havePair && !wrapped) return;
    const ModSample* to = c.swapTo;
    c.swapTo = nullptr;
    uint32_t into = wrapped && c.srcIndex >= old.loopStart ? c.srcIndex - old.loopStart : 0;
    c.smp = to;
    c.looping = (to->loopEnd - to->loopStart) > 1;
    if (c.looping) {
        startVoiceAt(c, to->loopStart + into % (to->loopEnd - to->loopStart));
    } else {
        startVoiceAt(c, 0);
        c.havePair = false;
    }
}

void ModPlayer::startAt(ModChannel& c, uint32_t pos) {
    // Past the end, a looped sample plays its loop and any other is silent.
    if (c.smp && !c.startPastEnd && pos < c.smp->length) {
        startVoiceAt(c, pos);
    } else if (c.smp) {
        startVoiceAt(c, c.looping ? c.smp->loopStart : 0);
        if (!c.looping) c.havePair = false;
    }
}

void __not_in_flash_func(ModPlayer::tickStart)() {
    if (_nextTempo) {
        _tempo = _nextTempo;
        _nextTempo = 0;
    }
}

void __not_in_flash_func(ModPlayer::tickEnd)() {
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        int v = (int)c.volume + c.tremoloDelta;
        c.mixVolume = (uint8_t)(v < 0 ? 0 : v > 64 ? 64 : v);
        c.mixPan = c.pan;
        if (c.swapTo) swapSample(c);
        // The step's 7 bits below 16.16 are spread over ticks, so a long
        // note keeps in phase with OpenMPT's finer step.
        if (c.step23) {
            uint32_t acc = c.stepAcc + (c.step23 & 127);
            c.sampleStep16 = (c.step23 >> 7) + (acc >> 7);
            c.stepAcc = (uint8_t)(acc & 127);
        }
    }
}

void ModPlayer::advanceRow() {
    // Per-row effect state resets before this row's cells; porta/vibrato/
    // slide speeds persist until changed.
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];
        c.fx = 0;
        c.fxParam = 0;
        c.retrigParam = 0;
        c.noteDelayParam = 0;
        c.noteCutParam = 0xFF;
    }

    stepPosition(64, 63);

    if (_orderPos >= _songLength || _orderPos < 0) {
        if (_restartPos >= 0 && _restartPos < _songLength) {
            _orderPos = _restartPos;
            _row = 0;
        } else {
            _state = STATE_DONE;
            return;
        }
    }

    int patternNum = _orderTable[_orderPos];
    if (patternNum >= _numPatterns) patternNum = 0;

    uint8_t rowBuf[MAX_MOD_CHANNELS * 4];
    bool ok;
    if (_flt8) {
        // Channels 0-3 and 4-7 are two 4-channel patterns one after the other.
        uint32_t left = _patternDataStart + ((uint32_t)patternNum * 128 + (uint32_t)_row) * 16;
        ok = _file.seekSet(left) && _file.read(rowBuf, 16) == 16 &&
             _file.seekSet(left + 64 * 16) && _file.read(rowBuf + 16, 16) == 16;
    } else {
        uint32_t rowOffset = _patternDataStart + (uint32_t)patternNum * _numChannels * 64 * 4 + (uint32_t)_row * _numChannels * 4;
        int need = _numChannels * 4;
        ok = _file.seekSet(rowOffset) && _file.read(rowBuf, need) == need;
    }
    if (!ok) {
        _state = STATE_DONE; // truncated file mid-song
        return;
    }

    for (int ch = 0; ch < _numChannels; ch++) {
        processCellTriggers(ch, &rowBuf[ch * 4]);
        // A row starts at the base pitch, dropping last tick's vibrato or arpeggio.
        ModChannel& c = _channels[ch];
        if (c.stepPeriod != c.period) recomputeStep(c);
    }
}

void ModPlayer::processCellTriggers(int ch, const uint8_t cell[4]) {
    ModChannel& c = _channels[ch];

    uint8_t sampleNum = (uint8_t)((cell[0] & 0xF0) | (cell[2] >> 4));
    uint16_t rawPeriod = (uint16_t)(((cell[0] & 0x0F) << 8) | cell[1]);
    uint8_t effect = cell[2] & 0x0F;
    uint8_t param = cell[3];

    bool isTonePorta = (effect == 0x3 || effect == 0x5);
    bool isNoteDelay = (effect == 0xE && (param >> 4) == 0xD && (param & 0x0F) != 0);

    // A sample number sets the volume and finetune now, even with a note
    // delay; the sample itself starts with the next note.
    if (sampleNum >= 1 && sampleNum <= MAX_SAMPLES) {
        c.sample = sampleNum;
        c.startOffset = 0;
        c.startPastEnd = false;
        c.finetune = _samples[sampleNum - 1].finetune;
        c.volume = _samples[sampleNum - 1].volume;
    }
    // E5x sets the finetune for this row's note too.
    if (effect == 0xE && (param >> 4) == 0x5) c.finetune = param & 0x0F;
    // Tremolo's volume holds through the next row's first tick, as on the
    // Amiga, unless that row sets the volume.
    if (rawPeriod || sampleNum || effect == 0xC || (effect == 0xE && ((param >> 4) == 0xA || (param >> 4) == 0xB))) c.tremoloDelta = 0;
    uint16_t period = rawPeriod ? clampPeriod(periodForNote(noteForPeriod(rawPeriod), c.finetune), c.finetune) : 0;
    if (_ptMode && sampleNum >= 1 && sampleNum <= MAX_SAMPLES && (period == 0 || isTonePorta) &&
        c.smp && c.smp != &_samples[sampleNum - 1] && c.smp->length > 0) {
        c.swapTo = &_samples[sampleNum - 1];
        c.swapLastPos = c.srcIndex;
    }

    if (isNoteDelay) {
        c.noteDelayParam = param & 0x0F;
        c.noteDelayPeriod = period; // 0 if this cell has no note of its own
    } else if (period != 0) {
        if (isTonePorta) {
            c.portaTarget = period;
        } else {
            c.period = period;
            triggerNote(ch, true);
        }
    }

    c.fx = effect;
    c.fxParam = param;
    switch (effect) {
        case 0x0: // Arpeggio, 1xx/2xx porta, Axy volume slide: ticks 1+ only, see processTickEffects()
        case 0x1:
        case 0x2:
        case 0xA:
            break;
        case 0x3: // Tone portamento
            if (param != 0) c.tonePortaSpeed = param;
            break;
        case 0x4: // Vibrato
            if (param >> 4) c.vibratoSpeed = param >> 4;
            if (param & 0x0F) c.vibratoDepth = param & 0x0F;
            break;
        case 0x5: // Tone portamento + volume slide
        case 0x6: // Vibrato + volume slide
            break;
        case 0x7: // Tremolo
            if (param >> 4) c.tremoloSpeed = param >> 4;
            if (param & 0x0F) c.tremoloDepth = param & 0x0F;
            break;
        case 0x8: // Set panning
            c.pan = param;
            break;
        case 0x9: { // Sample offset
            if (param != 0) c.sampleOffsetParam = param;
            if (period != 0 && !isTonePorta && !isNoteDelay && c.smp) {
                uint32_t off = (uint32_t)c.sampleOffsetParam << 8;
                if (!_ptMode) {
                    startAt(c, off);
                } else if (!c.startPastEnd) {
                    // ProTracker adds the offset to the sample start twice,
                    // once before the note starts and once after, so later
                    // notes without a sample number start further in.
                    uint32_t left = c.smp->length > c.startOffset ? c.smp->length - c.startOffset : 0;
                    if (off < left) {
                        c.startOffset += off;
                        startAt(c, c.startOffset);
                        if (off < left - off) c.startOffset += off;
                        else c.startPastEnd = true;
                    } else {
                        c.startPastEnd = true;
                        startAt(c, 0);
                    }
                }
            } else if (period == 0 && _ptMode && c.sample && !c.startPastEnd) {
                // Without a note ProTracker still moves the start, once.
                uint32_t off = (uint32_t)c.sampleOffsetParam << 8;
                uint32_t len = _samples[c.sample - 1].length;
                if (off < len - (c.startOffset < len ? c.startOffset : len)) c.startOffset += off;
                else c.startPastEnd = true;
            }
            break;
        }
        case 0xB: // Position jump
            _positionJumpPending = true;
            _positionJumpTarget = param;
            break;
        case 0xC: // Set volume
            c.volume = (param > 64) ? 64 : param;
            break;
        case 0xD: // Pattern break (row is BCD-encoded)
            _patternBreakPending = true;
            _patternBreakRow = (param >> 4) * 10 + (param & 0x0F);
            break;
        case 0xE:
            if ((param >> 4) != 0xD) processExtendedTickZero(ch, param, rawPeriod != 0); // 0xD (note delay) already handled above
            break;
        case 0xF: // Set speed/tempo
            if (param == 0) _speed = 1; // technically "stop" in the strict spec; treated as speed=1, a safe simplification
            else if (param < 32) _speed = param;
            else if (_startrekker) _speed = 31; // Startrekker has no tempo command
            else if (_ptMode) _nextTempo = param; // takes effect on the next tick, as ProTracker's CIA timer
            else _tempo = param;
            break;
        default:
            break;
    }
}

void ModPlayer::processExtendedTickZero(int ch, uint8_t param, bool hasNote) {
    ModChannel& c = _channels[ch];
    uint8_t sub = param >> 4;
    uint8_t val = param & 0x0F;

    switch (sub) {
        case 0x1: // Fine portamento up (applied once, this tick only)
            if (c.period) {
                c.period = clampPeriod(c.period > val * 4 ? (uint16_t)(c.period - val * 4) : 1, c.finetune);
                recomputeStep(c);
            }
            break;
        case 0x2: // Fine portamento down
            if (c.period) {
                uint32_t p = (uint32_t)c.period + val * 4;
                c.period = clampPeriod(p > 0xFFFF ? 0xFFFF : (uint16_t)p, c.finetune);
                recomputeStep(c);
            }
            break;
        case 0x3: // Glissando control
            c.glissando = (val != 0);
            break;
        case 0x4: // Vibrato waveform
            c.vibratoWave = val & 7;
            break;
        case 0x7: // Tremolo waveform
            c.tremoloWave = val & 7;
            break;
        case 0x5: // Set finetune, done before the note in processCellTriggers()
            break;
        case 0x6: // Pattern loop
            patternLoop(c.patternLoopRow, c.patternLoopCount, val);
            break;
        case 0x8: // Set panning, coarse (0-15 -> 0-255)
            c.pan = (uint8_t)(val * 17);
            break;
        case 0x9: // Retrigger note every `val` ticks, and now if the row has no note
            if (val > 0) {
                c.retrigParam = val;
                if (!hasNote && c.period) triggerNote(ch, false);
            }
            break;
        case 0xA: // Fine volume slide up
            c.volume = (uint8_t)min(64, c.volume + val);
            break;
        case 0xB: // Fine volume slide down
            c.volume = (uint8_t)max(0, (int)c.volume - val);
            break;
        case 0xC: // Note cut, EC0 cuts now
            if (val == 0) c.volume = 0;
            else c.noteCutParam = val;
            break;
        case 0xE: // Pattern delay -- hold the current row for `val` extra
                  // full tick-cycles before advancing (see advanceTick()),
                  // not a permanent speed change.
            _patternDelayRepeatsLeft = val;
            break;
        case 0x0: // Set filter -- no hardware filter to toggle here, no-op
        case 0xF: // Invert Loop / Funk Repeat -- unsupported, see header comment
        default:
            break;
    }
}

// Vibrato and tremolo waveform value at pos (0..63), -255..255.
static inline __attribute__((always_inline)) int waveValue(uint8_t wave, uint8_t pos) {
    int v;
    switch (wave & 3) {
        case 0: v = SINE_TABLE[pos & 31]; break;
        case 1: v = (pos & 31) << 3; if (pos & 32) v = 255 - v; break;
        default: v = 255; break;
    }
    return (pos & 32) ? -v : v;
}

void __not_in_flash_func(ModPlayer::processTickEffects)() {
    for (int ch = 0; ch < _numChannels; ch++) {
        ModChannel& c = _channels[ch];

        if (c.noteDelayParam != 0 && _tick == c.noteDelayParam) {
            // EDx with no note does nothing, as ProTracker.
            if (c.noteDelayPeriod != 0) {
                c.period = c.noteDelayPeriod;
                triggerNote(ch, true);
            }
            c.noteDelayParam = 0;
        }
        if (c.noteCutParam != 0xFF && _tick == c.noteCutParam) {
            c.volume = 0;
            c.noteCutParam = 0xFF;
        }
        if (c.retrigParam > 0 && (_tick % c.retrigParam) == 0) {
            triggerNote(ch, false);
        }

        uint8_t fx = c.fx, param = c.fxParam;
        if (fx != 0x7) c.tremoloDelta = 0;
        if (c.period == 0) continue;

        // Volume slide (Axy, 5xy, 6xy): up by x a tick, else down by y.
        if (fx == 0xA || fx == 0x5 || fx == 0x6) {
            uint8_t slideUp = param >> 4;
            uint8_t slideDown = param & 0x0F;
            if (slideUp) c.volume = (uint8_t)min(64, c.volume + slideUp);
            else if (slideDown) c.volume = (uint8_t)max(0, (int)c.volume - slideDown);
        }

        // Portamento up/down (1xx/2xx), in quarter periods.
        if (fx == 0x1) {
            int p = (int)c.period - param * 4;
            c.period = clampPeriod(p < 1 ? 1 : (uint16_t)p, c.finetune);
        } else if (fx == 0x2) {
            int p = (int)c.period + param * 4;
            c.period = clampPeriod(p > 0xFFFF ? 0xFFFF : (uint16_t)p, c.finetune);
        }

        uint16_t playPeriod = c.period;
        // Tone portamento (3xx, 5xy) slides toward portaTarget; with
        // glissando on, the pitch heard moves a semitone at a time.
        if ((fx == 0x3 || fx == 0x5) && c.portaTarget != 0 && c.tonePortaSpeed) {
            int speed = c.tonePortaSpeed * 4;
            if (c.period < c.portaTarget) {
                c.period = (uint16_t)min((int)c.portaTarget, c.period + speed);
            } else if (c.period > c.portaTarget) {
                c.period = (uint16_t)max((int)c.portaTarget, c.period - speed);
            }
            playPeriod = c.glissando ? periodForNote(noteAtOrAbove(c.period, c.finetune), c.finetune) : c.period;
        }

        // Vibrato (4xy, 6xy) and arpeggio (0xy) only change the pitch that
        // plays this tick, c.period stays the base.
        if ((fx == 0x4 || fx == 0x6) && c.vibratoDepth) {
            // ProTracker's (wave * depth) >> 7 periods, kept in quarters as OpenMPT does.
            int p = (int)c.period + (waveValue(c.vibratoWave, c.vibratoPos) * c.vibratoDepth) / 32;
            playPeriod = clampPeriod(p < 1 ? 1 : (uint16_t)p, c.finetune);
            c.vibratoPos = (uint8_t)((c.vibratoPos + c.vibratoSpeed) & 0x3F);
        }
        if (fx == 0x0 && param != 0) {
            // Cycles base note, +x semitones, +y semitones, one per tick.
            int cycle = _tick % 3;
            int semis = (cycle == 1) ? (param >> 4) : (cycle == 2) ? (param & 0x0F) : 0;
            if (semis != 0) {
                int baseNote = noteAtOrAbove(c.period, c.finetune);
                playPeriod = clampPeriod(periodForNote(baseNote + semis, c.finetune), c.finetune);
            }
        }
        if (playPeriod != c.stepPeriod) recomputeStep(c, playPeriod);

        // Tremolo (7xy) offsets the mixed volume only, see tickEnd().
        if (fx == 0x7 && c.tremoloDepth) {
            c.tremoloDelta = (int8_t)((waveValue(c.tremoloWave, c.tremoloPos) * c.tremoloDepth) / 64);
            c.tremoloPos = (uint8_t)((c.tremoloPos + c.tremoloSpeed) & 0x3F);
        }
    }
}
