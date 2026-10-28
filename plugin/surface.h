#pragma once
// Surface: what every plugin parameter does when MPC sets it, what it reads back, and what its value
// text says. EffectWerk's plugin/surface.{h,cpp} (SubWerk's, PolyWerk's, RackWerk's, device-proven)
// by way of MasterWerk's, without its chain and scenes and without LEARN.
//
// Threads:
//   UI thread      get / set / display (MPC polls these freely; they read caches)
//   UI thread      refresh(): recompute the plugin-owned values (stepper, tiles) and texts;
//                  called after every set
//   audio thread   notify(): tell MPC what changed underneath it (audioMasterAutomate for
//                  values, audioMasterUpdateDisplay for text) and snapshot() the sound values.
//                  No allocation, no locks.
//
// MPC sends values rounded to 1/1000 as "what it last read back + a delta": a Q-Link detent
// is 1/128 of the range, a data-wheel click 0.01, a touch drag about 0.04, and a tile tap
// sends a release echo ~0.7 s later. So every stepped parameter (choices, the preset stepper)
// moves exactly one step per event in MPC's direction, and the plugin pushes the snapped value back
// (stepIndex / stepItem below, RackWerk's rules). A button tap toggles its read-back, and a button
// always reads back 0 (it springs back): every 1 is a press, and no release ever follows.
//
// The Moves (R35). The Move parameter holds the selected Move and stays on it when the Move is over. A change of it, a tap
// on one of the 16 Move tiles and GO are Moves to fire (or stop), and a preset or project load ends the one that plays: all
// of them happen here, on the UI thread, and Moves::fire(), stop() and reset() belong to the audio thread, so they are
// handed over (takeMoveCall) and they coalesce: the latest fire, GO or stop is the call, since it is the state the Moves are
// to be in, and a load is a flag that is never lost and comes first (a fire or stop before it is ended by it; one after it
// follows it). GO carries the Move that was selected when it was pressed. A
// tile is tapped by flipping what MPC read back (the plugin lights the tiles of the Moves that play or wait for their line,
// from the audio thread), and what follows a tap within a second on the same tile is MPC's release echo, not another.
#include "library.h"
#include "move_call.h"
#include "param_ids.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace gf {

class Surface {
public:
    using AutomateFn = void (*)(void* ctx, int index, float value);
    using UpdateFn   = void (*)(void* ctx);

    Surface();

    // --- UI thread ---------------------------------------------------------------
    float       get(int i) const;
    void        set(int i, float n);
    std::string display(int i) const;
    bool        automatable(int i) const;

    // State load: values as saved (no stepping), pushed to MPC by the next blocks.
    void        setValue(int i, float n);
    std::string presetKey() const;         // the preset this sound came from ("" = none)
    void        setPresetKey(const std::string& key);
    void        refresh();

    // A preset or a project was loaded (state.cpp, once its batch is complete): the Move that plays ends, and no
    // Move is fired by the Move parameter the load set. UI thread.
    void        noteLoad() { postMove(MoveCall::kLoad, 0); }

