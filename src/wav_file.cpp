#include "wav_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include "shared_ram_arena.h"
#include <string.h>
#include <stdio.h>

namespace {

// WAV chunk header: 4-byte ID + 4-byte LITTLE-endian length (contrast
// MidiPlayer::readChunkHeader()'s SMF chunks, which are big-endian).
bool readChunkHeader(FsFile& f, char id[5], uint32_t& length, bool bigEndian = false) {
    uint8_t buf[8];
    if (f.read(buf, 8) != 8) return false;
    id[0] = buf[0]; id[1] = buf[1]; id[2] = buf[2]; id[3] = buf[3]; id[4] = '\0';
    if (bigEndian) {
        length = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
    } else {
        length = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    }
    return true;
}

// AIFF's 80-bit extended float sample rate (sign and 15-bit exponent, then a
// 64-bit mantissa with its top bit set) to whole Hz. 0 if it isn't sane.
uint32_t extendedToRate(const uint8_t* b) {
    int exp = (int)(((b[0] & 0x7F) << 8) | b[1]);
    if (b[0] & 0x80) return 0;
    uint32_t mant = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
    int shift = 16383 + 31 - exp;
    if (shift < 0 || shift > 31) return 0;
    return mant >> shift;
}

const uint16_t WAVE_FORMAT_PCM = 1;
const uint16_t WAVE_FORMAT_IEEE_FLOAT = 3;
const uint16_t WAVE_FORMAT_IMA_ADPCM = 0x0011;
const uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
// An EXTENSIBLE subformat GUID is the format code in its first two bytes
// followed by these 14.
const uint8_t KSDATAFORMAT_TAIL[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// The read buffer is SharedRamArena::scratch: up to READ_AT unread bytes
// carried over from the last read, then READ_BYTES of new data, so each
// read starts on a sector boundary (after the first) and stays whole sectors.
const uint32_t READ_AT = 16;
const uint32_t READ_BYTES = 1536;
static_assert(READ_AT + READ_BYTES <= SharedRamArena::SCRATCH_SIZE, "WAV read buffer no longer fits SharedRamArena::scratch");

// IEEE 754 single to int16 (v * 32768, clamped), in integer maths so core 0
// doesn't pay for soft float on every sample. Truncates toward zero.
int16_t floatToS16(uint32_t b) {
    int e = (int)((b >> 23) & 0xFF);
    bool neg = (b >> 31) != 0;
    if (e >= 127) return neg ? -32768 : 32767; // |v| >= 1, inf and NaN
    if (e < 112) return 0;                     // |v| < 2^-15
    int32_t v = (int32_t)(((b & 0x7FFFFF) | 0x800000) >> (135 - e));
    return (int16_t)(neg ? -v : v);
}

// Standard IMA ADPCM step-size and step-index tables -- same constants
// used by essentially every IMA ADPCM decoder (originally from the
// Interactive Multimedia Association's spec).
const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

// Decodes one 4-bit IMA ADPCM code against (and updates) a channel's
// running predictor/step index -- the one primitive readAdpcmFrame()
// builds its mono/stereo decode on top of.
int16_t decodeImaNibble(int32_t& predictor, int& stepIndex, uint8_t nibble) {
    int step = IMA_STEP_TABLE[stepIndex];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;

    int32_t newPredictor = predictor + diff;
    if (newPredictor > 32767) newPredictor = 32767;
    if (newPredictor < -32768) newPredictor = -32768;
    predictor = newPredictor;

    stepIndex += IMA_INDEX_TABLE[nibble & 0x0F];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;

    return (int16_t)newPredictor;
}

} // namespace

bool WavPlayer::fail(const char* msg) {
    strncpy(_error, msg, sizeof(_error) - 1);
    _error[sizeof(_error) - 1] = '\0';
    if (_file) _file.close();
    _state = STATE_ERROR;
    return false;
}

bool WavPlayer::load(const char* path) {
    close();
    _error[0] = '\0';
    _readFailed = false;
    hashPath(path);

    if (!sdOpenPath(_file, path, O_RDONLY)) return fail("could not open file");

    uint8_t head[12];
    if (_file.read(head, 12) != 12) return fail("not a WAV or AIFF file");
    bool aiff = memcmp(head, "FORM", 4) == 0 && (memcmp(head + 8, "AIFF", 4) == 0 || memcmp(head + 8, "AIFC", 4) == 0);
    bool aifc = aiff && head[11] == 'C';
    if (!aiff && (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0)) return fail("not a WAV or AIFF file");
    _bigEndian = false;
    _isAiff = aiff;
    _signed8 = false;

    bool haveFmt = false, extensibleOk = true;
    uint16_t formatTag = 0, channels = 0, bitsPerSample = 0, blockAlign = 0, samplesPerBlock = 0;
    uint32_t sampleRate = 0, aiffFrames = 0;
    _dataChunkStart = 0;
    _dataChunkLen = 0;

    // Walk chunks until both 'fmt ' and 'data' are found, skipping
    // anything else (LIST/fact/JUNK/id3 etc are common in real files).
    while (true) {
        char cid[5];
        uint32_t clen;
        if (!readChunkHeader(_file, cid, clen, aiff)) break; // EOF -- stop scanning
        uint32_t chunkDataStart = _file.curPosition();

        if (aiff) {
            if (strncmp(cid, "COMM", 4) == 0) {
                // channels, frames, bits, 80-bit rate, then AIFC's compression type
                uint8_t cb[22];
                if (clen < 18 || _file.read(cb, 18) != 18) return fail("invalid AIFF file");
                if (aifc && (clen < 22 || _file.read(cb + 18, 4) != 4)) return fail("invalid AIFF file");
                channels = (uint16_t)((cb[0] << 8) | cb[1]);
                aiffFrames = ((uint32_t)cb[2] << 24) | ((uint32_t)cb[3] << 16) | ((uint32_t)cb[4] << 8) | cb[5];
                bitsPerSample = (uint16_t)((cb[6] << 8) | cb[7]);
                sampleRate = extendedToRate(cb + 8);
                formatTag = WAVE_FORMAT_PCM;
                _bigEndian = true;
                _signed8 = true;
                if (aifc) {
                    if (!memcmp(cb + 18, "NONE", 4) || !memcmp(cb + 18, "twos", 4)) {
                    } else if (!memcmp(cb + 18, "sowt", 4)) {
                        _bigEndian = false;
                    } else if (!memcmp(cb + 18, "fl32", 4) || !memcmp(cb + 18, "FL32", 4)) {
                        formatTag = WAVE_FORMAT_IEEE_FLOAT;
                    } else {
                        return fail("unsupported codec");
                    }
                }
                // 12 or 20-bit samples sit left-justified in whole bytes
                if (formatTag == WAVE_FORMAT_PCM && bitsPerSample >= 1 && bitsPerSample <= 32) bitsPerSample = (bitsPerSample + 7) & ~7u;
                haveFmt = true;
            } else if (strncmp(cid, "SSND", 4) == 0) {
                // offset and block size, then the samples
                uint8_t sb[8];
                if (clen < 8 || _file.read(sb, 8) != 8) return fail("invalid AIFF file");
                uint32_t offset = ((uint32_t)sb[0] << 24) | ((uint32_t)sb[1] << 16) | ((uint32_t)sb[2] << 8) | sb[3];
                uint64_t start = (uint64_t)chunkDataStart + 8 + offset;
                uint64_t size = _file.fileSize();
                uint32_t avail = start < size ? (uint32_t)(size - start) : 0;
                uint32_t len = (uint64_t)offset + 8 <= clen ? clen - 8 - offset : 0;
                _dataChunkStart = (uint32_t)start;
                _dataChunkLen = len > avail ? avail : len;
                if (haveFmt) break;
            }
        } else if (strncmp(cid, "fmt ", 4) == 0) {
            // 16 basic bytes, then cbSize and its extra bytes: IMA ADPCM's
            // samplesPerBlock, or EXTENSIBLE's valid bits, channel mask and
            // subformat GUID (40 bytes in all).
            uint8_t fb[40];
            uint32_t want = clen < sizeof(fb) ? clen : sizeof(fb);
            if (clen < 16 || _file.read(fb, want) != (int)want) return fail("invalid WAV file");
            formatTag = (uint16_t)fb[0] | ((uint16_t)fb[1] << 8);
            channels = (uint16_t)fb[2] | ((uint16_t)fb[3] << 8);
            sampleRate = (uint32_t)fb[4] | ((uint32_t)fb[5] << 8) | ((uint32_t)fb[6] << 16) | ((uint32_t)fb[7] << 24);
            blockAlign = (uint16_t)fb[12] | ((uint16_t)fb[13] << 8);
            bitsPerSample = (uint16_t)fb[14] | ((uint16_t)fb[15] << 8);
            if (formatTag == WAVE_FORMAT_IMA_ADPCM && want >= 20) {
                samplesPerBlock = (uint16_t)fb[18] | ((uint16_t)fb[19] << 8);
            }
            if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
                // The real format is the subformat GUID's code.
                extensibleOk = want >= 40 && memcmp(fb + 26, KSDATAFORMAT_TAIL, sizeof(KSDATAFORMAT_TAIL)) == 0;
                formatTag = (uint16_t)fb[24] | ((uint16_t)fb[25] << 8);
            }
            haveFmt = true;
        } else if (strncmp(cid, "data", 4) == 0) {
            _dataChunkStart = chunkDataStart;
            uint32_t avail = _file.fileSize() > chunkDataStart ? (uint32_t)_file.fileSize() - chunkDataStart : 0;
            _dataChunkLen = clen > avail ? avail : clen; // a bogus length can't run past the file
            if (haveFmt) break; // have both -- no need to keep scanning trailing chunks
        }

        uint64_t next64 = (uint64_t)chunkDataStart + clen + (clen & 1); // chunks are word-padded
        if (next64 > _file.fileSize()) break; // past the end (also catches a length that would wrap)
        if (!_file.seekSet((uint32_t)next64)) break;
    }

