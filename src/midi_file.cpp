#include "midi_file.h"
#include "sd_card.h"
#include "midi_output.h"
#include "synth.h"
#include "smf.h"
#include <string.h>
#include <new>

namespace {
// Controllers a seek gathers and sends after bank, program and RPNs.
const uint8_t SEEK_CCS[7] = { 1, 7, 10, 11, 64, 91, 93 };
}

int MidiPlayer::readByte(TrackReader& t) {
    if (t.bufPos >= t.bufLen) {
        t.bufFilePos += t.bufLen;
        t.bufPos = 0;
        int n = t.file.read(t.buf, _bufSize);
        t.bufLen = n > 0 ? (uint8_t)n : 0;
        if (t.bufLen == 0) return -1;
    }
    return t.buf[t.bufPos++];
}

bool MidiPlayer::readBytes(TrackReader& t, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int b = readByte(t);
        if (b < 0) return false;
        dst[i] = (uint8_t)b;
    }
    return true;
}

void MidiPlayer::skipBytes(TrackReader& t, uint32_t n) {
    if (n <= (uint32_t)(t.bufLen - t.bufPos)) {
        t.bufPos += (uint8_t)n;
        return;
    }
    seekTrack(t, trackPos(t) + n);
}

void MidiPlayer::seekTrack(TrackReader& t, uint32_t pos) {
    t.file.seekSet(pos);
    t.bufFilePos = pos;
    t.bufLen = 0;
    t.bufPos = 0;
}

bool MidiPlayer::readVarLen(TrackReader& t, uint32_t& value) {
    return Smf::readVarLen([this, &t]() { return readByte(t); }, value);
}

bool MidiPlayer::primeTrack(int idx) {
    TrackReader& t = _tracks[idx];

    while (true) {
        if (!t.file || trackPos(t) >= t.chunkEnd) {
            t.ended = true; _trackFailed = true;
            return false;
        }

        uint32_t delta;
        if (!readVarLen(t, delta)) { t.ended = true; _trackFailed = true; return false; }
        t.absoluteTick += delta;

        int b0i = readByte(t);
        if (b0i < 0) { t.ended = true; _trackFailed = true; return false; }
        uint8_t b0 = (uint8_t)b0i;

        uint8_t status;
        uint8_t firstDataByte = 0;
        bool haveFirstDataByte = false;

        if (b0 & 0x80) {
            status = b0;
            if (status < 0xF0) t.runningStatus = status;
        } else {
            // running status: b0 is actually the first data byte
            status = t.runningStatus;
            firstDataByte = b0;
            haveFirstDataByte = true;
            if (status == 0) { t.ended = true; _trackFailed = true; return false; } // malformed stream
        }

        if (status == 0xFF) {
            // Meta event: FF <type> <varlen> <data...>
            int metaTypeI = readByte(t);
            if (metaTypeI < 0) { t.ended = true; _trackFailed = true; return false; }
            uint8_t metaType = (uint8_t)metaTypeI;
            uint32_t len;
            if (!readVarLen(t, len)) { t.ended = true; _trackFailed = true; return false; }

            if (metaType == 0x51 && len == 3) { // Set Tempo
                uint8_t d[3];
                if (!readBytes(t, d, 3)) { t.ended = true; _trackFailed = true; return false; }
                t.pendingTick = t.absoluteTick;
                t.pendingKind = EV_TEMPO;
                t.pendingTempoUsPerQuarter =
                    ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
                return true;
            } else if (metaType == 0x2F) { // End of Track, fired at its own time
                t.pendingTick = t.absoluteTick;
                t.pendingKind = EV_END;
                return true;
            } else {
                if (len > 0) skipBytes(t, len);
                continue; // not playable, keep scanning
            }
        } else if (status == 0xF0 || status == 0xF7) {
            // SysEx: F0/F7 <varlen length> <payload, including trailing
            // F7>. Stage the payload's file position/length (see
            // TrackReader's comment) rather than reading it now -- it can
            // be arbitrarily long, unlike a channel-voice event's fixed
            // 1-2 data bytes.
            uint32_t len;
            if (!readVarLen(t, len)) { t.ended = true; _trackFailed = true; return false; }
            t.pendingTick = t.absoluteTick;
            t.pendingKind = EV_SYSEX;
            t.pendingSysExHasF0 = (status == 0xF0);
            t.pendingSysExPos = trackPos(t);
            t.pendingSysExLen = len;
            if (len > 0) skipBytes(t, len);
            return true;
        } else if (status >= 0x80 && status < 0xF0) {
            // Channel voice/mode message.
            uint8_t need = Smf::channelMessageDataBytes(status);
            uint8_t d1 = 0, d2 = 0;

            if (haveFirstDataByte) {
                d1 = firstDataByte;
                if (need >= 2) {
                    int r = readByte(t);
                    if (r < 0) { t.ended = true; _trackFailed = true; return false; }
                    d2 = (uint8_t)r;
                }
            } else {
                if (need >= 1) {
                    int r = readByte(t);
                    if (r < 0) { t.ended = true; _trackFailed = true; return false; }
                    d1 = (uint8_t)r;
                }
                if (need >= 2) {
                    int r = readByte(t);
                    if (r < 0) { t.ended = true; _trackFailed = true; return false; }
                    d2 = (uint8_t)r;
                }
            }

            t.pendingTick = t.absoluteTick;
            t.pendingKind = EV_CHANNEL;
            t.pendingStatus = status;
            t.pendingData1 = d1;
            t.pendingData2 = d2;
            t.pendingLen = 1 + need;
            return true;
        } else {
            // Other system messages (0xF1-0xF6, 0xF8-0xFE) shouldn't
            // appear in a standard MIDI file; bail on this track rather
            // than risk mis-parsing the rest of the stream.
            t.ended = true; _trackFailed = true;
            return false;
        }
    }
}

