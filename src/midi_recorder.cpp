#include "midi_recorder.h"
#include "sd_card.h"
#include "smf.h"
#include <string.h>
#include <hardware/timer.h>

namespace {
// _file.write() doesn't hit the card per call: SdFat keeps the current
// 512-byte sector in RAM and only writes it out when it fills or on
// sync()/close(). The interval is how much a power cut can lose.
const uint32_t SYNC_INTERVAL_MS = 5000;
// service() holds the sync back until input has been quiet this long, so
// it doesn't land in the middle of a dense dump, but no longer than
// SYNC_MAX_MS after the last one.
const uint32_t SYNC_QUIET_MS = 200;
const uint32_t SYNC_MAX_MS = 15000;

const char* const MSG_FAILED_LOST = "Card full or write failed, not saved";

void setError(char* dst, size_t size, const char* msg) {
    strncpy(dst, msg, size - 1);
    dst[size - 1] = '\0';
}
} // namespace

bool recorderTempPath(const char* path, char* out, size_t outSize) {
    const char* slash = strrchr(path, '/');
    int dirLen = slash ? (int)(slash - path) + 1 : 0;
    int n = snprintf(out, outSize, "%.*s.%s.part", dirLen, path, path + dirLen);
    return n > 0 && (size_t)n < outSize;
}

// Prefix for a take's free name: DUMP for .syx, REC otherwise.
static const char* takePrefix(const char* path) {
    const char* dot = strrchr(path, '.');
    return (dot && strcasecmp(dot, ".syx") == 0) ? "DUMP" : "REC";
}

bool recorderFreeName(const char* path, char* out, size_t outSize) {
    const char* slash = strrchr(path, '/');
    int dirLen = slash ? (int)(slash - path) + 1 : 0;
    const char* dot = strrchr(path + dirLen, '.');
    const char* ext = dot ? dot : "";
    for (int n = 1; n <= 999; n++) {
        int len = snprintf(out, outSize, "%.*s%s%03d%s", dirLen, path, takePrefix(path), n, ext);
        if (len <= 0 || (size_t)len >= outSize) return false;
        if (!sdExists(out)) return true;
    }
    return false;
}

bool recorderPlaceTake(const char* tmp, const char* path, bool keepOld, char* savedAs, size_t savedAsSize) {
    savedAs[0] = '\0';
    // A power cut between the remove and the rename leaves only the
    // temp, which recorderRecoverTemp() picks up later.
    if (!sdExists(path) || (!keepOld && sdRemove(path))) {
        if (sdRename(tmp, path)) return true;
    }
    char freePath[RECORDER_PATH_MAX + 8];
    if (!recorderFreeName(path, freePath, sizeof(freePath)) || !sdRename(tmp, freePath)) return false;
    const char* slash = strrchr(freePath, '/');
    setError(savedAs, savedAsSize, slash ? slash + 1 : freePath);
    return true;
}

bool recorderSettleTake(const char* written, const char* path, bool replace, bool failed, bool synced,
                        const char* what, char* err, size_t errSize) {
    bool problem = failed || !synced;
    const char* cause = failed ? "Write failed" : "Sync failed";
    if (replace) {
        char savedAs[48];
        if (!recorderPlaceTake(written, path, problem, savedAs, sizeof(savedAs))) {
            snprintf(err, errSize, "Save failed, %s left in a temp file", what);
            return false;
        }
        if (savedAs[0]) {
            snprintf(err, errSize, "%s, saved as %s", problem ? cause : "Old file locked", savedAs);
            return false;
        }
    }
    if (failed) {
        snprintf(err, errSize, "Card full or write failed, %s kept", what);
        return false;
    }
    if (!synced) {
        snprintf(err, errSize, "Sync failed, %s may be cut short", what);
        return false;
    }
    return true;
}

