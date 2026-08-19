#include "midi_recorder.h"
#include "sd_card.h"
#include <string.h>

namespace {
// _file.write() below does NOT hit the card per call -- SdFat already
// buffers writes in a RAM-resident 512-byte sector cache and only issues
// real card I/O when that sector fills or sync()/close() forces it. This
// interval controls only the forced early flush (so a power loss loses at
// most this much), trading a bit of durability for fewer partial-sector
// writes; it's not standing in for missing buffering. MIDI event data
// (a few bytes/event) is tiny compared to flash wear limits regardless.
const uint32_t SYNC_INTERVAL_MS = 5000;
} // namespace

void MidiRecorder::writeBytes(const uint8_t* buf, size_t n) {
    _file.write(buf, n);
    _bytesWritten += n;
}

// Standard MIDI file variable-length quantity: 7 bits per byte,
// most-significant group first, continuation bit set on every byte but
// the last.
void MidiRecorder::writeVarLen(uint32_t value) {
    uint32_t buffer = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer <<= 8;
        buffer |= 0x80 | (value & 0x7F);
    }
    for (;;) {
        uint8_t b = (uint8_t)(buffer & 0xFF);
        writeBytes(&b, 1);
        if (buffer & 0x80) buffer >>= 8;
        else break;
    }
}

bool MidiRecorder::arm(const char* path) {
    stop(); // finalize/discard any previous session first
    _error[0] = '\0';

    if (sd.exists(path)) {
        strncpy(_error, "file already exists", sizeof(_error) - 1);
        _state = STATE_ERROR;
        return false;
    }

    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _eventCount = 0;
    _state = STATE_ARMED;
    return true;
}

void MidiRecorder::writeHeader() {
    if (!_file.open(_path, O_RDWR | O_CREAT | O_TRUNC)) {
        strncpy(_error, "could not create file", sizeof(_error) - 1);
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
    _file.write(mthd, sizeof(mthd));

    // MTrk header with a placeholder length, patched in by stop() once
    // the final byte count is known.
    _file.write((const uint8_t*)"MTrk", 4);
    _mtrkLenPos = _file.curPosition();
    const uint8_t zero[4] = {0, 0, 0, 0};
    _file.write(zero, 4);
    _bytesWritten = 0; // counts only bytes after the length field, from here on

    // Fixed synthetic tempo meta event so playback tools show correct
    // real-world timing for what is otherwise just wall-clock capture.
    const uint8_t tempoEvt[6] = {
        0xFF, 0x51, 0x03,
        (uint8_t)(_usPerQuarter >> 16), (uint8_t)(_usPerQuarter >> 8), (uint8_t)_usPerQuarter,
    };
    writeVarLen(0);
    writeBytes(tempoEvt, sizeof(tempoEvt));

    _startMicros = micros();
    _lastTick = 0;
    _lastSyncMs = millis();
    _state = STATE_RECORDING;
}

bool MidiRecorder::prepareEvent() {
    if (_state == STATE_ARMED) {
        writeHeader();
        if (_state != STATE_RECORDING) return false; // writeHeader() failed
    }
    return _state == STATE_RECORDING;
}

uint32_t MidiRecorder::nextDeltaTick() {
    uint32_t elapsedUs = micros() - _startMicros;
    uint32_t absTick = (uint32_t)(((uint64_t)elapsedUs * _division) / _usPerQuarter);
    uint32_t deltaTick = absTick - _lastTick;
    _lastTick = absTick;
    return deltaTick;
}

void MidiRecorder::maybeSync() {
    uint32_t nowMs = millis();
    _lastEventMs = nowMs; // unconditional, unlike the sync below -- see msSinceLastEvent()
    if (nowMs - _lastSyncMs >= SYNC_INTERVAL_MS) {
        _file.sync();
        _lastSyncMs = nowMs;
    }
}

void MidiRecorder::feed(uint8_t status, uint8_t data1, uint8_t data2, uint8_t len) {
    if (!prepareEvent()) return;

    writeVarLen(nextDeltaTick());
    const uint8_t evt[3] = {status, data1, data2};
    writeBytes(evt, 1 + len);
    _eventCount++;
    maybeSync();
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
    _eventCount++;
    maybeSync();
}

void MidiRecorder::stop() {
    if (_state == STATE_RECORDING) {
        writeVarLen(0);
        const uint8_t eot[3] = {0xFF, 0x2F, 0x00}; // End of Track
        writeBytes(eot, sizeof(eot));

        uint32_t endPos = _file.curPosition();
        const uint8_t lenBytes[4] = {
            (uint8_t)(_bytesWritten >> 24), (uint8_t)(_bytesWritten >> 16),
            (uint8_t)(_bytesWritten >> 8), (uint8_t)_bytesWritten,
        };
        _file.seekSet(_mtrkLenPos);
        _file.write(lenBytes, 4);
        _file.seekSet(endPos);

        _file.sync();
        _file.close();
    }
    // STATE_ARMED: no file was ever created (still waiting for the first
    // event), so there's nothing to finalize.
    _state = STATE_IDLE;
}

void MidiRecorder::cancel() {
    if (_state == STATE_RECORDING) {
        _file.close();
        sd.remove(_path);
    }
    // STATE_ARMED: no file was ever created (still waiting for the first
    // event), so there's nothing on disk to remove.
    _state = STATE_IDLE;
}

uint32_t MidiRecorder::elapsedMs() const {
    if (_state != STATE_RECORDING) return 0;
    return (micros() - _startMicros) / 1000;
}

uint32_t MidiRecorder::msSinceLastEvent() const {
    if (_lastEventMs == 0) return UINT32_MAX;
    return millis() - _lastEventMs;
}
