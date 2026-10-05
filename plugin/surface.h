#pragma once
// Surface: what every plugin parameter does when MPC sets it, what it reads back, and what
// its value text says. Ported from RackForcePlugin's plugin/surface.{h,cpp} (device-proven).
//
// Threads:
//   UI thread      get / set / display (MPC polls these freely; they read caches)
//   any thread     refresh(): recompute the plugin-owned values (steppers, tiles) and texts;
//                  called after every set and by the loader's worker after every load
//   audio thread   notify(): tell MPC what changed underneath it (audioMasterAutomate for
//                  values, audioMasterUpdateDisplay for text) and snapshot() the sound values.
//                  No allocation, no locks.
//
// MPC sends values rounded to 1/1000 as "what it last read back + a delta": a Q-Link detent
// is 1/128 of the range, a data-wheel click 0.01, a touch drag about 0.04, and a tile tap
// sends a release echo ~0.7 s later. So every stepped parameter (choices, small whole
// numbers, list steppers) moves exactly one step per event in MPC's direction, and the
// plugin pushes the snapped value back (stepIndex / stepItem below, RackForce's rules). A
// button tap toggles the read-back (always 0), so it is a 1 every time, never a release.
#include "library.h"
#include "loader.h"
#include "param_ids.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pf {

class Surface {
public:
    using AutomateFn = void (*)(void* ctx, int index, float value);
    using UpdateFn   = void (*)(void* ctx);

    explicit Surface(Loader& loader);   // loader slots 0, 1 = oscillator tables, 2 = tuning

    // --- UI thread ---------------------------------------------------------------
    float       get(int i) const;
    void        set(int i, float n);
    std::string display(int i) const;
    bool        automatable(int i) const;

    // State load: values as saved (no stepping), pushed to MPC by the next blocks.
    void        setValue(int i, float n);
    void        setTable(int osc, const std::string& key, bool now);
    std::string tableKey(int osc) const;   // what the oscillator wants (saved in the state)
    void        setTuning(const std::string& key, bool now);
    std::string tuningKey() const;
    std::string presetKey() const;         // the preset this sound came from ("" = none)
    void        setPresetKey(const std::string& key);

    // --- any thread --------------------------------------------------------------
    void        refresh();
    std::string busyText() const;          // "LOADING ..." / "MISSING ..." for the status line, or ""

    // Many values changed as one (a preset, a project, randomize): the audio thread keeps the
    // previous sound until the batch is complete. Nests; UI thread.
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
    void        notify(AutomateFn automate, UpdateFn update, void* ctx);
    // Every parameter's current 0..1 value. False (and `out` untouched) while a batch is
    // being written: keep using the previous snapshot.
    bool        snapshot(float* out) const;
    // Moves on every value write: unchanged since a snapshot, the snapshot is still current.
    uint32_t    writes() const { return changes_.load(std::memory_order_acquire); }
    // True once when a MISSING note in the status line has run its time (no lock, no allocation).
    bool        statusExpired();

    static int  kFine;   // ranges with this many steps or more follow MPC's value

private:
    struct Category {
        std::string name;
        std::vector<std::string> keys;
    };
    void apply(int i, float n);
    int  stepIndex(int i, float n, int count, int cur);
    int  stepItem(float n, int normRange, int items, int cur);   // one item per event
    bool toggleBounce(int i, bool on);
    void stepTable(int osc, int delta);
    static std::string stepKey(FileLibrary& lib, const std::string& cur, int delta);
    void browserAction(int i);
    // FAVORITES, RECENT, then the library's categories (from L, the listing in use).
    std::vector<Category> categories(FileLibrary& lib, const Listing& L) const;
    int  browseTarget() const;                  // 0, 1: an oscillator's table; 2: presets
    static FileLibrary& browseLibrary(int target);
    std::string browseKey(int target) const;    // with mtx_ held
    void loadPreset(const std::string& key);
    void savePreset();
    void randomize(float amount);
    std::string tuningText() const;
    std::string frameText(int osc, float n) const;
    std::string amountText(int i) const;
    void autoAssignXy();
    std::string tableText(int osc) const;       // with mtx_ held
    int  stepperCur(int i, const Listing& L, const std::string& key) const;   // where a stepper stands

    Loader& loader_;

    // UI-thread-only stepping state (RackForce's).
    long long lastSentMs_[P_COUNT] = {};
    float     lastN_[P_COUNT] = {};
    long long toggleMs_[P_COUNT] = {};
    bool      toggleOn_[P_COUNT] = {};

    // What each parameter should read vs what MPC last saw.
    std::atomic<float>    want_[P_COUNT];
    std::atomic<float>    shown_[P_COUNT];
    std::atomic<bool>     release_[P_COUNT];
    std::atomic<uint32_t> textGen_{0};
    std::atomic<long long> statusUntil_{0};   // when the status line's MISSING note ends (ms), 0 = none
    std::atomic<uint32_t> batchSeq_{0};    // odd while a batch is being written (a seqlock)
    std::atomic<uint32_t> changes_{0};     // bumped by every write to want_ / shown_ (notify's cue)
    std::atomic<int>      batchDepth_{0};

    // Browser state + text cache (refresh writes, the UI thread reads).
    mutable std::mutex       mtx_;
    std::vector<std::string> texts_;
    size_t                   textHash_ = 0;
    int                      brCat_ = 2;       // index into categories(): starts on Built-in
    int                      catPage_ = 0, itemPage_ = 0;
    std::string              followed_;        // the target's key the browser last followed
    int                      browsed_ = 0;     // the browser target the state above is for
    std::string              presetKey_;
    std::vector<std::string> tileKeys_;        // what each table tile holds now
    std::vector<int>         catTiles_;        // which category each category tile holds
    uint32_t                 rng_ = 0x2545F491u;

    // Every write of a value MPC should see goes through here.
    void put(int i, float v) {
        want_[i].store(v, std::memory_order_relaxed);
        changes_.fetch_add(1, std::memory_order_release);
    }

    // Audio-thread state.
    uint32_t scanned_ = 0xffffffffu;   // changes_ at the last complete notify pass
    bool     scanPending_ = true;
    uint32_t textSeen_ = 0;
    int      cursor_ = 0;
    int      sinceText_ = 0;
};

} // namespace pf
