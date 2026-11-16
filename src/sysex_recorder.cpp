#include "sysex_recorder.h"
#include "sd_card.h"
#include <string.h>

namespace {
// Same sync timing as MidiRecorder: every SYNC_INTERVAL_MS, held back
// until input has been quiet for SYNC_QUIET_MS so a bulk dump isn't
// interrupted, or SYNC_MAX_MS has passed. Only between messages, so a
// power cut never leaves part of one in the file.
const uint32_t SYNC_INTERVAL_MS = 5000;
const uint32_t SYNC_QUIET_MS = 200;
const uint32_t SYNC_MAX_MS = 15000;
// A message with no bytes for this long is dropped (cable pulled).
const uint32_t OPEN_MESSAGE_TIMEOUT_MS = 1000;

const char* const MSG_FAILED_LOST = "Card full or write failed, not saved";

void setError(char* dst, size_t size, const char* msg) {
    strncpy(dst, msg, size - 1);
    dst[size - 1] = '\0';
}
} // namespace

const char* SysExRecorder::writePath(char* buf, size_t size) const {
    if (!_replace) return _path;
    recorderTempPath(_path, buf, size); // arm() already checked it fits
    return buf;
}

bool SysExRecorder::arm(const char* path, bool replace) {
    stop(); // finalize any previous session first
    _error[0] = '\0';
    _messageCount = 0;
    _droppedCount = 0;
    _msgSource = NO_SOURCE;
    _lastMessageMs = 0;
    _writeOk = true;
    _goodPos = 0;
    _replace = replace;

    char tmp[RECORDER_PATH_MAX + 8];
    if (strlen(path) >= sizeof(_path) || (replace && !recorderTempPath(path, tmp, sizeof(tmp)))) {
        setError(_error, sizeof(_error), "path too long");
        _state = STATE_ERROR;
        return false;
    }
    if (replace) {
        // Left by a power cut or a failed save: move it out of the way.
        if (sdExists(tmp) && !recorderRecoverTemp(tmp)) {
            setError(_error, sizeof(_error), "old temp file in the way");
            _state = STATE_ERROR;
            return false;
        }
    } else if (sdExists(path)) {
        setError(_error, sizeof(_error), "file already exists");
        _state = STATE_ERROR;
        return false;
    }

    strcpy(_path, path);
    _state = STATE_ARMED;
    return true;
}

void SysExRecorder::beginMessage(uint8_t source) {
    if (_state == STATE_ARMED) {
        char tmp[RECORDER_PATH_MAX + 8];
        if (!sdOpenPath(_file, writePath(tmp, sizeof(tmp)), O_RDWR | O_CREAT | O_TRUNC)) {
            setError(_error, sizeof(_error), "could not create file");
            _state = STATE_ERROR;
            return;
        }
        _startMs = millis();
        _lastSyncMs = _startMs;
        _state = STATE_RECORDING;
    }
    if (_state != STATE_RECORDING || !_writeOk) return;
    if (_msgSource != NO_SOURCE) {
        if (source != _msgSource) { // the other source is mid-message
            _droppedCount++;
            return;
        }
        abortMessage(source);
    }
    _msgSource = source;
    _lastMessageMs = millis();
}

// Raw, boundary-inclusive: messages are self-delimiting via their own
// F0/F7, no framing needed. Written to SdFat's sector cache; syncing is
// left to service().
void SysExRecorder::writeMessage(uint8_t source, const uint8_t* data, size_t len) {
    if (_state != STATE_RECORDING || !_writeOk || source != _msgSource) return;
    if (_file.write(data, len) != len) {
        _writeOk = false;
        return;
    }
    _lastMessageMs = millis();
}

bool SysExRecorder::endMessage(uint8_t source) {
    if (_state != STATE_RECORDING || !_writeOk || source != _msgSource) return false;
    _msgSource = NO_SOURCE;
    _goodPos = (uint32_t)_file.curPosition();
    _messageCount++;
    _lastMessageMs = millis();
    return true;
}

// Cuts the file back to the end of the last whole message.
void SysExRecorder::abortMessage(uint8_t source) {
    if (_state != STATE_RECORDING || source != _msgSource) return;
    _msgSource = NO_SOURCE;
    _droppedCount++;
    if (_writeOk && !_file.truncate(_goodPos)) _writeOk = false;
}

void SysExRecorder::feed(const uint8_t* data, size_t len) {
    if (len < 2 || data[0] != 0xF0 || data[len - 1] != 0xF7) return;
    beginMessage(0);
    writeMessage(0, data, len);
    endMessage(0);
}

void SysExRecorder::service() {
    if (_state != STATE_RECORDING) return;
    uint32_t now = millis();
    if (_msgSource != NO_SOURCE && now - _lastMessageMs >= OPEN_MESSAGE_TIMEOUT_MS) abortMessage(_msgSource);
    if (_writeOk && _msgSource == NO_SOURCE && now - _lastSyncMs >= SYNC_INTERVAL_MS &&
        (now - _lastMessageMs >= SYNC_QUIET_MS || now - _lastSyncMs >= SYNC_MAX_MS)) {
        if (!_file.sync()) _writeOk = false;
        _lastSyncMs = now;
    }
    if (!_writeOk) {
        finish(); // always false here, with the error set
        _state = STATE_ERROR;
    }
}

// Closes the file, keeping only whole messages, and moves it into place.
// Removes it only if no whole message made it. False with _error set on
// any problem.
bool SysExRecorder::finish() {
    if (_msgSource != NO_SOURCE) { // stopped mid-message
        _msgSource = NO_SOURCE;
        _droppedCount++;
    }
    bool failed = !_writeOk;
    if (failed) _file.clearWriteError();
    if (failed || _file.fileSize() > _goodPos)
        _file.truncate(_goodPos); // if this fails the tail is a part message, still kept
    bool synced = _file.sync();
    _file.close();
    _writeOk = true;

    char tmp[RECORDER_PATH_MAX + 8];
    const char* written = writePath(tmp, sizeof(tmp));
    if (_messageCount == 0) {
        sdRemove(written);
        setError(_error, sizeof(_error), failed ? MSG_FAILED_LOST : "No whole message came in, not saved");
        return false;
    }
    return recorderSettleTake(written, _path, _replace, failed, synced, "dump", _error, sizeof(_error));
}

bool SysExRecorder::stop() {
    if (_state == STATE_RECORDING && !finish()) {
        _state = STATE_ERROR;
        return false;
    }
    _state = STATE_IDLE;
    return true;
}

void SysExRecorder::cancel() {
    _msgSource = NO_SOURCE;
    if (_state == STATE_RECORDING) {
        _file.close();
        char tmp[RECORDER_PATH_MAX + 8];
        sdRemove(writePath(tmp, sizeof(tmp)));
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
