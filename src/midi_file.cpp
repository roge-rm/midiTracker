#include "midi_file.h"
#include "sd_card.h"
#include "midi_output.h"
#include <string.h>

bool MidiPlayer::readChunkHeader(FsFile& f, char id[5], uint32_t& length) {
    uint8_t buf[8];
    if (f.read(buf, 8) != 8) return false;
    id[0] = buf[0]; id[1] = buf[1]; id[2] = buf[2]; id[3] = buf[3]; id[4] = '\0';
    length = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
             ((uint32_t)buf[6] << 8) | buf[7];
    return true;
}

bool MidiPlayer::readVarLen(FsFile& f, uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; i++) {
        int b = f.read();
        if (b < 0) return false;
        value = (value << 7) | (uint32_t)(b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true; // malformed (>4 continuation bytes) -- accept what we have
}

uint8_t MidiPlayer::channelMessageDataBytes(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0: // Program Change
        case 0xD0: // Channel Pressure
            return 1;
        default: // Note Off/On, Poly Pressure, Control Change, Pitch Bend
            return 2;
    }
}

bool MidiPlayer::primeTrack(int idx) {
    TrackReader& t = _tracks[idx];

    while (true) {
        if (!t.file || t.file.curPosition() >= t.chunkEnd) {
            t.ended = true;
            return false;
        }

        uint32_t delta;
        if (!readVarLen(t.file, delta)) { t.ended = true; return false; }
        t.absoluteTick += delta;

        int b0i = t.file.read();
        if (b0i < 0) { t.ended = true; return false; }
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
            if (status == 0) { t.ended = true; return false; } // malformed stream
        }

        if (status == 0xFF) {
            // Meta event: FF <type> <varlen> <data...>
            int metaTypeI = t.file.read();
            if (metaTypeI < 0) { t.ended = true; return false; }
            uint8_t metaType = (uint8_t)metaTypeI;
            uint32_t len;
            if (!readVarLen(t.file, len)) { t.ended = true; return false; }

            if (metaType == 0x51 && len == 3) { // Set Tempo
                uint8_t d[3];
                if (t.file.read(d, 3) != 3) { t.ended = true; return false; }
                t.pendingTick = t.absoluteTick;
                t.pendingIsTempo = true;
                t.pendingIsSysEx = false;
                t.pendingTempoUsPerQuarter =
                    ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
                return true;
            } else if (metaType == 0x2F) { // End of Track
                t.ended = true;
                return false;
            } else {
                if (len > 0) t.file.seekCur(len);
                continue; // not playable, keep scanning
            }
        } else if (status == 0xF0 || status == 0xF7) {
            // SysEx: F0/F7 <varlen length> <payload, including trailing
            // F7>. Stage the payload's file position/length (see
            // TrackReader's comment) rather than reading it now -- it can
            // be arbitrarily long, unlike a channel-voice event's fixed
            // 1-2 data bytes.
            uint32_t len;
            if (!readVarLen(t.file, len)) { t.ended = true; return false; }
            t.pendingTick = t.absoluteTick;
            t.pendingIsTempo = false;
            t.pendingIsSysEx = true;
            t.pendingSysExPos = t.file.curPosition();
            t.pendingSysExLen = len;
            if (len > 0) t.file.seekCur(len);
            return true;
        } else if (status >= 0x80 && status < 0xF0) {
            // Channel voice/mode message.
            uint8_t need = channelMessageDataBytes(status);
            uint8_t d1 = 0, d2 = 0;

            if (haveFirstDataByte) {
                d1 = firstDataByte;
                if (need >= 2) {
                    int r = t.file.read();
                    if (r < 0) { t.ended = true; return false; }
                    d2 = (uint8_t)r;
                }
            } else {
                if (need >= 1) {
                    int r = t.file.read();
                    if (r < 0) { t.ended = true; return false; }
                    d1 = (uint8_t)r;
                }
                if (need >= 2) {
                    int r = t.file.read();
                    if (r < 0) { t.ended = true; return false; }
                    d2 = (uint8_t)r;
                }
            }

            t.pendingTick = t.absoluteTick;
            t.pendingIsTempo = false;
            t.pendingIsSysEx = false;
            t.pendingStatus = status;
            t.pendingData1 = d1;
            t.pendingData2 = d2;
            t.pendingLen = 1 + need;
            return true;
        } else {
            // Other system messages (0xF1-0xF6, 0xF8-0xFE) shouldn't
            // appear in a standard MIDI file; bail on this track rather
            // than risk mis-parsing the rest of the stream.
            t.ended = true;
            return false;
        }
    }
}