void MidiPlayer::sendPendingSysEx(TrackReader& t) {
    uint32_t len = t.pendingSysExLen;
    if (len == 0 || len > MIDI_SYSEX_MAX_LEN) return; // 0 = malformed; oversized -- see config.h's MIDI_SYSEX_MAX_LEN comment

    // Reads straight from the file, then puts the file cursor back at the
    // end of the read-ahead buffer, which stays valid.
    uint32_t savedPos = t.file.curPosition();
    t.file.seekSet(t.pendingSysExPos);

    uint8_t* buf = MidiOutput::sysExScratch(); // MIDI_SYSEX_MAX_LEN + 1, room for the F0 added below
    const uint32_t off = t.pendingSysExHasF0 ? 1 : 0;
    buf[0] = 0xF0;
    bool ok = t.file.read(buf + off, len) == (int)len;

    t.file.seekSet(savedPos);
    if (ok) MidiOutput::sendSysExRaw(buf, len + off);
}

int MidiPlayer::findNextTrack() const {
    int best = -1;
    uint32_t bestTick = 0;
    for (int i = 0; i < _numTracks; i++) {
        if (_tracks[i].ended) continue;
        if (best < 0 || _tracks[i].pendingTick < bestTick) {
            best = i;
            bestTick = _tracks[i].pendingTick;
        }
    }
    return best;
}

MidiPlayer::~MidiPlayer() {
    close();
    delete[] _tracks;
    delete[] _bufPool;
}