bool recorderRecoverTemp(const char* tmpPath) {
    const char* slash = strrchr(tmpPath, '/');
    int dirLen = slash ? (int)(slash - tmpPath) + 1 : 0;
    const char* leaf = tmpPath + dirLen;
    size_t leafLen = strlen(leaf);
    if (leaf[0] != '.' || leafLen <= 6 || strcmp(leaf + leafLen - 5, ".part") != 0) return false;

    FsFile f;
    if (!sdOpenPath(f, tmpPath, O_RDONLY)) return false;
    bool empty = f.fileSize() == 0;
    f.close();
    if (empty) return sdRemove(tmpPath);

    char realPath[RECORDER_PATH_MAX + 8];
    snprintf(realPath, sizeof(realPath), "%.*s%.*s", dirLen, tmpPath, (int)(leafLen - 6), leaf + 1);
    char savedAs[48];
    return recorderPlaceTake(tmpPath, realPath, true, savedAs, sizeof(savedAs));
}

const char* MidiRecorder::writePath(char* buf, size_t size) const {
    if (!_replace) return _path;
    recorderTempPath(_path, buf, size); // arm() already checked it fits
    return buf;
}

void MidiRecorder::writeBytes(const uint8_t* buf, size_t n) {
    if (!_writeOk) return;
    if (_file.write(buf, n) != n) {
        _writeOk = false;
        return;
    }
    _bytesWritten += n;
}

void MidiRecorder::writeVarLen(uint32_t value) {
    uint8_t buf[5];
    writeBytes(buf, Smf::encodeVarLen(value, buf));
}

// Marks the end of a whole event: what finish() keeps after a failure.
void MidiRecorder::endEvent() {
    if (!_writeOk) return;
    _goodPos = (uint32_t)_file.curPosition();
    _goodBytes = _bytesWritten;
}

bool MidiRecorder::arm(const char* path, bool replace) {
    stop(); // finalize any previous session first
    _error[0] = '\0';
    _eventCount = 0;
    _lastEventMs = 0;
    _writeOk = true;
    _goodPos = 0;
    _goodBytes = 0;
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

void MidiRecorder::writeHeader() {
    char tmp[RECORDER_PATH_MAX + 8];
    if (!sdOpenPath(_file, writePath(tmp, sizeof(tmp)), O_RDWR | O_CREAT | O_TRUNC)) {
        setError(_error, sizeof(_error), "could not create file");
        _state = STATE_ERROR;
        return;
    }

    // MThd: format 0 (events already merged into one stream as they're
    // captured), 1 track, `_division` ticks per quarter note.
    const uint8_t mthd[14] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6,
        0, 0, // format 0
        0, 1, // ntrks = 1
        (uint8_t)(_division >> 8), (uint8_t)(_division & 0xFF),
    };
    writeBytes(mthd, sizeof(mthd));

    // MTrk header with a placeholder length, patched in by finish() once
    // the final byte count is known.
    writeBytes((const uint8_t*)"MTrk", 4);
    _mtrkLenPos = (uint32_t)_file.curPosition();
    const uint8_t zero[4] = {0, 0, 0, 0};
    writeBytes(zero, 4);
    _bytesWritten = 0; // counts only bytes after the length field, from here on

    // Fixed synthetic tempo meta event so playback tools show correct
    // real-world timing for what is otherwise just wall-clock capture.
    const uint8_t tempoEvt[6] = {
        0xFF, 0x51, 0x03,
        (uint8_t)(_usPerQuarter >> 16), (uint8_t)(_usPerQuarter >> 8), (uint8_t)_usPerQuarter,
    };
    writeVarLen(0);
    writeBytes(tempoEvt, sizeof(tempoEvt));
    endEvent(); // _goodPos stays 0 if the header didn't make it

    _startMicros = time_us_64();
    _lastTick = 0;
    _lastSyncMs = millis();
    _state = STATE_RECORDING;
}

bool MidiRecorder::prepareEvent() {
    if (_state == STATE_ARMED) writeHeader();
    return _state == STATE_RECORDING && _writeOk;
}

uint32_t MidiRecorder::nextDeltaTick() {
    uint64_t elapsedUs = time_us_64() - _startMicros;
    uint32_t absTick = (uint32_t)((elapsedUs * _division) / _usPerQuarter);
    uint32_t deltaTick = absTick - _lastTick;
    _lastTick = absTick;
    return deltaTick;
}

void MidiRecorder::feed(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
    if (!prepareEvent()) return;

    writeVarLen(nextDeltaTick());
    const uint8_t evt[3] = {status, data1, data2};
    writeBytes(evt, 1 + len);
    _lastEventMs = millis();
    endEvent();
    if (_writeOk) _eventCount++;
}

