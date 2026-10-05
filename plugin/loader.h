#pragma once
// Background loading with a lock-free handoff to the audio thread (docs/M1_DESIGN.md §2-3).
//
// TableCache   process-wide: key -> shared table, least recently used evicted beyond a memory
//              cap, never one still in use. The same table on both oscillators or in two
//              plugin instances is loaded once.
// Loader       one per plugin instance, its own worker thread, a few slots (osc 1 table,
//              osc 2 table, tuning). Per slot:
//                UI thread   want(key)            what the user picked (debounced 150 ms, so
//                                                 scrolling through 50 tables loads one)
//                worker      loads, then publishes: live = new pointer, old one to the graveyard
//                audio       live(slot)           read once at block start, used until block end
//              The audio thread calls blockStart() before and blockDone() after every block;
//              the worker frees a graveyard entry once a block has ended after its swap, or
//              when no block is running (MPC may stop processing a track for a long time), so a
//              block never sees its table freed. No locks, frees or allocation on the audio thread.
//              A failed load publishes the slot's fallback (built-in Classic, 12-TET) but keeps
//              the wanted key, so a saved project keeps its reference.
#include "../dsp/wavetable.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pf {

class TableCache {
public:
    static TableCache& get();
    std::shared_ptr<const Wavetable> find(const std::string& key);
    // Inserts unless already there; returns the cached copy (a racing second load keeps the first).
    std::shared_ptr<const Wavetable> put(const std::string& key, std::shared_ptr<const Wavetable> t);
    size_t bytes() const;
    size_t entries() const;
    void setCap(size_t bytes);   // default 96 MB
    void clear();                // tests

private:
    void evict();   // with mtx_ held
    struct Item {
        std::shared_ptr<const Wavetable> table;
        std::list<std::string>::iterator lru;
    };
    mutable std::mutex mtx_;
    std::map<std::string, Item> items_;
    std::list<std::string> lru_;   // front = most recently used
    size_t bytes_ = 0;
    size_t cap_ = 96u << 20;
};

// A table by key: "builtin:Classic" (static, never freed), another built-in (cache, else
// computed) or a file key (cache, else import).
std::shared_ptr<const Wavetable> loadTable(const std::string& key, std::string* err = nullptr);

class Loader {
public:
    // Loads `key` (worker thread). `info` gets something to show (frames, notes); null = failed.
    using LoadFn = std::function<std::shared_ptr<const void>(const std::string& key, std::string* err, int* info)>;
    struct SlotType {
        LoadFn load;
        std::string fallbackKey;
    };
    enum State : int { Ready, Loading, Missing };
    struct View {
        std::string key;         // wanted
        std::string loadedKey;   // what live() holds (the fallback's key when missing)
        State state = Ready;
        int info = 0;
        std::string error;
        long long ageMs = 0;     // since the last publish (how long MISSING has been showing)
    };
    // slot, key: called on the worker after every publish (not under any lock).
    using Listener = std::function<void(int slot, const std::string& key, bool ok)>;

    explicit Loader(std::vector<SlotType> types);
    ~Loader();
    void stop();   // joins the worker; the listener is never called after this returns
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    void setListener(Listener l);   // before the first want()
    void want(int slot, const std::string& key, bool now);   // now: skip the debounce
    std::string wanted(int slot) const;
    View view(int slot) const;
    bool busy() const;              // some slot isn't at its wanted key yet
    int loads() const { return loads_.load(); }   // completed loads (tests)

    const void* live(int slot) const { return slots_[static_cast<size_t>(slot)].live.load(std::memory_order_seq_cst); }
    void blockStart() { inBlock_.store(true, std::memory_order_seq_cst); }
    void blockDone() {
        epoch_.fetch_add(1, std::memory_order_seq_cst);
        inBlock_.store(false, std::memory_order_seq_cst);
    }

    static constexpr int kDebounceMs = 150;

private:
    struct Slot {
        SlotType type;
        std::string want, loadedKey, error;
        std::chrono::steady_clock::time_point wantAt{}, doneAt{};
        bool now = false, missing = false;
        int info = 0;
        std::shared_ptr<const void> owned;
        std::atomic<const void*> live{nullptr};
    };
    struct Grave {
        std::shared_ptr<const void> obj;
        uint32_t epoch;
    };
    void run();
    void publish(Slot& s, std::shared_ptr<const void> obj, const std::string& loadedKey);   // mtx_ held

    std::vector<Slot> slots_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool quit_ = false, kick_ = false;
    std::vector<Grave> graveyard_;
    std::atomic<uint32_t> epoch_{0};
    std::atomic<bool> inBlock_{false};
    std::atomic<int> loads_{0};
    Listener listener_;
    std::thread thread_;
};

// The table slot type (fallback: built-in Classic). `info` = frame count.
Loader::SlotType tableSlotType();

} // namespace pf