    if (!haveFmt || _dataChunkLen == 0) return fail(aiff ? "invalid AIFF file" : "invalid WAV file");
    if (!extensibleOk) return fail("unsupported format");
    bool isAdpcm = (formatTag == WAVE_FORMAT_IMA_ADPCM);
    bool isFloat = (formatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (formatTag != WAVE_FORMAT_PCM && !isAdpcm && !isFloat) return fail("unsupported codec");
    if (channels == 0 || channels > 2) return fail(channels > 2 ? "too many channels" : aiff ? "invalid AIFF file" : "invalid WAV file");
    if (isAdpcm) {
        // A block is a 4-byte header per channel and then the nibbles, which
        // stereo groups in 4 bytes per channel. Its first sample is in the
        // header, then 2 per byte per channel.
        uint32_t headerBytes = 4u * channels;
        bool layoutOk = blockAlign > headerBytes && (channels == 1 || (blockAlign - headerBytes) % 8 == 0);
        uint32_t expected = layoutOk ? (blockAlign - headerBytes) * 2 / channels + 1 : 0;
        if (bitsPerSample != 4 || !layoutOk || samplesPerBlock != expected) return fail("invalid WAV file");
    } else if (isFloat ? bitsPerSample != 32
                       : (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)) {
        return fail("unsupported bit depth");
    }
    if (sampleRate < 4000 || sampleRate > 192000) return fail(aiff ? "invalid AIFF file" : "invalid WAV file");

    if (aiff) {
        // The frame count in COMM can be shorter than the data, as when more audio follows.
        uint64_t bytes = (uint64_t)aiffFrames * channels * (bitsPerSample / 8);
        if (bytes < _dataChunkLen) _dataChunkLen = (uint32_t)bytes;
        if (_dataChunkLen == 0) return fail("invalid AIFF file");
    }
    _isAdpcm = isAdpcm;
    _isFloat = isFloat;
    _srcSampleRate = sampleRate;
    _srcChannels = (uint8_t)channels;
    _srcBitsPerSample = (uint8_t)bitsPerSample;
    _blockAlign = blockAlign;
    _samplesPerBlock = samplesPerBlock;
    _bytesPerSrcFrame = isAdpcm ? 0 : (uint32_t)_srcChannels * (_srcBitsPerSample / 8);
    // Only complete ADPCM blocks are decoded; a short trailing block is
    // ignored, as most decoders do.
    _totalSrcFrames = isAdpcm ? (_dataChunkLen / _blockAlign) * _samplesPerBlock : _dataChunkLen / _bytesPerSrcFrame;

    seekSource(0);
    _srcFramesRead = 0;
    _underrunBase = Synth::wavStreamUnderrunSamples();
    _resampleStep = (uint32_t)(((uint64_t)_srcSampleRate << 16) / SAMPLE_RATE);
    primeInterpolationPair();
    restartClock(0);
    _state = STATE_PAUSED;
    return true;
}

void WavPlayer::close() {
    if (_file) _file.close();
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

void WavPlayer::seekSource(uint32_t byteOffset) {
    _filePos = _dataChunkStart + byteOffset;
    _dataEnd = _dataChunkStart + _dataChunkLen;
    if (_filePos > _dataEnd || !_file.seekSet(_filePos)) _filePos = _dataEnd;
    _bufPos = _bufLen = READ_AT;
    _sourceEnded = false;
    _adpcmBlockBytesLeft = 0; // the next readAdpcmFrame() reads a fresh block header
    _adpcmHavePendingMonoSample = false;
    _adpcmGroupPosL = _adpcmGroupPosR = 8;
}

void WavPlayer::restartClock(uint32_t ms) {
    _startMs = ms;
    _ringBase = Synth::wavStreamWritten();
}

bool WavPlayer::fill(uint32_t n) {
    uint32_t have = _bufLen - _bufPos;
    if (have >= n) return true;
    uint8_t* buf = SharedRamArena::scratch.bytes;
    memmove(buf + READ_AT - have, buf + _bufPos, have);
    _bufPos = READ_AT - have;
    _bufLen = READ_AT;
    uint32_t len = READ_BYTES;
    uint32_t end = (_filePos + len) & ~511u;
    if (end > _filePos) len = end - _filePos; // end on a sector boundary
    if (len > _dataEnd - _filePos) len = _dataEnd - _filePos;
    if (len == 0) return false;
#ifdef TRACKER_PROFILE
    uint32_t t0 = micros();
#endif
    int got = _file.read(buf + READ_AT, len);
#ifdef TRACKER_PROFILE
    profile.readUs += micros() - t0;
    profile.reads++;
    profile.readBytes += got > 0 ? (uint32_t)got : 0;
#endif
    if (got <= 0) {
        _readFailed = true;
        _dataEnd = _filePos; // a read error ends the source here
        return false;
    }
    _filePos += (uint32_t)got;
    _bufLen = READ_AT + (uint32_t)got;
    return _bufLen - _bufPos >= n;
}

int16_t WavPlayer::pcmSample(const uint8_t* p) const {
    // The top 16 bits, from the low byte of them and the high byte.
    switch (_srcBitsPerSample) {
        case 8: return _signed8 ? (int16_t)((int)(int8_t)p[0] << 8) : (int16_t)(((int)p[0] - 128) << 8); // WAV's is unsigned, 128 is silence
        case 16: return _bigEndian ? (int16_t)(((uint16_t)p[0] << 8) | p[1]) : (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        case 24: return _bigEndian ? (int16_t)(((uint16_t)p[0] << 8) | p[1]) : (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        default: {
            if (_isFloat) {
                uint32_t b = _bigEndian ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
                                        : (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                return floatToS16(b);
            }
            return _bigEndian ? (int16_t)(((uint16_t)p[0] << 8) | p[1]) : (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        }
    }
}

bool WavPlayer::readSrcFrame(int16_t& outL, int16_t& outR) {
    if (_srcFramesRead >= _totalSrcFrames) return false;
    if (_isAdpcm) return readAdpcmFrame(outL, outR);

    if (!fill(_bytesPerSrcFrame)) return false;
    const uint8_t* p = SharedRamArena::scratch.bytes + _bufPos;
    _bufPos += _bytesPerSrcFrame;
    _srcFramesRead++;
    outL = pcmSample(p);
    outR = _srcChannels == 2 ? pcmSample(p + _srcBitsPerSample / 8) : outL;
    return true;
}

bool WavPlayer::readAdpcmFrame(int16_t& outL, int16_t& outR) {
    const uint8_t* buf = SharedRamArena::scratch.bytes;

    // A new block starts only once this one's bytes and its decoded but
    // unplayed samples (the mono pending nibble, the stereo groups) are used.
    if (_adpcmBlockBytesLeft == 0 && !_adpcmHavePendingMonoSample && _adpcmGroupPosL >= 8) {
        // Each channel's 4-byte header (int16 predictor, step index,
        // reserved) is the block's first sample and its decode state.
        uint32_t headerBytes = 4u * _srcChannels;
        if (!fill(headerBytes)) return false;
        const uint8_t* hdr = buf + _bufPos;
        _bufPos += headerBytes;
        _adpcmPredictorL = (int16_t)((uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8));
        _adpcmStepIndexL = hdr[2];
        if (_adpcmStepIndexL > 88) _adpcmStepIndexL = 88;
        int16_t sampleL = (int16_t)_adpcmPredictorL;
        int16_t sampleR = sampleL;
        if (_srcChannels == 2) {
            _adpcmPredictorR = (int16_t)((uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8));
            _adpcmStepIndexR = hdr[6];
            if (_adpcmStepIndexR > 88) _adpcmStepIndexR = 88;
            sampleR = (int16_t)_adpcmPredictorR;
        }

        _adpcmBlockBytesLeft = _blockAlign - headerBytes;
        _adpcmHavePendingMonoSample = false;
        _adpcmGroupPosL = _adpcmGroupPosR = 8;

        outL = sampleL;
        outR = sampleR;
        _srcFramesRead++;
        return true;
    }

    if (_srcChannels == 1) {
        if (_adpcmHavePendingMonoSample) {
            outL = outR = _adpcmPendingMonoSample;
            _adpcmHavePendingMonoSample = false;
            _srcFramesRead++;
            return true;
        }
        if (_adpcmBlockBytesLeft == 0 || !fill(1)) return false;
        uint8_t b = buf[_bufPos++];
        _adpcmBlockBytesLeft--;
        int16_t s0 = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, b & 0x0F);
        int16_t s1 = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, (b >> 4) & 0x0F);
        outL = outR = s0;
        _adpcmPendingMonoSample = s1;
        _adpcmHavePendingMonoSample = true;
        _srcFramesRead++;
        return true;
    }

    // Stereo: nibbles come in 4-byte (8-sample) groups, L then R. Both
    // channels take one sample per call, so their groups run out together
    // and are refilled in the file's L, R order.
    if (_adpcmGroupPosL >= 8) {
        if (_adpcmBlockBytesLeft < 8 || !fill(8)) return false;
        const uint8_t* g = buf + _bufPos;
        _bufPos += 8;
        _adpcmBlockBytesLeft -= 8;
        for (int i = 0; i < 4; i++) {
            _adpcmGroupL[i * 2]     = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, g[i] & 0x0F);
            _adpcmGroupL[i * 2 + 1] = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, (g[i] >> 4) & 0x0F);
            _adpcmGroupR[i * 2]     = decodeImaNibble(_adpcmPredictorR, _adpcmStepIndexR, g[4 + i] & 0x0F);
            _adpcmGroupR[i * 2 + 1] = decodeImaNibble(_adpcmPredictorR, _adpcmStepIndexR, (g[4 + i] >> 4) & 0x0F);
        }
        _adpcmGroupPosL = _adpcmGroupPosR = 0;
    }

    outL = _adpcmGroupL[_adpcmGroupPosL++];
    outR = _adpcmGroupR[_adpcmGroupPosR++];
    _srcFramesRead++;
    return true;
}

void WavPlayer::rewindSource() {
    seekSource(0);
    _srcFramesRead = 0;
    _havePair = false; // update() has nothing to play until load() or seekTo()
}

size_t WavPlayer::readSource(int16_t* out, size_t maxFrames) {
    size_t n = 0;
    while (n < maxFrames && readSrcFrame(out[n * 2], out[n * 2 + 1])) n++;
    return n;
}

void WavPlayer::primeInterpolationPair() {
    _havePair = readSrcFrame(_lastSrcL, _lastSrcR);
    if (_havePair) {
        if (!readSrcFrame(_nextSrcL, _nextSrcR)) {
            // Only one source frame total -- repeat it so interpolation
            // still has a valid (flat) pair to work from.
            _nextSrcL = _lastSrcL;
            _nextSrcR = _lastSrcR;
        }
    } else {
        _lastSrcL = _lastSrcR = _nextSrcL = _nextSrcR = 0;
    }
    _resampleFrac = 0;
}

void WavPlayer::refillFromSource() {
    const size_t MAX_FRAMES_PER_TICK = 1024; // bound per-update() work
    const size_t BATCH = 32;                 // frames per ring write
    int16_t out[BATCH * 2];
    size_t budget = MAX_FRAMES_PER_TICK;

    while (_havePair && budget > 0) {
        size_t n = Synth::wavStreamFree();
        if (n > BATCH) n = BATCH;
        if (n > budget) n = budget;
        if (n == 0) break;

        size_t k = 0;
        bool ended = false;
        while (k < n) {
            int32_t diffL = (int32_t)_nextSrcL - (int32_t)_lastSrcL;
            int32_t diffR = (int32_t)_nextSrcR - (int32_t)_lastSrcR;
            // 15-bit fraction so |diff| (up to 65535) times it fits in int32.
            int32_t frac15 = (int32_t)((_resampleFrac & 0xFFFF) >> 1);
            out[k * 2] = (int16_t)(_lastSrcL + ((diffL * frac15) >> 15));
            out[k * 2 + 1] = (int16_t)(_lastSrcR + ((diffR * frac15) >> 15));
            k++;

            _resampleFrac += _resampleStep;
            while (_resampleFrac >= 0x10000) {
                _resampleFrac -= 0x10000;
                _lastSrcL = _nextSrcL;
                _lastSrcR = _nextSrcR;
                if (!readSrcFrame(_nextSrcL, _nextSrcR)) {
                    _nextSrcL = _lastSrcL;
                    _nextSrcR = _lastSrcR;
                    ended = true;
                    break;
                }
            }
            if (ended) break;
        }
        // Only the producer adds to the ring, so all k frames fit.
        Synth::wavStreamWrite(out, k);
        budget -= k;
        if (ended) {
            // After the last write, so core 1 never sees an empty ended
            // ring with frames still to come.
            Synth::wavStreamEnd();
            _sourceEnded = true;
            _havePair = false;
        }
    }
}

bool WavPlayer::ringDrained() const {
    // Core 1 stops mixing an ended stream once it finds the ring empty.
    if (!Synth::wavStreamIsActive() && Synth::wavStreamIsEnded()) return true;
    return (int32_t)(Synth::wavStreamPlayed() - Synth::wavStreamWritten()) >= 0;
}

void WavPlayer::update() {
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;

    refillFromSource();

    // The ring runs up to ~186 ms ahead of what's heard, so the song ends
    // once the source is read and the ring has played out.
    if (_state == STATE_PLAYING && _sourceEnded && ringDrained()) {
        _state = STATE_DONE;
    }
}

void WavPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _state = STATE_PLAYING;
    Synth::wavStreamSetActive(true);
}

void WavPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void WavPlayer::stop() {
    close();
}

uint32_t WavPlayer::elapsedMs() const {
    if (_state == STATE_DONE) return totalMs();
    if (_state != STATE_PLAYING && _state != STATE_PAUSED) return 0;
    uint32_t played = Synth::wavStreamPlayed() - _ringBase;
    uint32_t ms = _startMs + (uint32_t)((uint64_t)played * 1000u / SAMPLE_RATE);
    uint32_t total = totalMs();
    return ms < total ? ms : total;
}

uint32_t WavPlayer::totalMs() const {
    if (_srcSampleRate == 0) return 0;
    return (uint32_t)(((uint64_t)_totalSrcFrames * 1000ULL) / _srcSampleRate);
}

void WavPlayer::seekTo(uint32_t targetMs) {
    if (_state != STATE_PLAYING && _state != STATE_PAUSED && _state != STATE_DONE) return;

    uint32_t total = totalMs();
    if (targetMs > total) targetMs = total;

    uint32_t targetSrcFrame = (uint32_t)(((uint64_t)targetMs * _srcSampleRate) / 1000);

    if (_isAdpcm) {
        // No random access inside a compressed block: start at the block's
        // header and decode and drop up to the target sample.
        uint32_t blockIndex = targetSrcFrame / _samplesPerBlock;
        uint32_t withinBlock = targetSrcFrame % _samplesPerBlock;
        seekSource(blockIndex * (uint32_t)_blockAlign);
        _srcFramesRead = blockIndex * _samplesPerBlock;

        int16_t discardL, discardR;
        for (uint32_t i = 0; i < withinBlock; i++) {
            if (!readSrcFrame(discardL, discardR)) break;
        }
    } else {
        seekSource(targetSrcFrame * _bytesPerSrcFrame);
        _srcFramesRead = targetSrcFrame;
    }

    Synth::wavStreamReset(); // discard stale pre-seek audio; update() refills from here
    primeInterpolationPair();

    bool wasPlaying = (_state == STATE_PLAYING);
    restartClock(targetMs);

    bool atEnd = (targetSrcFrame >= _totalSrcFrames);
    if (atEnd) {
        _state = STATE_DONE;
    } else {
        _state = wasPlaying ? STATE_PLAYING : STATE_PAUSED;
        Synth::wavStreamSetActive(wasPlaying);
    }
}

void WavPlayer::formatText(char* out, size_t size) const {
    snprintf(out, size, "%dBIT", _srcBitsPerSample);
}

uint32_t WavPlayer::underrunSamples() const {
    return Synth::wavStreamUnderrunSamples() - _underrunBase;
}

#ifdef TRACKER_PROFILE
WavPlayer::Profile WavPlayer::profile;
#endif