void MidiRecorder::feedSysEx(const uint8_t* data, size_t len) {
    if (len < 2) return; // smaller than a bare F0 F7 -- not a real message
    if (!prepareEvent()) return;

    // SMF SysEx event: F0 <varlen length> <bytes after F0, including the
    // trailing F7>. `data` already starts with F0 (see this method's
    // header comment), so the length is len-1 and the payload is
    // data+1..data+len-1.
    writeVarLen(nextDeltaTick());
    const uint8_t f0 = 0xF0;
    writeBytes(&f0, 1);
    writeVarLen((uint32_t)(len - 1));
    writeBytes(data + 1, len - 1);
    _lastEventMs = millis();
    endEvent();
    if (_writeOk) _eventCount++;
}

void MidiRecorder::service() {
    if (_state != STATE_RECORDING) return;
    uint32_t now = millis();
    if (_writeOk && now - _lastSyncMs >= SYNC_INTERVAL_MS &&
        (now - _lastEventMs >= SYNC_QUIET_MS || now - _lastSyncMs >= SYNC_MAX_MS)) {
        if (!_file.sync()) _writeOk = false;
        _lastSyncMs = now;
    }
    if (!_writeOk) {
        finish(); // always false here, with the error set
        _state = STATE_ERROR;
    }
}

bool MidiRecorder::finish() {
    bool failed = !_writeOk;
    if (failed) {
        // Drop the part-written event. If that fails the length field
        // below still ends the track at the last whole event.
        _file.clearWriteError();
        if (_goodPos == 0 || !_file.truncate(_goodPos)) _file.seekSet(_goodPos);
        _bytesWritten = _goodBytes;
        _writeOk = true;
    }
    if (_goodPos > 0) {
        writeVarLen(0);
        const uint8_t eot[3] = {0xFF, 0x2F, 0x00}; // End of Track
        writeBytes(eot, sizeof(eot));
        if (!_writeOk) {
            // No room for it: the track just ends at the last event, which
            // players read fine.
            _file.clearWriteError();
            _file.truncate(_goodPos);
            _bytesWritten = _goodBytes;
            _writeOk = true;
        }
    }
    const uint8_t lenBytes[4] = {
        (uint8_t)(_bytesWritten >> 24), (uint8_t)(_bytesWritten >> 16),
        (uint8_t)(_bytesWritten >> 8), (uint8_t)_bytesWritten,
    };
    bool synced = _file.seekSet(_mtrkLenPos) && _file.write(lenBytes, 4) == 4 && _file.sync();
    _file.close();

    char tmp[RECORDER_PATH_MAX + 8];
    const char* written = writePath(tmp, sizeof(tmp));
    if (_goodPos == 0 || _eventCount == 0) {
        // No event made it, so there's no take to keep.
        sdRemove(written);
        setError(_error, sizeof(_error), MSG_FAILED_LOST);
        return false;
    }
    return recorderSettleTake(written, _path, _replace, failed, synced, "take", _error, sizeof(_error));
}

bool MidiRecorder::stop() {
    if (_state == STATE_RECORDING && !finish()) {
        _state = STATE_ERROR;
        return false;
    }
    // STATE_ARMED: no file was ever created (still waiting for the first
    // event), so there's nothing to finalize.
    _state = STATE_IDLE;
    return true;
}

void MidiRecorder::cancel() {
    if (_state == STATE_RECORDING) {
        _file.close();
        char tmp[RECORDER_PATH_MAX + 8];
        sdRemove(writePath(tmp, sizeof(tmp)));
    }
    // STATE_ARMED: no file was ever created (still waiting for the first
    // event), so there's nothing on disk to remove. A file being replaced
    // is untouched either way.
    _state = STATE_IDLE;
}

uint32_t MidiRecorder::elapsedMs() const {
    if (_state != STATE_RECORDING) return 0;
    return (uint32_t)((time_us_64() - _startMicros) / 1000);
}

uint32_t MidiRecorder::msSinceLastEvent() const {
    if (_lastEventMs == 0) return UINT32_MAX;
    return millis() - _lastEventMs;
}
