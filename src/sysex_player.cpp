#include "sysex_player.h"
#include "sd_card.h"
#include "midi_output.h"
#include <string.h>

bool SysExPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!sdOpenPath(_file, path, O_RDONLY)) {
        strncpy(_error, "could not open file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }
    _totalBytes = (uint32_t)_file.fileSize();
    if (_totalBytes == 0) {
        strncpy(_error, "empty file", sizeof(_error) - 1);
        _file.close();
        _state = STATE_ERROR;
        return false;
    }

    _phase = PH_SEEK;
    _bufLen = _bufPos = 0;
    _filePos = 0;
    MidiOutput::sysExOutNewPlayback();
    _bytesSent = 0;
    _messagesSent = 0;
    _messagesSkipped = 0;
    _junkBytes = 0;
    _startMs = millis();
    _lastSendMs = 0;
    _state = STATE_SENDING;
    return true;
}

void SysExPlayer::close() {
    if (_state == STATE_SENDING && _phase == PH_SEND) MidiOutput::sysExOutEnd();
    if (_file) _file.close();
    _state = STATE_IDLE;
}

// Next byte from the file through _buf, -1 at the end or -2 if the read failed.
int SysExPlayer::readByte() {
    if (_bufPos == _bufLen) {
        int n = _file.read(_buf, BUF_SIZE);
        if (n < 0) return -2;
        if (n == 0) return -1;
        _bufLen = (uint8_t)n;
        _bufPos = 0;
        _filePos += (uint32_t)n;
    }
    return _buf[_bufPos++];
}

bool SysExPlayer::seekTo(uint32_t pos) {
    _filePos = pos;
    _bufLen = _bufPos = 0;
    return _file.seekSet(pos);
}

void SysExPlayer::readFailed() {
    strncpy(_error, "card read failed", sizeof(_error) - 1);
    _state = STATE_ERROR;
}

void SysExPlayer::finish() {
    _bytesSent = _totalBytes;
    if (_messagesSent == 0 && _messagesSkipped == 0) {
        strncpy(_error, "no SysEx in this file", sizeof(_error) - 1);
        _state = STATE_ERROR;
    } else {
        _state = STATE_DONE;
    }
}

// Skips to the next F0.
void SysExPlayer::seek(uint32_t& budget) {
    while (budget) {
        budget--;
        int b = readByte();
        if (b == -2) {
            readFailed();
            return;
        }
        if (b < 0) {
            finish();
            return;
        }
        uint32_t next = _filePos - (_bufLen - _bufPos);
        if (b == 0xF0) {
            _msgStart = next - 1;
            _phase = PH_SCAN;
            return;
        }
        _junkBytes++;
        _bytesSent = next;
    }
}

// Reads ahead to the message's F7 without sending. A message that ends
// any other way is skipped whole.
void SysExPlayer::scan(uint32_t& budget) {
    while (budget) {
        budget--;
        int b = readByte();
        if (b == -2) {
            readFailed();
            return;
        }
        if (b < 0) { // cut off by the end of the file
            _messagesSkipped++;
            finish();
            return;
        }
        if (b >= 0xF8) continue; // realtime, left out when sending
        uint32_t next = _filePos - (_bufLen - _bufPos);
        if (b == 0xF7) {
            _msgEnd = next;
            if (!seekTo(_msgStart)) {
                readFailed();
                return;
            }
            MidiOutput::sysExOutBegin();
            _phase = PH_SEND;
            return;
        }
        if (b == 0xF0) { // a new message starts before this one ended
            _messagesSkipped++;
            _msgStart = next - 1;
            _bytesSent = _msgStart;
            continue;
        }
        if (b & 0x80) { // a status byte before the F7
            _messagesSkipped++;
            _junkBytes++;
            _bytesSent = next;
            _phase = PH_SEEK;
            return;
        }
    }
}

// Streams _msgStart.._msgEnd through _buf as fast as the outputs take it.
void SysExPlayer::send() {
    for (int i = 0; i < 8; i++) {
        if (_bufPos == _bufLen) {
            if (_filePos >= _msgEnd) {
                MidiOutput::sysExOutEnd();
                _messagesSent++;
                _bytesSent = _msgEnd;
                _phase = PH_DRAIN;
                return;
            }
            uint32_t want = _msgEnd - _filePos;
            int n = _file.read(_buf, want < BUF_SIZE ? want : BUF_SIZE);
            if (n <= 0) {
                MidiOutput::sysExOutEnd();
                readFailed();
                return;
            }
            _filePos += (uint32_t)n;
            uint8_t k = 0;
            for (int j = 0; j < n; j++) {
                if (_buf[j] >= 0xF8) _junkBytes++;
                else _buf[k++] = _buf[j];
            }
            _bufLen = k;
            _bufPos = 0;
            continue;
        }
        size_t took = MidiOutput::sysExOutWrite(_buf + _bufPos, _bufLen - _bufPos);
        if (!took) return;
        _bufPos += (uint8_t)took;
        _bytesSent = _filePos - (_bufLen - _bufPos);
        _lastSendMs = millis();
    }
}

void SysExPlayer::update() {
    if (_state != STATE_SENDING) return;
    if (_phase == PH_DRAIN) {
        if (!MidiOutput::sysExOutDrained()) return;
        _gapStartMs = millis();
        _phase = PH_GAP;
    }
    if (_phase == PH_GAP) {
        if (millis() - _gapStartMs < INTER_MESSAGE_GAP_MS) return;
        _phase = PH_SEEK;
    }
    uint32_t budget = SCAN_BYTES_PER_UPDATE;
    while (_state == STATE_SENDING && budget > 0 && (_phase == PH_SEEK || _phase == PH_SCAN)) {
        if (_phase == PH_SEEK) seek(budget);
        else scan(budget);
    }
    if (_state == STATE_SENDING && _phase == PH_SEND) send();
}

uint32_t SysExPlayer::elapsedMs() const {
    if (_state == STATE_IDLE) return 0;
    return millis() - _startMs;
}

uint32_t SysExPlayer::msSinceLastSend() const {
    if (_lastSendMs == 0) return UINT32_MAX;
    return millis() - _lastSendMs;
}
