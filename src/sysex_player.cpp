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

    _bytesSent = 0;
    _messagesSent = 0;
    _startMs = millis();
    _lastSendMs = 0;
    _state = STATE_SENDING;
    return true;
}

void SysExPlayer::close() {
    if (_file) _file.close();
    _state = STATE_IDLE;
}

bool SysExPlayer::sendNextMessage() {
    int b = _file.read();
    if (b < 0) {
        _state = STATE_DONE;
        return false;
    }
    if (b != 0xF0) {
        strncpy(_error, "malformed .syx file", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    uint8_t* msgBuf = MidiOutput::sysExScratch();
    uint32_t bufLen = 1;
    msgBuf[0] = 0xF0;
    uint32_t rawLen = 1;
    for (;;) {
        int c = _file.read();
        if (c < 0) {
            strncpy(_error, "truncated .syx file", sizeof(_error) - 1);
            _state = STATE_ERROR;
            return false;
        }
        rawLen++;
        if (bufLen < MIDI_SYSEX_MAX_LEN) msgBuf[bufLen++] = (uint8_t)c;
        if (c == 0xF7) break;
    }

    _bytesSent += rawLen;
    _messagesSent++;

    // See this method's header comment on the MIDI_SYSEX_MAX_LEN skip.
    if (rawLen <= MIDI_SYSEX_MAX_LEN) MidiOutput::sendSysExRaw(msgBuf, bufLen);
    // After the send, so the gap is measured from when the message finished going out.
    _lastSendMs = millis();

    return true;
}

void SysExPlayer::update() {
    if (_state != STATE_SENDING) return;
    uint32_t now = millis();
    if (_lastSendMs != 0 && now - _lastSendMs < INTER_MESSAGE_GAP_MS) return;
    sendNextMessage();
}

uint32_t SysExPlayer::elapsedMs() const {
    if (_state == STATE_IDLE) return 0;
    return millis() - _startMs;
}

uint32_t SysExPlayer::msSinceLastSend() const {
    if (_lastSendMs == 0) return UINT32_MAX;
    return millis() - _lastSendMs;
}