bool MidiPlayer::load(const char* path) {
    close();
    _error[0] = '\0';
    // seekTo() reopens via load(_path) itself -- guard the self-copy case
    // rather than relying on strncpy's unspecified behavior when src and
    // dest are the same buffer.
    if (path != _path) {
        strncpy(_path, path, sizeof(_path) - 1);
        _path[sizeof(_path) - 1] = '\0';
    }

    FsFile hdr;
    if (!sdOpenPath(hdr, path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    char id[5];
    uint32_t len;
    if (!Smf::readChunkHeader(hdr, id, len) || strncmp(id, "MThd", 4) != 0 || len < 6) {
        strncpy(_error, "not a MIDI file", sizeof(_error) - 1);
        hdr.close();
        _state = STATE_ERROR;
        return false;
    }

    uint8_t hb[6];
    if (hdr.read(hb, 6) != 6) {
        strncpy(_error, "truncated header", sizeof(_error) - 1);
        hdr.close();
        _state = STATE_ERROR;
        return false;
    }
    _format = ((uint16_t)hb[0] << 8) | hb[1];
    uint16_t ntrks = ((uint16_t)hb[2] << 8) | hb[3];
    _division = ((uint16_t)hb[4] << 8) | hb[5];

    if (_division == 0) {
        strncpy(_error, "invalid time division", sizeof(_error) - 1);
        hdr.close();
        _state = STATE_ERROR;
        return false;
    }
    _smpte = (_division & 0x8000) != 0;
    if (_smpte) {
        // High byte is -fps (24, 25, 29 = 29.97 drop frame, 30), low byte ticks per frame.
        uint32_t fps = (uint32_t)(-(int8_t)(_division >> 8));
        uint32_t tpf = _division & 0xFF;
        if (fps == 0 || fps > 127 || tpf == 0) {
            strncpy(_error, "invalid time division", sizeof(_error) - 1);
            hdr.close();
            _state = STATE_ERROR;
            return false;
        }
        _tickNum = fps == 29 ? 1001000 : 1000000; // 29.97 = 30000/1001
        _tickDen = (fps == 29 ? 30 : fps) * tpf;
    } else {
        _tickNum = 500000; // 120 BPM until a tempo meta says otherwise
        _tickDen = _division;
    }
    if (_format == 2) {
        strncpy(_error, "format 2 files not supported", sizeof(_error) - 1);
        hdr.close();
        _state = STATE_ERROR;
        return false;
    }
    if (ntrks > MIDI_MAX_TRACKS) {
        strncpy(_error, "too many tracks", sizeof(_error) - 1);
        hdr.close();
        _state = STATE_ERROR;
        return false;
    }
    if (ntrks > _trackCapacity) {
        delete[] _tracks;
        _tracks = new (std::nothrow) TrackReader[ntrks];
        _trackCapacity = _tracks ? ntrks : 0;
        if (!_tracks) {
            strncpy(_error, "not enough memory", sizeof(_error) - 1);
            hdr.close();
            _state = STATE_ERROR;
            return false;
        }
    }
    uint8_t bufSize = (uint8_t)(ntrks ? (BUF_POOL_MAX / ntrks > BUF_MAX ? BUF_MAX
                                  : BUF_POOL_MAX / ntrks < BUF_MIN ? BUF_MIN : BUF_POOL_MAX / ntrks)
                                  : BUF_MAX);
    uint32_t poolNeed = (uint32_t)ntrks * bufSize;
    if (poolNeed > _bufPoolSize) {
        delete[] _bufPool;
        _bufPool = new (std::nothrow) uint8_t[poolNeed];
        _bufPoolSize = _bufPool ? (uint16_t)poolNeed : 0;
        if (!_bufPool) {
            strncpy(_error, "not enough memory", sizeof(_error) - 1);
            hdr.close();
            _state = STATE_ERROR;
            return false;
        }
    }
    _bufSize = bufSize;
    if (len > 6) hdr.seekCur(len - 6); // tolerate padded header chunks

    _numTracks = 0;
    for (uint16_t i = 0; i < ntrks; i++) {
        char tid[5];
        uint32_t tlen;
        if (!Smf::readChunkHeader(hdr, tid, tlen)) break;
        uint32_t trackStart = hdr.curPosition();

        if (strncmp(tid, "MTrk", 4) != 0) {
            hdr.seekCur(tlen); // unknown chunk type, skip it
            continue;
        }

        TrackReader& t = _tracks[_numTracks];
        t = TrackReader(); // reset in case this slot was used previously
        t.buf = _bufPool + (uint32_t)_numTracks * _bufSize;
        if (!sdOpenPath(t.file, path, O_RDONLY)) {
            hdr.seekCur(tlen);
            continue;
        }
        seekTrack(t, trackStart);
        t.chunkEnd = trackStart + tlen;

        hdr.seekSet(trackStart + tlen); // advance master cursor past this track

        // A track holding only End of Track at tick 0 is left out.
        if (primeTrack(_numTracks) && !(t.pendingKind == EV_END && t.pendingTick == 0)) {
            _numTracks++;
        } else {
            t.file.close();
            // Track had no playable/tempo events (e.g. an empty track) --
            // simply don't count it; the slot is reused next iteration.
        }
    }
    hdr.close();

    if (_numTracks == 0) {
        strncpy(_error, "no playable tracks found", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    _usPerQuarter = 500000; // 120 BPM until a tempo meta says otherwise
    // _tempoScale is deliberately NOT reset here -- see the header comment
    // on adjustTempoScale(). Callers doing a fresh (not restart) open call
    // setTempoScale(1.0f) themselves.
    updateEffTick();
    _globalTick = 0;
    _songUs = 0;
    _tickRem = 0;
    _originMicros = micros();
    _pausedOffsetUs = 0;
    memset(_rpnSel, 0xFF, sizeof(_rpnSel));
    memset(_held, 0, sizeof(_held));
    _trackFailed = false; // tracks dropped above sent nothing
    clearSeekState();
    if (_deferSeek) {
        _seekNeedsReset = true;
        _seekPending = true;
    } else {
        _seekNeedsReset = false;
        resetChannels();
    }
    _state = STATE_PAUSED;
    return true;
}

void MidiPlayer::close() {
    for (int i = 0; i < _numTracks; i++) {
        if (_tracks[i].file) _tracks[i].file.close();
    }
    _numTracks = 0;
    _state = STATE_IDLE;
}

void MidiPlayer::play() {
    if (_state != STATE_PAUSED) return;
    _originMicros = micros() - _pausedOffsetUs;
    _state = STATE_PLAYING;
}

void MidiPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _pausedOffsetUs = micros() - _originMicros;
    _state = STATE_PAUSED;
    MidiOutput::allNotesOffAllChannels();
    memset(_held, 0, sizeof(_held));
}

void MidiPlayer::stop() {
    MidiOutput::allNotesOffAllChannels();
    Synth::resetControllers();
    close();
}

uint32_t MidiPlayer::elapsedMs() const {
    uint64_t us = _songUs;
    if (_state == STATE_PLAYING) us += (uint32_t)(micros() - _originMicros);
    else if (_state == STATE_PAUSED) us += _pausedOffsetUs;
    return (uint32_t)(us / 1000);
}

uint64_t MidiPlayer::ticksToUs(uint32_t tick, uint32_t& remOut) const {
    uint64_t num = (uint64_t)(tick - _globalTick) * _effTickNum + _tickRem;
    uint64_t us = num / _tickDen;
    remOut = (uint32_t)(num - us * _tickDen);
    return us;
}

void MidiPlayer::updateEffTick() {
    _effTickNum = (uint32_t)(_tickNum / _tempoScale);
    if (_effTickNum == 0) _effTickNum = 1;
}

void MidiPlayer::applyTempo(uint32_t usPerQuarter) {
    if (_smpte || usPerQuarter == 0) return; // SMPTE ticks are fixed
    _usPerQuarter = usPerQuarter;
    _tickNum = usPerQuarter;
    updateEffTick();
}

void MidiPlayer::trackRpnSelect(uint8_t ch, uint8_t cc, uint8_t value) {
    if (cc == 101) _rpnSel[ch][0] = value & 0x7F;
    else if (cc == 100) _rpnSel[ch][1] = value & 0x7F;
    else if (cc == 99 || cc == 98) _rpnSel[ch][0] = _rpnSel[ch][1] = 0x7F;
}

void MidiPlayer::resetChannels() {
    // CC121 leaves volume and pan alone (RP-015), so the synth gets a full
    // controller reset too; seek then sends the landing point's values.
    Synth::resetControllers();
    for (uint8_t ch = 0; ch < 16; ch++) {
        MidiOutput::sendRaw(0xB0 | ch, 121, 0, 3);   // Reset All Controllers
        MidiOutput::sendRaw(0xE0 | ch, 0, 0x40, 3);  // bend centre
    }
}

void MidiPlayer::gatherSeekEvent(const TrackReader& t) {
    uint8_t ch = t.pendingStatus & 0x0F;
    uint8_t d1 = t.pendingData1 & 0x7F, d2 = t.pendingData2 & 0x7F;
    SeekChannel& c = _seek[ch];
    switch (t.pendingStatus & 0xF0) {
        case 0xC0: c.program = d1; c.progBank0 = c.bank0; c.progBank32 = c.bank32; break;
        case 0xE0: c.bendLsb = d1; c.bendMsb = d2; break;
        case 0xB0: {
            if (d1 == 0) { c.bank0 = d2; break; }
            if (d1 == 32) { c.bank32 = d2; break; }
            if (d1 >= 98 && d1 <= 101) { trackRpnSelect(ch, d1, d2); c.rpnTouched = true; break; }
            if (d1 == 6 || d1 == 38) {
                uint8_t m = _rpnSel[ch][0], l = _rpnSel[ch][1];
                if (m != 0) break; // none, null or not RPN 0-2
                bool msb = d1 == 6;
                if (l == 0) { if (msb) c.rpn0Msb = d2; else c.rpn0Lsb = d2; }
                else if (l == 1) { if (msb) c.rpn1Msb = d2; else c.rpn1Lsb = d2; }
                else if (l == 2) { if (msb) c.rpn2Msb = d2; }
                else break;
                c.rpnTouched = true;
                break;
            }
            for (int i = 0; i < 7; i++) {
                if (SEEK_CCS[i] == d1) { c.cc[i] = d2; break; }
            }
            break;
        }
        default: break; // notes and pressure are dropped
    }
}

void MidiPlayer::clearSeekState() {
    memset(_seek, 0xFF, sizeof(_seek));
    for (auto& c : _seek) c.rpnTouched = false;
}

void MidiPlayer::sendSeekState() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        const SeekChannel& c = _seek[ch];
        const uint8_t cc = 0xB0 | ch;
        // The program goes out with the bank it was chosen under; a bank
        // set after it follows, for the file's next program change.
        // 0xFF progBank means the bank was set before the seek began.
        uint8_t sent0 = 0xFF, sent32 = 0xFF;
        if (c.program != 0xFF) {
            sent0 = c.progBank0;
            sent32 = c.progBank32;
            if (sent0 != 0xFF) MidiOutput::sendRaw(cc, 0, sent0, 3);
            if (sent32 != 0xFF) MidiOutput::sendRaw(cc, 32, sent32, 3);
            MidiOutput::sendRaw(0xC0 | ch, c.program, 0, 2);
        }
        if (c.bank0 != 0xFF && c.bank0 != sent0) MidiOutput::sendRaw(cc, 0, c.bank0, 3);
        if (c.bank32 != 0xFF && c.bank32 != sent32) MidiOutput::sendRaw(cc, 32, c.bank32, 3);
        if (c.rpnTouched) {
            const uint8_t msb[3] = { c.rpn0Msb, c.rpn1Msb, c.rpn2Msb };
            const uint8_t lsb[3] = { c.rpn0Lsb, c.rpn1Lsb, 0xFF };
            for (uint8_t r = 0; r < 3; r++) {
                if (msb[r] == 0xFF && lsb[r] == 0xFF) continue;
                MidiOutput::sendRaw(cc, 101, 0, 3);
                MidiOutput::sendRaw(cc, 100, r, 3);
                if (msb[r] != 0xFF) MidiOutput::sendRaw(cc, 6, msb[r], 3);
                if (lsb[r] != 0xFF) MidiOutput::sendRaw(cc, 38, lsb[r], 3);
            }
            // Leave the file's own select in place, or null if it had none.
            uint8_t m = _rpnSel[ch][0], l = _rpnSel[ch][1];
            MidiOutput::sendRaw(cc, 101, m == 0xFF ? 0x7F : m, 3);
            MidiOutput::sendRaw(cc, 100, l == 0xFF ? 0x7F : l, 3);
        }
        for (int i = 0; i < 7; i++) {
            if (c.cc[i] != 0xFF) MidiOutput::sendRaw(cc, SEEK_CCS[i], c.cc[i], 3);
        }
        if (c.bendMsb != 0xFF) MidiOutput::sendRaw(0xE0 | ch, c.bendLsb, c.bendMsb, 3);
    }
}

void MidiPlayer::trackHeld(uint8_t status, uint8_t note, uint8_t velocity) {
    uint8_t type = status & 0xF0, ch = status & 0x0F;
    if (type != 0x80 && type != 0x90) return;
    note &= 0x7F;
    uint32_t bit = 1u << (note & 31);
    if (type == 0x90 && velocity > 0) _held[ch][note >> 5] |= bit;
    else _held[ch][note >> 5] &= ~bit;
}

void MidiPlayer::releaseHeld() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        for (uint8_t w = 0; w < 4; w++) {
            uint32_t bits = _held[ch][w];
            while (bits) {
                uint8_t b = (uint8_t)__builtin_ctz(bits);
                bits &= bits - 1;
                MidiOutput::sendRaw(0x80 | ch, (uint8_t)(w * 32 + b), 0, 3);
            }
            _held[ch][w] = 0;
        }
    }
}

