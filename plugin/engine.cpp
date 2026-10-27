#include "engine.h"

#include "groove_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gf {

Engine::Engine() {
    for (int i = 0; i < P_COUNT; ++i) norm_[i] = PARAM_INFO[i].def;
    params_ = grooveParams(norm_);   // the first snapshot from MPC is taken as it is: the chain starts fresh and jumps to it
    std::memcpy(used_, norm_, sizeof used_);
    haveUsed_ = true;
}

void Engine::setParams(const float* norm) {
    std::memcpy(norm_, norm, sizeof norm_);
    haveUsed_ = false;   // the next chunk works the chain's parameters out again
}

void Engine::reset() {
    groove_.reset();
    moves_.reset();
    fresh_ = true;
    echoMoved_ = false;
    echoHold_ = 0;
    haveUsed_ = false;   // the first chunk after a reset works the knobs out at once, not the Move's last step that was held
    sinceConvert_ = kConvertGap;
}

int Engine::pickOf(int id) const {
    return std::clamp(static_cast<int>(paramValue(id, norm_[id])), 0, PARAM_INFO[id].nopts - 1);
}

void Engine::fire(int move) {
    if (move < 0 || move >= Moves::count()) return;
    moves_.fire(move, pickOf(P_MOVE_MODE), pickOf(P_MOVE_QUANT));
}

void Engine::moveCall(const MoveCall& call) {
    switch (call.kind) {
        case MoveCall::kFire:
        case MoveCall::kGo: fire(call.move); break;   // (GO carries the Move it was pressed for)
        case MoveCall::kStop: moves_.stop(); break;
        case MoveCall::kLoad:   // the new sound is the sound: the Move ends, and nothing it had on is kept
            moves_.stop();
            echoMoved_ = false;
            echoHold_ = 0;
            break;
        default: break;
    }
}

uint32_t Engine::litMoves() const {
    uint32_t mask = 0;
    const int playing = moves_.playing(), waiting = moves_.armed();
    if (playing >= 0 && playing < 32) mask |= 1u << playing;
    if (waiting >= 0 && waiting < 32) mask |= 1u << waiting;
    return mask;
}

// The grid for the chunk that starts `pos` samples into the block and is `len` long. MPC says where the block starts; while it
// plays, the chunk starts `pos` samples on. Otherwise the clock runs on, from MPC's held position (if it says one) at the
// first chunk after a reset(). The Moves, Time and the other parts all get this one, as a position that is always known and
// always followed: a part that woke up while MPC was stopped takes the clock's place, not a place of its own.
ef::Transport Engine::gridFor(const ef::Transport& t, int pos, int len) {
    const bool known = t.valid && std::isfinite(t.beats);
    const double bpm = ef::tempoOf(t);   // the parts' rule for a tempo (the position inside a block too, not the raw one)
    if (t.playing && known) clock_ = t.beats + pos * (bpm / 60.0 / ef::kRate);
    else if (fresh_ && known) clock_ = t.beats;
    fresh_ = false;
    ef::Transport c = t;
    c.beats = clock_;
    c.playing = c.valid = true;
    clock_ += len * (bpm / 60.0 / ef::kRate);
    return c;
}

// R38: the Echo a Move switched on (the knobs have it off) stays on after the Move let go of it, for as long as the chain's
// tail says it rings, and then it is the knobs' again: it fades off as a switch does.
// True if it wrote into norm (the Echo held on).
bool Engine::keepEcho(float* norm, int len) {
    if (norm_[P_ECHO_ON] > 0.5f) {   // the knobs have it on: nothing to keep
        echoMoved_ = false;
        echoHold_ = 0;
        return false;
    }
    if (norm[P_ECHO_ON] > 0.5f) {    // a Move has it on
        echoMoved_ = true;
        echoHold_ = 0;
        return false;
    }
    if (echoMoved_) {                // it just let go
        echoMoved_ = false;
        echoHold_ = std::min(groove_.tailSamples(), kMaxHoldSamples);
    }
    if (echoHold_ > 0) {
        norm[P_ECHO_ON] = 1.0f;
        echoHold_ = std::max(echoHold_ - len, 0);
        return true;
    }
    return false;
}

void Engine::render(float* L, float* R, int n, const ef::Transport& t) {
    for (int pos = 0; pos < n; pos += ef::kChunk) {
        const int len = std::min(ef::kChunk, n - pos);
        const ef::Transport c = gridFor(t, pos, len);

        // The snapshot with the Move that plays laid on it, and the Time pattern it asks for.
        float norm[P_COUNT];
        std::memcpy(norm, norm_, sizeof norm);
        int pattern = -1;
        double patternStart = 0.0;
        bool loop = false;
        bool laid = moves_.apply(norm, c, &pattern, &patternStart, &loop, len);
        laid = keepEcho(norm, len) || laid;
        // The chain's parameters follow the snapshot with the Move on it (R53): a new snapshot at once, and what a Move's ramps
        // change at most once in 128 frames, a block. A conversion costs about 8000 ARM instructions, so four in a block for a
        // ramp that moves in every chunk cost 0.8% of it; every part glides a change over a millisecond or more, MPC's own
        // automation arrives a block at a time, and a ramp that is 2.9 ms late is not heard. A Time pattern is not a parameter:
        // it goes in every chunk, to the sample.
        //
        // R60a: the compare (916 bytes, 5.0k ARM instructions a block) has something to find only if `norm` is not the snapshot
        // (a Move laid something on it, or the Echo is held on) or `used_` is not (the last conversion was made from one that was
        // laid on: the Move has let go, or the gate held its last step). Otherwise both are the snapshot, and an idle chunk
        // compares nothing. A Move that plays is not the test: a Move's switched-on parts are laid on `norm` with no ramp, and
        // the release of the hold or the last step of a ramp leaves `used_` laid on after nothing is laid on `norm`.
        bool convert = !haveUsed_;
        if (!convert && sinceConvert_ >= kConvertGap && (laid || usedLaid_ || compareAlways_)) {
            ++compares_;
            convert = std::memcmp(norm, used_, sizeof norm) != 0;
        }
        if (convert) {
            params_ = grooveParams(norm);
            ++conversions_;
            std::memcpy(used_, norm, sizeof used_);
            haveUsed_ = true;
            usedLaid_ = laid;
            sinceConvert_ = 0;
        }
        sinceConvert_ = std::min(sinceConvert_ + len, kConvertGap);
        params_.time.pattern = pattern;
        params_.time.patternStart = patternStart;
        params_.time.loop = loop;

        groove_.set(params_, c);
        groove_.process(L + pos, R + pos, len);
    }
}

} // namespace gf
