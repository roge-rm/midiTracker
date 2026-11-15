#pragma once
// Standard MIDI File helpers shared by MidiPlayer, MidiRecorder and
// LooperMode's track save, load and import.
#include <stdint.h>
#include <SdFat.h>

namespace Smf {

// Reads an 8-byte chunk header: 4-char id (NUL-terminated into id) and a
// big-endian length.
inline bool readChunkHeader(FsFile& f, char id[5], uint32_t& length) {
    uint8_t buf[8];
    if (f.read(buf, 8) != 8) return false;
    id[0] = buf[0]; id[1] = buf[1]; id[2] = buf[2]; id[3] = buf[3]; id[4] = '\0';
    length = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
             ((uint32_t)buf[6] << 8) | buf[7];
    return true;
}

// Reads a variable-length quantity. readByte() returns the next byte, or
// -1 at the end. More than 4 bytes is malformed; what was read is kept.
template <typename ReadByte>
inline bool readVarLen(ReadByte readByte, uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; i++) {
        int b = readByte();
        if (b < 0) return false;
        value = (value << 7) | (uint32_t)(b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true;
}

inline bool readVarLen(FsFile& f, uint32_t& value) {
    return readVarLen([&f]() { return f.read(); }, value);
}

// Encodes value as a variable-length quantity into out, most significant
// group first, and returns the byte count. SMF allows values up to
// 0x0FFFFFFF (4 bytes); larger values come out malformed.
inline uint8_t encodeVarLen(uint32_t value, uint8_t out[5]) {
    uint32_t buffer = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer <<= 8;
        buffer |= 0x80 | (value & 0x7F);
    }
    uint8_t n = 0;
    for (;;) {
        out[n++] = (uint8_t)(buffer & 0xFF);
        if (buffer & 0x80) buffer >>= 8;
        else break;
    }
    return n;
}

// Data bytes after a channel message's status byte.
inline uint8_t channelMessageDataBytes(uint8_t status) {
    uint8_t type = status & 0xF0;
    return (type == 0xC0 || type == 0xD0) ? 1 : 2; // Program Change / Channel Pressure
}

} // namespace Smf