void MidiPlayer::advanceSilentlyTo(uint32_t targetMs, bool& reachedEnd) {
    uint64_t targetUs = (uint64_t)targetMs * 1000;
    reachedEnd = false;

    for (;;) {
        int nt = findNextTrack();
        if (nt < 0) { reachedEnd = true; return; }

        TrackReader& t = _tracks[nt];
        uint32_t rem;
        uint64_t deltaUs = ticksToUs(t.pendingTick, rem);
        if (_songUs + deltaUs >= targetUs) return; // events at the target fire in play

        _songUs += deltaUs;
        _tickRem = rem;
        _globalTick = t.pendingTick;

        if (t.pendingKind == EV_TEMPO) applyTempo(t.pendingTempoUsPerQuarter);
        else if (t.pendingKind == EV_CHANNEL) gatherSeekEvent(t);
        // SysEx is skipped during a seek, not re-sent.

        if (t.pendingKind == EV_END) t.ended = true;
        else primeTrack(nt);
    }
}

void MidiPlayer::seekTo(uint32_t targetMs, bool defer) {
    if (_state != STATE_PLAYING && _state != STATE_PAUSED && _state != STATE_DONE) return;

    bool wasPlaying = (_state == STATE_PLAYING);
    releaseHeld(); // the full panic comes with finishSeek()

    // No random-access index exists (see header comment) -- a backward
    // seek, or one starting from STATE_DONE (every track already ended),
    // has no choice but to reopen and re-scan from the top. A forward
    // seek instead just continues from the current track cursors.
    bool needsRewind = (_state == STATE_DONE) || (targetMs < elapsedMs());
    if (needsRewind) {
        _deferSeek = defer;
        bool ok = load(_path); // clears _seek, and sends or defers the reset
        _deferSeek = false;
        if (!ok) return; // failure leaves _state == STATE_ERROR, same as restartFromTop()
    }

    bool reachedEnd = false;
    advanceSilentlyTo(targetMs, reachedEnd);
    _seekPending = true;
    if (!defer) finishSeek();

    // Time past the last event passed, up to the target.
    uint64_t targetUs = (uint64_t)targetMs * 1000;
    uint32_t offset = reachedEnd || targetUs < _songUs ? 0 : (uint32_t)(targetUs - _songUs);
    if (reachedEnd) {
        _pausedOffsetUs = 0;
        _state = STATE_DONE;
    } else if (wasPlaying) {
        _originMicros = micros() - offset;
        _state = STATE_PLAYING;
    } else {
        _pausedOffsetUs = offset;
        _state = STATE_PAUSED;
    }
}