    // Many values changed as one (a preset, a project): the audio thread keeps the previous sound
    // until the batch is complete. Nests; UI thread.
    void        beginBatch();
    void        endBatch();
    struct Batch {
        explicit Batch(Surface& s) : s_(s) { s_.beginBatch(); }
        ~Batch() { s_.endBatch(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Surface& s_;
    };

    // --- audio thread ------------------------------------------------------------
    // frames: the block just rendered. Texts that changed are told at most once per kTextGap frames.
    void        notify(AutomateFn automate, UpdateFn update, void* ctx, int frames);
    // Every parameter's current 0..1 value. False (and `out` untouched) while a batch is
    // being written: keep using the previous snapshot.
    bool        snapshot(float* out) const;
    // Moves on every value write: unchanged since a snapshot, the snapshot is still current.
    uint32_t    writes() const { return changes_.load(std::memory_order_acquire); }
    // The texts changed underneath (the status line, from the audio thread): MPC redraws them.
    void        textsChanged() { textGen_.fetch_add(1, std::memory_order_release); }

    // The next Move call from the UI thread, a load first, then the latest of the others; false when there is none.
    // Once per call, before the block.
    bool        takeMoveCall(MoveCall& out);
    // Every call waiting is forgotten (the plugin was switched off for a while, and comes back clean).
    void        dropMoveCalls();
    // The Moves that play or wait for their line (bit k: Move k): their tiles are lit, the others dark. Every block.
    void        moveLit(uint32_t mask);

    void        seed(uint32_t s) { rng_ = s ? s : 1u; }   // RND's random numbers

    static int  kFine;   // ranges with this many steps or more follow MPC's value
    // At most one audioMasterUpdateDisplay per 50 ms, as sd88me's wrapper since 2026-10-21 (their #487):
    // MPC rebuilds its parameter screen on each one, and a Q-Link turned on track edit -> automation
    // meanwhile loses its value. A change inside the gap waits for its end; it is never dropped.
    static constexpr int kTextGap = 2205;
    // Milliseconds for telling gestures apart (null: the steady clock). Tests set one that only
    // moves when they say, so stepping doesn't depend on how fast the machine is.
    static long long (*clock)();

private:
    struct Category {
        std::string name;
        std::vector<std::string> keys;
    };
    void apply(int i, float n);
    void postMove(int kind, int move);          // UI thread: a Move call for the audio thread
    void tapMoveTile(int param, int move, bool on);   // MPC set a Move's tile to `on`
    int  stepIndex(int i, float n, int count, int cur);
    int  stepItem(float n, int normRange, int items, int cur);   // one item per event
    bool toggleBounce(int i, bool on);
    void browserAction(int i);
    // FAVORITES, RECENT, then the library's categories (from L, the listing in use).
    std::vector<Category> categories(const Listing& L) const;
    void loadPreset(const std::string& key);
    void savePreset();
    int  stepperCur(int i, const Listing& L, const std::string& key) const;   // where a stepper stands

    // UI-thread-only stepping state (RackWerk's).
    long long lastSentMs_[P_COUNT] = {};
    float     lastN_[P_COUNT] = {};
    long long toggleMs_[P_COUNT] = {};
    bool      toggleOn_[P_COUNT] = {};

    // What each parameter should read vs what MPC last saw.
    std::atomic<float>    want_[P_COUNT];
    std::atomic<float>    shown_[P_COUNT];
    std::atomic<bool>     release_[P_COUNT];
    std::atomic<uint32_t> textGen_{0};
    std::atomic<uint32_t> batchSeq_{0};    // odd while a batch is being written (a seqlock)
    std::atomic<uint32_t> changes_{0};     // bumped by every write to want_ / shown_ (notify's cue)
    std::atomic<int>      batchDepth_{0};

    // The Move calls from the UI thread to the audio thread, coalesced: the latest fire, GO or stop in one word (0: none; its
    // kind plus one and its Move), and a load as a flag. The UI thread writes, the audio thread takes (exchange).
    std::atomic<uint32_t> moveCall_{0};
    std::atomic<bool>     moveLoad_{false};

    // Browser state + text cache (refresh writes, the UI thread reads).
    mutable std::mutex       mtx_;
    std::vector<std::string> texts_;
    size_t                   textHash_ = 0;
    int                      brCat_ = 2;       // index into categories(): starts on the first factory category
    int                      catPage_ = 0, itemPage_ = 0;
    std::string              followed_;        // the preset key the browser last followed
    std::string              presetKey_;
    std::vector<std::string> tileKeys_;        // what each preset tile holds now
    std::vector<int>         catTiles_;        // which category each category tile holds
    std::vector<std::string> catTileNames_;    // and its name (a rescan may renumber the categories)
    uint32_t                 rng_ = 0x2545F491u;

    // Every write of a value MPC should see goes through here.
    void put(int i, float v) {
        want_[i].store(v, std::memory_order_relaxed);
        changes_.fetch_add(1, std::memory_order_release);
    }

    // Audio-thread state.
    uint32_t litMoves_ = 0;            // the Move tiles lit now (bit k: Move k)
    uint32_t scanned_ = 0xffffffffu;   // changes_ at the last complete notify pass
    bool     scanPending_ = true;
    uint32_t textSeen_ = 0;
    int      cursor_ = 0;
    int      sinceText_ = kTextGap;   // frames since the last UpdateDisplay (the first goes out at once)
};

} // namespace gf
