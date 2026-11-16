#pragma once
#include <stdint.h>
#include <stddef.h>

// SysEx of any length, passed on in pieces as it arrives instead of whole.
// A message is SYSEX_BEGIN, any number of SYSEX_DATA chunks (the raw bytes,
// the first starting with F0 and the last ending with F7), then SYSEX_END
// with `len` the whole message's length. SYSEX_ABORT instead of SYSEX_END
// means the message was cut off and its bytes should be thrown away.
// `source` is 0 for DIN, 1 for USB.
enum SysExStreamEvent { SYSEX_BEGIN, SYSEX_DATA, SYSEX_END, SYSEX_ABORT };
typedef void (*SysExStreamHandler)(SysExStreamEvent ev, uint8_t source, const uint8_t* data, size_t len);

// Finds SysEx in one transport's incoming bytes. Realtime bytes (F8-FF)
// inside a message are left out and don't end it; F0 or any other status
// byte before the F7 aborts it. Collects up to CHUNK bytes before passing
// them on; call flush() after each batch of input so nothing waits long.
class SysExInParser {
public:
    static const size_t CHUNK = 32;

    explicit SysExInParser(uint8_t source) : _source(source) {}

    void byte(uint8_t b, SysExStreamHandler h) {
        if (b >= 0xF8) return;
        if (b == 0xF0) {
            if (_active) abort(h);
            _active = true;
            _n = 0;
            _len = 0;
            if (h) h(SYSEX_BEGIN, _source, nullptr, 0);
            push(b, h);
            return;
        }
        if (!_active) return;
        if (b == 0xF7) {
            push(b, h);
            flush(h);
            _active = false;
            if (h) h(SYSEX_END, _source, nullptr, _len);
            return;
        }
        if (b & 0x80) {
            abort(h);
            return;
        }
        push(b, h);
    }

    void flush(SysExStreamHandler h) {
        if (_n && h) h(SYSEX_DATA, _source, _buf, _n);
        _n = 0;
    }

private:
    uint8_t _source;
    bool _active = false;
    uint8_t _n = 0;
    uint32_t _len = 0;
    uint8_t _buf[CHUNK];

    void push(uint8_t b, SysExStreamHandler h) {
        _buf[_n++] = b;
        _len++;
        if (_n == CHUNK) flush(h);
    }

    void abort(SysExStreamHandler h) {
        _n = 0;
        _active = false;
        if (h) h(SYSEX_ABORT, _source, nullptr, 0);
    }
};