void MidiPlayer::finishSeek() {
    if (!_seekPending) return;
    MidiOutput::allNotesOffAllChannels();
    memset(_held, 0, sizeof(_held));
    if (_seekNeedsReset) resetChannels();
    sendSeekState();
    clearSeekState();
    _seekPending = false;
    _seekNeedsReset = false;
}

void MidiPlayer::adjustTempoScale(float delta) {
    setTempoScale(_tempoScale + delta);
}

void MidiPlayer::setTempoScale(float scale) {
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.0f) scale = 2.0f;
    _tempoScale = scale;
    updateEffTick();
}

uint16_t MidiPlayer::currentBPM() const {
    uint32_t effectiveUs = _smpte ? (uint32_t)(500000 / _tempoScale) : (uint32_t)(_usPerQuarter / _tempoScale);
    if (effectiveUs == 0) return 0;
    return (uint16_t)(60000000UL / effectiveUs);
}

void MidiPlayer::update() {
    if (_state != STATE_PLAYING) return;

    // Bound how many events we drain in one call so a dense chord/burst
    // can't block the UI/button loop for too long.
    const int MAX_EVENTS_PER_CALL = 64;

    for (int n = 0; n < MAX_EVENTS_PER_CALL; n++) {
        int nt = findNextTrack();
        if (nt < 0) {
            // Natural end: the file's note-offs have gone out, so the last
            // notes keep their release. Notes still on get note-offs, and
            // a track that broke off mid-stream gets a panic. Elapsed time
            // holds at the end.
            releaseHeld();
            if (_trackFailed) MidiOutput::allNotesOffAllChannels();
            _pausedOffsetUs = 0;
            _state = STATE_DONE;
            return;
        }

        TrackReader& t = _tracks[nt];
        uint32_t rem;
        uint64_t deltaUs = ticksToUs(t.pendingTick, rem);
        uint32_t due = _originMicros + (uint32_t)deltaUs;

        uint32_t now = micros();
        if ((int32_t)(now - due) < 0) return; // not due yet, try again next update()

        if (t.pendingKind == EV_TEMPO) {
            applyTempo(t.pendingTempoUsPerQuarter);
        } else if (t.pendingKind == EV_SYSEX) {
            sendPendingSysEx(t);
        } else if (t.pendingKind == EV_CHANNEL) {
            if ((t.pendingStatus & 0xF0) == 0xB0) trackRpnSelect(t.pendingStatus & 0x0F, t.pendingData1, t.pendingData2);
            trackHeld(t.pendingStatus, t.pendingData1, t.pendingData2);
            MidiOutput::sendRaw(t.pendingStatus, t.pendingData1, t.pendingData2, t.pendingLen);
        }

        _globalTick = t.pendingTick;
        _tickRem = rem;
        _songUs += deltaUs;
        _originMicros = due;

        if (t.pendingKind == EV_END) t.ended = true;
        else primeTrack(nt);
    }
}
