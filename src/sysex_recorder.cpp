#include "sysex_recorder.h"
#include "sd_card.h"
#include "config.h"
#include <string.h>

namespace {
// A bulk dump can send many messages back to back with no real gap
// between them (e.g. one per patch slot) -- syncing every single one
// would mean a card-I/O stall on the hot path of every incoming message,
// risking dropped/missed bytes on a fast dump. Same periodic-sync
// throttling MidiRecorder uses, same reasoning: a power loss loses at
// most this much.
const uint32_t SYNC_INTERVAL_MS = 5000;
} // namespace

bool SysExRecorder::arm(const char* path) {
    stop(); // finalize/discard any previous session first
    _error[0] = '\0';

    if (sd.exists(path)) {
        strncpy(_error, "file already exists", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _messageCount = 0;
    _lastMessageMs = 0;
    _state = STATE_ARMED;
    return true;
}

void SysExRecorder::feed(const uint8_t* data, size_t len) {
    if (len < 2 || len > MIDI_SYSEX_MAX_LEN) return; // malformed, or bigger than this firmware buffers anywhere -- see config.h

    if (_state == STATE_ARMED) {
        if (!_file.open(_path, O_RDWR | O_CREAT | O_TRUNC)) {
            strncpy(_error, "could not create file", sizeof(_error) - 1);
            _state = STATE_ERROR;
            return;
        }
        _startMs = millis();
        _lastSyncMs = _startMs;
        _state = STATE_RECORDING;
    }
    if (_state != STATE_RECORDING) return;

    _file.write(data, len); // raw, boundary-inclusive -- messages are self-delimiting via their own F0/F7, no framing needed
    _messageCount++;
    _lastMessageMs = millis();

    if (_lastMessageMs - _lastSyncMs >= SYNC_INTERVAL_MS) {
        _file.sync();
        _lastSyncMs = _lastMessageMs;
    }
}

void SysExRecorder::stop() {
    if (_state == STATE_RECORDING) {
        _file.sync();
        _file.close();
    }
    _state = STATE_IDLE;
}

void SysExRecorder::cancel() {
    if (_state == STATE_RECORDING) {
        _file.close();
        sd.remove(_path);
    }
    _state = STATE_IDLE;
}

uint32_t SysExRecorder::elapsedMs() const {
    if (_state != STATE_RECORDING) return 0;
    return millis() - _startMs;
}

uint32_t SysExRecorder::msSinceLastMessage() const {
    if (_lastMessageMs == 0) return UINT32_MAX;
    return millis() - _lastMessageMs;
}
