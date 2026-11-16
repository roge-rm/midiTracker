#pragma once
#include <stdint.h>

// The MIDI recording, SysEx capture and SysEx send screens. Core 0 only.
//
// These run while MIDI streams in, so the *Live() calls repaint only the
// fields that changed, each in its own small strip. ui.cpp fills the
// structs from the recorders and the player; the host renderer fills them
// with made-up data.
namespace UiRec {

// ---- recording and capture ---------------------------------------------------

enum Kind : uint8_t { KIND_MIDI, KIND_SYSEX };
enum RecState : uint8_t { REC_IDLE, REC_WAITING, REC_RECORDING, REC_ERROR };

struct RecInfo {
    Kind kind = KIND_MIDI;
    const char* name = nullptr;  // with its extension
    RecState state = REC_IDLE;
    const char* error = nullptr; // REC_ERROR only
    uint32_t elapsedMs = 0;
    uint32_t count = 0;          // events, or whole messages
    uint32_t bytes = 0;          // SysEx: bytes of whole messages kept
    uint32_t dropped = 0;        // SysEx: messages cut off
    bool active = false;         // something arrived just now
};

// Rows in the MIDI-in log.
const int LOG_ROWS = 8;

// The whole screen. log is the newest entries, oldest first, at most
// LOG_ROWS of them.
void recDraw(const RecInfo& r, const char* const* log, int logCount);

// Time, counts and the activity dot, only what changed. Call while
// REC_RECORDING.
void recLive(const RecInfo& r);

// Repaints the log rows whose text changed.
void recLog(const char* const* log, int logCount);

// Makes the next recLive(), recLog() or sendLive() repaint every live
// field. Call after a full draw that went out through a transition, whose
// bands may show older values than the ones remembered.
void invalidate();

// Discard confirm over the screen: NAV keeps going, ENT discards.
void discardCard(Kind kind, const char* name);

// ---- SysEx send ----------------------------------------------------------------

enum SendState : uint8_t { SEND_IDLE, SEND_SENDING, SEND_DONE, SEND_ERROR };

struct SendInfo {
    const char* name = nullptr;
    SendState state = SEND_IDLE;
    const char* error = nullptr;
    uint32_t elapsedMs = 0;
    uint32_t bytesSent = 0, totalBytes = 0;
    uint32_t messages = 0, skipped = 0, junk = 0;
    bool active = false; // bytes went out just now
};

void sendDraw(const SendInfo& s);

// Time, counts and the progress bar, only what changed. Call while
// SEND_SENDING.
void sendLive(const SendInfo& s);

// ---- convert -------------------------------------------------------------------

enum ConvState : uint8_t { CONV_RUNNING, CONV_DONE, CONV_FAILED, CONV_CANCELLED };

struct ConvInfo {
    const char* name = nullptr;    // the source file
    const char* target = nullptr;  // "WAV 16-bit 44.1k mono"
    const char* outName = nullptr; // the new file's name
    ConvState state = CONV_RUNNING;
    const char* message = nullptr; // CONV_FAILED and CONV_CANCELLED, one clause
    uint32_t elapsedMs = 0;
    uint32_t leftMs = UINT32_MAX;  // UINT32_MAX until known
    int permille = 0;
};

void convDraw(const ConvInfo& c);

// Time, time left, percent and the bar, only what changed. Call while
// CONV_RUNNING.
void convLive(const ConvInfo& c);

} // namespace UiRec