void MidiPlayer::sendPendingSysEx(TrackReader& t) {
    uint32_t len = t.pendingSysExLen;
    if (len == 0 || len > MIDI_SYSEX_MAX_LEN) return; // 0 = malformed; oversized -- see config.h's MIDI_SYSEX_MAX_LEN comment

    uint32_t savedPos = t.file.curPosition(); // primeTrack() already advanced past the payload for scanning
    t.file.seekSet(t.pendingSysExPos);

    static uint8_t buf[MIDI_SYSEX_MAX_LEN + 1]; // +1 for the F0 this reconstructs below
    buf[0] = 0xF0;
    bool ok = t.file.read(buf + 1, len) == (int)len;

    t.file.seekSet(savedPos); // restore, so the next primeTrack(idx) continues from the right place
    if (ok) MidiOutput::sendSysExRaw(buf, len + 1);
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

    FsFile hdr = sd.open(path, O_RDONLY);
    if (!hdr) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    char id[5];
    uint32_t len;
    if (!readChunkHeader(hdr, id, len) || strncmp(id, "MThd", 4) != 0 || len < 6) {
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

    if (_division & 0x8000) {
        strncpy(_error, "SMPTE time division not supported", sizeof(_error) - 1);
        hdr.close();
        _state = STATE_ERROR;
        return false;
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
    if (len > 6) hdr.seekCur(len - 6); // tolerate padded header chunks

    _numTracks = 0;
    for (uint16_t i = 0; i < ntrks; i++) {
        char tid[5];
        uint32_t tlen;
        if (!readChunkHeader(hdr, tid, tlen)) break;
        uint32_t trackStart = hdr.curPosition();

        if (strncmp(tid, "MTrk", 4) != 0) {
            hdr.seekCur(tlen); // unknown chunk type, skip it
            continue;
        }

        TrackReader& t = _tracks[_numTracks];
        t = TrackReader(); // reset in case this slot was used previously
        if (!t.file.open(path, O_RDONLY)) {
            hdr.seekCur(tlen);
            continue;
        }
        t.file.seekSet(trackStart);
        t.chunkEnd = trackStart + tlen;

        hdr.seekSet(trackStart + tlen); // advance master cursor past this track

        if (primeTrack(_numTracks)) {
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
    _globalTick = 0;
    _originTick = 0;
    _originMicros = micros();
    _elapsedMsFrozen = 0;
    _lastResumeMicros = micros();
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
    _originTick = _globalTick;
    _originMicros = micros();
    _lastResumeMicros = micros();
    _state = STATE_PLAYING;
}

void MidiPlayer::pause() {
    if (_state != STATE_PLAYING) return;
    _elapsedMsFrozen += (micros() - _lastResumeMicros) / 1000;
    _state = STATE_PAUSED;
    MidiOutput::allNotesOffAllChannels();
}

void MidiPlayer::stop() {
    MidiOutput::allNotesOffAllChannels();
    close();
}

uint32_t MidiPlayer::elapsedMs() const {
    uint32_t extra = (_state == STATE_PLAYING) ? (micros() - _lastResumeMicros) / 1000 : 0;
    return _elapsedMsFrozen + extra;
}

uint32_t MidiPlayer::advanceSilentlyTo(uint32_t targetMs, bool& reachedEnd) {
    uint64_t targetUs = (uint64_t)targetMs * 1000;
    uint64_t simulatedUs = (uint64_t)elapsedMs() * 1000;
    reachedEnd = false;

    for (;;) {
        int nt = findNextTrack();
        if (nt < 0) { reachedEnd = true; break; }

        TrackReader& t = _tracks[nt];
        uint32_t deltaTicks = t.pendingTick - _globalTick;
        uint32_t effectiveUsPerQuarter = (uint32_t)(_usPerQuarter / _tempoScale);
        uint64_t deltaUs = ((uint64_t)deltaTicks * effectiveUsPerQuarter) / _division;

        if (simulatedUs + deltaUs > targetUs) break; // next event is past the target -- stop here

        simulatedUs += deltaUs;
        _globalTick = t.pendingTick;

        if (t.pendingIsTempo) {
            _usPerQuarter = t.pendingTempoUsPerQuarter;
        } else if (!t.pendingIsSysEx) {
            uint8_t type = t.pendingStatus & 0xF0;
            if (type != 0x80 && type != 0x90) { // suppress Note On/Off only
                MidiOutput::sendRaw(t.pendingStatus, t.pendingData1, t.pendingData2, t.pendingLen);
            }
        }
        // SysEx: skipped entirely during a seek, not re-sent (see header comment).

        primeTrack(nt);
    }

    return reachedEnd ? (uint32_t)(simulatedUs / 1000) : targetMs;
}

void MidiPlayer::seekTo(uint32_t targetMs) {
    if (_state != STATE_PLAYING && _state != STATE_PAUSED && _state != STATE_DONE) return;

    bool wasPlaying = (_state == STATE_PLAYING);
    MidiOutput::allNotesOffAllChannels();

    // No random-access index exists (see header comment) -- a backward
    // seek, or one starting from STATE_DONE (every track already ended),
    // has no choice but to reopen and re-scan from the top. A forward
    // seek instead just continues from the current track cursors.
    bool needsRewind = (_state == STATE_DONE) || (targetMs < elapsedMs());
    if (needsRewind) {
        if (!load(_path)) return; // failure leaves _state == STATE_ERROR, same as restartFromTop()
    }

    bool reachedEnd = false;
    uint32_t reachedMs = advanceSilentlyTo(targetMs, reachedEnd);

    _originTick = _globalTick;
    _originMicros = micros();
    _elapsedMsFrozen = reachedMs;
    _lastResumeMicros = micros();
    _state = reachedEnd ? STATE_DONE : (wasPlaying ? STATE_PLAYING : STATE_PAUSED);
}

void MidiPlayer::adjustTempoScale(float delta) {
    setTempoScale(_tempoScale + delta);
}

void MidiPlayer::setTempoScale(float scale) {
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.0f) scale = 2.0f;
    _tempoScale = scale;
}

uint16_t MidiPlayer::currentBPM() const {
    uint32_t effectiveUs = (uint32_t)(_usPerQuarter / _tempoScale);
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
            _state = STATE_DONE;
            MidiOutput::allNotesOffAllChannels();
            return;
        }

        TrackReader& t = _tracks[nt];
        uint32_t deltaTicks = t.pendingTick - _originTick;
        uint32_t effectiveUsPerQuarter = (uint32_t)(_usPerQuarter / _tempoScale);
        uint32_t due = _originMicros +
                       (uint32_t)(((uint64_t)deltaTicks * effectiveUsPerQuarter) / _division);

        uint32_t now = micros();
        if ((int32_t)(now - due) < 0) return; // not due yet, try again next update()

        if (t.pendingIsTempo) {
            _usPerQuarter = t.pendingTempoUsPerQuarter;
        } else if (t.pendingIsSysEx) {
            sendPendingSysEx(t);
        } else {
            MidiOutput::sendRaw(t.pendingStatus, t.pendingData1, t.pendingData2, t.pendingLen);
        }

        _globalTick = t.pendingTick;
        _originTick = t.pendingTick;
        _originMicros = due;

        primeTrack(nt);
    }
}
