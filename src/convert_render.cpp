// Convert sources that play a file to make the audio: MIDI through the
// synth, modules through the tracker players. See convert.h.
#include "convert.h"
#include "midi_file.h"
#include "midi_output.h"
#include "synth.h"
#include "shared_ram_arena.h"
#include "mod_file.h"
#include "s3m_file.h"
#include "xm_file.h"
#include "it_file.h"
#include <new>

namespace {
const uint32_t PLAY_RATE = 44100; // the synth's and trackers' rate
}

// ---- MIDI ---------------------------------------------------------------------

bool MidiConvertSource::open(const char* path) {
    _err = nullptr;
    _ended = false;
    _frames = _endFrame = 0;
    _quiet = 0;
    _blockLeft = 0;
    _lengthTicks = 0;
    if (!_p) _p = new (std::nothrow) MidiPlayer();
    if (!_p) {
        _err = "Not enough RAM to convert";
        return false;
    }
    // The synth only: nothing goes out on DIN or USB.
    _target = (uint8_t)MidiOutput::getTarget();
    _audio = MidiOutput::audioOutputEnabled();
    MidiOutput::setTarget(MIDI_OUT_OFF);
    MidiOutput::setAudioOutput(true);
    // A clean slate as for a fresh open in the player, taken in by offlineBegin().
    Synth::resetPrograms();
    Synth::offlineBegin();
    _offline = true;
    _p->setOfflineClock(true);
    _p->setTempoScale(1.0f);
    if (!_p->load(path)) return false;
    _lengthTicks = _p->lengthTicks();
    _p->play();
    return true;
}

const char* MidiConvertSource::error() const {
    if (_err) return _err;
    return _p ? _p->errorMessage() : "";
}

uint32_t MidiConvertSource::rate() const { return PLAY_RATE; }

int MidiConvertSource::read(int16_t* out, int max) {
    if (!_p || !_offline) return READ_FAILED;
    if (_blockLeft == 0) {
        // Events due by now go to the synth, then a block is rendered.
        if (_p->state() == MidiPlayer::STATE_PLAYING) {
            _p->setOfflineMicros((uint32_t)(_frames * 1000000ull / PLAY_RATE));
            _p->update();
        }
        if (_p->trackFailed()) {
            _err = "Read failed";
            return READ_FAILED;
        }
        if (!_ended && _p->state() != MidiPlayer::STATE_PLAYING) {
            _ended = true;
            _endFrame = _frames;
            _quiet = 0;
        }
        if (_ended) {
            if (_quiet >= QUIET_FRAMES && !Synth::offlineVoicesActive()) return READ_END;
            if (_frames - _endFrame >= TAIL_MAX_FRAMES) return READ_END;
        }
        _blockLeft = UPDATE_FRAMES;
    }
    uint32_t n = (uint32_t)max < _blockLeft ? (uint32_t)max : _blockLeft;
    Synth::offlineRender(out, n);
    for (uint32_t i = 0; i < n * 2; i += 2) {
        bool quiet = abs(out[i]) <= QUIET_LEVEL && abs(out[i + 1]) <= QUIET_LEVEL;
        _quiet = quiet ? _quiet + 1 : 0;
    }
    _blockLeft -= n;
    _frames += n;
    return (int)n;
}

void MidiConvertSource::progress(uint32_t& done, uint32_t& total) const {
    total = _lengthTicks;
    done = !_p || _ended ? total : _p->currentTick();
}

void MidiConvertSource::close() {
    if (!_offline) return;
    if (_p) {
        // Freed as leaving normal play does; the player makes a new one.
        _p->close();
        delete _p;
        _p = nullptr;
    }
    // Clears MidiOutput's note list and the synth's notes, while MIDI out is still off.
    MidiOutput::allNotesOffAllChannels();
    Synth::offlineEnd();
    MidiOutput::setTarget((MidiOutTarget)_target);
    MidiOutput::setAudioOutput(_audio);
    _offline = false;
}

// ---- modules ------------------------------------------------------------------

bool TrackerConvertSource::open(const char* path) {
    _err = nullptr;
    _block = nullptr;
    _blockFrames = _blockPos = 0;
    _updateLeft = 0;
    void* at = SharedRamArena::data();
    switch (_kind) {
        case FILE_MOD:
            static_assert(sizeof(ModPlayer) <= SharedRamArena::SIZE, "ModPlayer doesn't fit SharedRamArena");
            _p = new (at) ModPlayer();
            break;
        case FILE_S3M:
            static_assert(sizeof(S3mPlayer) <= SharedRamArena::SIZE, "S3mPlayer doesn't fit SharedRamArena");
            _p = new (at) S3mPlayer();
            break;
        case FILE_XM:
            static_assert(sizeof(XmPlayer) <= SharedRamArena::SIZE, "XmPlayer doesn't fit SharedRamArena");
            _p = new (at) XmPlayer();
            break;
        case FILE_IT:
            static_assert(sizeof(ItPlayer) <= SharedRamArena::SIZE, "ItPlayer doesn't fit SharedRamArena");
            _p = new (at) ItPlayer();
            break;
        default:
            _err = "Can't read this file";
            return false;
    }
    if (!_p->load(path)) return false;
    if (!_p->beginOffline(&_end)) {
        _err = "Can't read this file";
        return false;
    }
    return true;
}

const char* TrackerConvertSource::error() const {
    if (_err) return _err;
    return _p ? _p->errorMessage() : "";
}

uint32_t TrackerConvertSource::rate() const { return PLAY_RATE; }

int TrackerConvertSource::read(int16_t* out, int max) {
    if (!_p) return READ_FAILED;
    if (_blockPos >= _blockFrames) {
        // Blocks split as update() splits them, so the audio is the same.
        if (_updateLeft == 0) _updateLeft = TrackerPlayer::MAX_FRAMES_PER_UPDATE;
        uint32_t n;
        _block = _p->mixOffline(_updateLeft, n);
        if (!n) return _p->state() == TrackerPlayer::STATE_ERROR ? READ_FAILED : READ_END;
        _updateLeft -= n;
        _blockFrames = n;
        _blockPos = 0;
    }
    uint32_t n = _blockFrames - _blockPos;
    if (n > (uint32_t)max) n = (uint32_t)max;
    memcpy(out, _block + _blockPos * 2, n * 4);
    _blockPos += n;
    return (int)n;
}

void TrackerConvertSource::progress(uint32_t& done, uint32_t& total) const {
    // Orders, and rows within one (256 at most).
    total = _p ? (uint32_t)_p->songLength() * 256 : 0;
    if (!_p || _p->state() != TrackerPlayer::STATE_PLAYING) {
        done = total;
        return;
    }
    int row = _p->currentRow();
    done = (uint32_t)_p->patternOrderPosition() * 256 + (uint32_t)(row > 255 ? 255 : row);
}

void TrackerConvertSource::close() {
    if (!_p) return;
    _p->stop();
    _p->~TrackerPlayer(); // in SharedRamArena, not the heap
    _p = nullptr;
    _block = nullptr;
}
