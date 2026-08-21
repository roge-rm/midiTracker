#include "wav_file.h"
#include "sd_card.h"
#include "synth.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>

namespace {

// WAV chunk header: 4-byte ID + 4-byte LITTLE-endian length (contrast
// MidiPlayer::readChunkHeader()'s SMF chunks, which are big-endian).
bool readChunkHeader(FsFile& f, char id[5], uint32_t& length) {
    uint8_t buf[8];
    if (f.read(buf, 8) != 8) return false;
    id[0] = buf[0]; id[1] = buf[1]; id[2] = buf[2]; id[3] = buf[3]; id[4] = '\0';
    length = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    return true;
}

const uint16_t WAVE_FORMAT_PCM = 1;
const uint16_t WAVE_FORMAT_IMA_ADPCM = 0x0011;
const uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

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

bool WavPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!_file.open(path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    char id[5];
    uint32_t riffLen;
    uint8_t waveId[4];
    if (!readChunkHeader(_file, id, riffLen) || strncmp(id, "RIFF", 4) != 0 ||
        _file.read(waveId, 4) != 4 || strncmp((const char*)waveId, "WAVE", 4) != 0) {
        strncpy(_error, "not a WAV file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    bool haveFmt = false;
    uint16_t formatTag = 0, channels = 0, bitsPerSample = 0, blockAlign = 0, samplesPerBlock = 0;
    uint32_t sampleRate = 0;
    _dataChunkStart = 0;
    _dataChunkLen = 0;

    // Walk chunks until both 'fmt ' and 'data' are found, skipping
    // anything else (LIST/fact/JUNK/id3 etc are common in real files).
    while (true) {
        char cid[5];
        uint32_t clen;
        if (!readChunkHeader(_file, cid, clen)) break; // EOF -- stop scanning
        uint32_t chunkDataStart = _file.curPosition();

        if (strncmp(cid, "fmt ", 4) == 0) {
            uint8_t fb[16];
            if (clen < 16 || _file.read(fb, 16) != 16) {
                strncpy(_error, "invalid WAV file", sizeof(_error) - 1);
                _file.close();
                _state = STATE_ERROR;
                return false;
            }
            formatTag = (uint16_t)fb[0] | ((uint16_t)fb[1] << 8);
            channels = (uint16_t)fb[2] | ((uint16_t)fb[3] << 8);
            sampleRate = (uint32_t)fb[4] | ((uint32_t)fb[5] << 8) | ((uint32_t)fb[6] << 16) | ((uint32_t)fb[7] << 24);
            blockAlign = (uint16_t)fb[12] | ((uint16_t)fb[13] << 8);
            bitsPerSample = (uint16_t)fb[14] | ((uint16_t)fb[15] << 8);

            // IMA ADPCM's fmt chunk carries two extra fields past the
            // basic 16 bytes: a 2-byte cbSize (extra-bytes count, == 2)
            // then a 2-byte wSamplesPerBlock -- the file's own authority
            // on its block layout, so this is read rather than derived.
            if (formatTag == WAVE_FORMAT_IMA_ADPCM && clen >= 20) {
                uint8_t extra[4];
                if (_file.read(extra, 4) == 4) {
                    samplesPerBlock = (uint16_t)extra[2] | ((uint16_t)extra[3] << 8);
                }
            }
            haveFmt = true;
        } else if (strncmp(cid, "data", 4) == 0) {
            _dataChunkStart = chunkDataStart;
            _dataChunkLen = clen;
            if (haveFmt) break; // have both -- no need to keep scanning trailing chunks
        }

        uint32_t next = chunkDataStart + clen + (clen & 1); // chunks are word-padded
        if (!_file.seekSet(next)) break;
    }

    if (!haveFmt || _dataChunkLen == 0) {
        strncpy(_error, "invalid WAV file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }
    if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
        strncpy(_error, "unsupported format", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }
    bool isAdpcm = (formatTag == WAVE_FORMAT_IMA_ADPCM);
    if (formatTag != WAVE_FORMAT_PCM && !isAdpcm) {
        strncpy(_error, "unsupported codec", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }
    if (channels == 0 || channels > 2) {
        strncpy(_error, channels > 2 ? "too many channels" : "invalid WAV file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }
    if (isAdpcm) {
        // A block needs at least one 4-byte header per channel; anything
        // smaller, or a missing/zero samplesPerBlock (the fmt chunk's
        // extra field wasn't readable), can't be decoded.
        uint16_t minBlockAlign = (channels == 2) ? 8 : 4;
        if (bitsPerSample != 4 || samplesPerBlock == 0 || blockAlign < minBlockAlign) {
            strncpy(_error, "invalid WAV file", sizeof(_error) - 1);
            _file.close();
            _state = STATE_ERROR;
            return false;
        }
    } else if (bitsPerSample != 8 && bitsPerSample != 16) {
        strncpy(_error, "unsupported bit depth", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }
    if (sampleRate < 4000 || sampleRate > 192000) {
        strncpy(_error, "invalid WAV file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    _isAdpcm = isAdpcm;
    _srcSampleRate = sampleRate;
    _srcChannels = (uint8_t)channels;
    _srcBitsPerSample = (uint8_t)bitsPerSample;
    _blockAlign = blockAlign;
    _samplesPerBlock = samplesPerBlock;
    _bytesPerSrcFrame = isAdpcm ? 0 : (uint32_t)_srcChannels * (_srcBitsPerSample / 8);

    _file.seekSet(_dataChunkStart);
    _srcFramesRead = 0;
    _sourceEnded = false;
    _adpcmBlockBytesLeft = 0; // force a fresh block-header read on the first readAdpcmFrame() call
    _adpcmHavePendingMonoSample = false;
    _adpcmGroupPosL = _adpcmGroupPosR = 8;

    _resampleStep = (uint32_t)(((uint64_t)_srcSampleRate << 16) / SAMPLE_RATE);
    _resampleFrac = 0;
    primeInterpolationPair();

    _elapsedMsFrozen = 0;
    _lastResumeMicros = micros();
    _state = STATE_PAUSED;
    return true;
}

void WavPlayer::close() {
    if (_file) _file.close();
    Synth::wavStreamReset();
    _state = STATE_IDLE;
}

uint32_t WavPlayer::totalSrcFrames() const {
    if (_isAdpcm) {
        // Only complete blocks are decodable -- a short trailing partial
        // block (dataChunkLen not an exact multiple of blockAlign) is
        // silently ignored, same as most IMA ADPCM decoders.
        if (_blockAlign == 0) return 0;
        return (_dataChunkLen / _blockAlign) * _samplesPerBlock;
    }
    return _bytesPerSrcFrame > 0 ? _dataChunkLen / _bytesPerSrcFrame : 0;
}

bool WavPlayer::readSrcFrame(int16_t& outL, int16_t& outR) {
    if (_isAdpcm) return readAdpcmFrame(outL, outR);

    if (_srcFramesRead >= totalSrcFrames()) return false;

    uint8_t raw[4]; // max 2 channels * 2 bytes
    if (_file.read(raw, _bytesPerSrcFrame) != (int)_bytesPerSrcFrame) return false;
    _srcFramesRead++;

    if (_srcBitsPerSample == 16) {
        int16_t l = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
        if (_srcChannels == 2) {
            outL = l;
            outR = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
        } else {
            outL = outR = l;
        }
    } else {
        // 8-bit PCM is unsigned (0..255, 128 == silence) per the WAV
        // spec, unlike 16-bit's signed convention -- shift up to int16
        // range after centering.
        int16_t l = (int16_t)(((int)raw[0] - 128) << 8);
        if (_srcChannels == 2) {
            outL = l;
            outR = (int16_t)(((int)raw[1] - 128) << 8);
        } else {
            outL = outR = l;
        }
    }
    return true;
}

bool WavPlayer::readAdpcmFrame(int16_t& outL, int16_t& outR) {
    if (_srcFramesRead >= totalSrcFrames()) return false;

    if (_adpcmBlockBytesLeft == 0) {
        // Start of a new block: each channel's own 4-byte header (a raw
        // int16 predictor + step index + reserved byte) directly gives
        // this block's first sample and initial decode state -- no
        // nibble decode needed for it.
        uint8_t hdr[4];
        if (_file.read(hdr, 4) != 4) return false;
        _adpcmPredictorL = (int16_t)((uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8));
        _adpcmStepIndexL = hdr[2];
        if (_adpcmStepIndexL > 88) _adpcmStepIndexL = 88;
        int16_t sampleL = (int16_t)_adpcmPredictorL;
        int16_t sampleR = sampleL;

        uint32_t headerBytes = 4;
        if (_srcChannels == 2) {
            uint8_t hdrR[4];
            if (_file.read(hdrR, 4) != 4) return false;
            _adpcmPredictorR = (int16_t)((uint16_t)hdrR[0] | ((uint16_t)hdrR[1] << 8));
            _adpcmStepIndexR = hdrR[2];
            if (_adpcmStepIndexR > 88) _adpcmStepIndexR = 88;
            sampleR = (int16_t)_adpcmPredictorR;
            headerBytes = 8;
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
        uint8_t b;
        if (_adpcmBlockBytesLeft == 0 || _file.read(&b, 1) != 1) return false;
        _adpcmBlockBytesLeft--;
        int16_t s0 = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, b & 0x0F);
        int16_t s1 = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, (b >> 4) & 0x0F);
        outL = outR = s0;
        _adpcmPendingMonoSample = s1;
        _adpcmHavePendingMonoSample = true;
        _srcFramesRead++;
        return true;
    }

    // Stereo: nibbles arrive as interleaved 4-byte/8-sample groups, L
    // then R, repeating -- both channels' group positions always advance
    // in lockstep (one sample each per call below), so they always run
    // out on the same call, which is what keeps refilling them in this
    // fixed L-then-R order correctly matched to the file's own byte order.
    if (_adpcmGroupPosL >= 8) {
        uint8_t g[4];
        if (_adpcmBlockBytesLeft < 4 || _file.read(g, 4) != 4) return false;
        _adpcmBlockBytesLeft -= 4;
        for (int i = 0; i < 4; i++) {
            _adpcmGroupL[i * 2]     = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, g[i] & 0x0F);
            _adpcmGroupL[i * 2 + 1] = decodeImaNibble(_adpcmPredictorL, _adpcmStepIndexL, (g[i] >> 4) & 0x0F);
        }
        _adpcmGroupPosL = 0;
    }
    if (_adpcmGroupPosR >= 8) {
        uint8_t g[4];
        if (_adpcmBlockBytesLeft < 4 || _file.read(g, 4) != 4) return false;
        _adpcmBlockBytesLeft -= 4;
        for (int i = 0; i < 4; i++) {
            _adpcmGroupR[i * 2]     = decodeImaNibble(_adpcmPredictorR, _adpcmStepIndexR, g[i] & 0x0F);
            _adpcmGroupR[i * 2 + 1] = decodeImaNibble(_adpcmPredictorR, _adpcmStepIndexR, (g[i] >> 4) & 0x0F);
        }
        _adpcmGroupPosR = 0;
    }

    outL = _adpcmGroupL[_adpcmGroupPosL++];
    outR = _adpcmGroupR[_adpcmGroupPosR++];
    _srcFramesRead++;
    return true;
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
    if (!_havePair) return; // nothing left to resample from

    const size_t MAX_FRAMES_PER_TICK = 1024; // bound per-update() work
    size_t produced = 0;

    while (produced < MAX_FRAMES_PER_TICK) {
        if (Synth::wavStreamFree() == 0) break;

        int32_t diffL = (int32_t)_nextSrcL - (int32_t)_lastSrcL;
        int32_t diffR = (int32_t)_nextSrcR - (int32_t)_lastSrcR;
        uint16_t frac16 = (uint16_t)(_resampleFrac & 0xFFFF);
        int16_t outFrame[2] = {
            (int16_t)(_lastSrcL + ((diffL * (int32_t)frac16) >> 16)),
            (int16_t)(_lastSrcR + ((diffR * (int32_t)frac16) >> 16)),
        };

        if (Synth::wavStreamWrite(outFrame, 1) == 0) break; // ring buffer filled since the check above
        produced++;

        _resampleFrac += _resampleStep;
        while (_resampleFrac >= 0x10000) {
            _resampleFrac -= 0x10000;
            _lastSrcL = _nextSrcL;
            _lastSrcR = _nextSrcR;
            if (!readSrcFrame(_nextSrcL, _nextSrcR)) {
                // Source exhausted -- keep repeating the final frame so
                // interpolation stays well-defined, and signal EOF once.
                _nextSrcL = _lastSrcL;
                _nextSrcR = _lastSrcR;
                if (!_sourceEnded) {
                    Synth::wavStreamEnd();
                    _sourceEnded = true;
                }
                _havePair = false;
                return;
            }
        }
    }
}

void WavPlayer::update() {
    if (_state == STATE_IDLE || _state == STATE_ERROR || _state == STATE_DONE) return;

    refillFromSource();

    // Wall-clock based: the ring buffer runs up to its capacity ahead of
    // what's actually audible, so "all source data read" doesn't mean
    // "done playing" yet -- wait for elapsedMs() to actually catch up to
    // totalMs() before switching to STATE_DONE, matching what's audible.
    if (_state == STATE_PLAYING && _sourceEnded && elapsedMs() >= totalMs()) {
        _elapsedMsFrozen = totalMs();
        _lastResumeMicros = micros();
        _state = STATE_DONE;
    }
}

void WavPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _lastResumeMicros = micros();
    _state = STATE_PLAYING;
    Synth::wavStreamSetActive(true);
}

void WavPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _elapsedMsFrozen += (micros() - _lastResumeMicros) / 1000;
    _state = STATE_PAUSED;
    Synth::wavStreamSetActive(false);
}

void WavPlayer::stop() {
    close();
}

uint32_t WavPlayer::elapsedMs() const {
    uint32_t extra = (_state == STATE_PLAYING) ? (micros() - _lastResumeMicros) / 1000 : 0;
    return _elapsedMsFrozen + extra;
}

uint32_t WavPlayer::totalMs() const {
    if (_srcSampleRate == 0) return 0;
    return (uint32_t)(((uint64_t)totalSrcFrames() * 1000ULL) / _srcSampleRate);
}

void WavPlayer::seekTo(uint32_t targetMs) {
    if (_state != STATE_PLAYING && _state != STATE_PAUSED && _state != STATE_DONE) return;

    uint32_t total = totalMs();
    if (targetMs > total) targetMs = total;

    uint32_t targetSrcFrame = (uint32_t)(((uint64_t)targetMs * _srcSampleRate) / 1000);

    if (_isAdpcm) {
        // No random access into a compressed block -- jump to the target
        // block's own boundary (real seekSet(), no reopen) and decode-
        // and-discard forward to the exact sample within it. See this
        // class's header comment for the cost characterization.
        uint32_t blockIndex = targetSrcFrame / _samplesPerBlock;
        uint32_t withinBlock = targetSrcFrame % _samplesPerBlock;
        _file.seekSet(_dataChunkStart + (uint64_t)blockIndex * _blockAlign);
        _srcFramesRead = blockIndex * _samplesPerBlock;
        _adpcmBlockBytesLeft = 0; // force a fresh block-header read on the next readAdpcmFrame() call
        _adpcmHavePendingMonoSample = false;
        _adpcmGroupPosL = _adpcmGroupPosR = 8;

        int16_t discardL, discardR;
        for (uint32_t i = 0; i < withinBlock; i++) {
            if (!readAdpcmFrame(discardL, discardR)) break;
        }
    } else {
        _file.seekSet(_dataChunkStart + targetSrcFrame * _bytesPerSrcFrame);
        _srcFramesRead = targetSrcFrame;
    }
    _sourceEnded = false;

    Synth::wavStreamReset(); // discard stale pre-seek audio; update() refills from here
    _resampleFrac = 0;
    primeInterpolationPair();

    bool wasPlaying = (_state == STATE_PLAYING);
    _elapsedMsFrozen = targetMs;
    _lastResumeMicros = micros();

    bool atEnd = (targetSrcFrame >= totalSrcFrames());
    if (atEnd) {
        _state = STATE_DONE;
    } else {
        _state = wasPlaying ? STATE_PLAYING : STATE_PAUSED;
        Synth::wavStreamSetActive(wasPlaying);
    }
}

bool WavPlayer::underrun() const {
    return Synth::wavStreamTookUnderrun();
}
