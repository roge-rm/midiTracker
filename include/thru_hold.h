#pragma once
#include <stdint.h>
#include <string.h>

// Thru channel messages held for one output while a .syx message is going
// out on it, sent once it's done (see midi_output.cpp). When full, note-ons
// are dropped first so note-offs and controllers get through; if those
// overflow too, the channel is marked for All Notes Off and sustain off.
class ThruHold {
public:
    static const uint8_t CAPACITY = 32;
    struct Msg { uint8_t status, data1, data2; };

    bool empty() const { return _n == 0 && _panic == 0; }
    uint8_t count() const { return _n; }
    uint16_t panicChannels() const { return _panic; }

    void push(uint8_t status, uint8_t data1, uint8_t data2) {
        if (_n == CAPACITY) {
            if (isNoteOn(status, data2)) return;
            uint8_t i = 0;
            while (i < _n && !isNoteOn(_q[i].status, _q[i].data2)) i++;
            if (i == _n) {
                _panic |= (uint16_t)(1u << (status & 0x0F));
                return;
            }
            memmove(&_q[i], &_q[i + 1], (size_t)(_n - i - 1) * sizeof(Msg));
            _n--;
        }
        _q[_n++] = {status, data1, data2};
    }

    // Oldest held message; only when count() > 0.
    const Msg& front() const { return _q[0]; }
    void pop() {
        memmove(&_q[0], &_q[1], (size_t)(_n - 1) * sizeof(Msg));
        _n--;
    }

    // Lowest channel still needing the panic, or -1. Clears it.
    int takePanicChannel() {
        for (int ch = 0; ch < 16; ch++) {
            if (_panic & (1u << ch)) {
                _panic &= (uint16_t)~(1u << ch);
                return ch;
            }
        }
        return -1;
    }

private:
    Msg _q[CAPACITY];
    uint8_t _n = 0;
    uint16_t _panic = 0;

    static bool isNoteOn(uint8_t status, uint8_t data2) { return (status & 0xF0) == 0x90 && data2 > 0; }
};
