#include "loader.h"

#include "library.h"
#include "paths.h"

#include <algorithm>

namespace pf {

// --- cache ------------------------------------------------------------------------------------

TableCache& TableCache::get() {
    static TableCache c;
    return c;
}

std::shared_ptr<const Wavetable> TableCache::find(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = items_.find(key);
    if (it == items_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.table;
}

std::shared_ptr<const Wavetable> TableCache::put(const std::string& key, std::shared_ptr<const Wavetable> t) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = items_.find(key);
    if (it != items_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return it->second.table;
    }
    lru_.push_front(key);
    bytes_ += t->bytes();
    items_.emplace(key, Item{t, lru_.begin()});
    evict();
    return t;
}

void TableCache::evict() {
    auto it = lru_.end();
    while (bytes_ > cap_ && it != lru_.begin()) {
        --it;
        auto found = items_.find(*it);
        if (found->second.table.use_count() > 1) continue;   // someone plays it
        bytes_ -= found->second.table->bytes();
        items_.erase(found);
        it = lru_.erase(it);
    }
}

size_t TableCache::bytes() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return bytes_;
}

size_t TableCache::entries() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return items_.size();
}

void TableCache::setCap(size_t b) {
    std::lock_guard<std::mutex> lk(mtx_);
    cap_ = b;
    evict();
}

void TableCache::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    items_.clear();
    lru_.clear();
    bytes_ = 0;
}

std::shared_ptr<const Wavetable> loadTable(const std::string& key, std::string* err) {
    static const std::string kBuiltin = "builtin:";
    const bool builtin = key.compare(0, kBuiltin.size(), kBuiltin) == 0;
    int index = -1;
    if (builtin) {
        index = builtinIndex(key.substr(kBuiltin.size()));
        if (index < 0) {
            if (err) *err = "no such built-in";
            return nullptr;
        }
        if (index == 0) return std::shared_ptr<const Wavetable>(&classicBuiltin(), [](const Wavetable*) {});   // static
    }
    if (auto hit = TableCache::get().find(key)) return hit;
    if (builtin) {   // computed, then cached like a file (evicted when unused, rebuilt on demand)
        auto t = std::make_shared<Wavetable>();
        buildBuiltin(index, *t);
        return TableCache::get().put(key, std::move(t));
    }
    const std::string path = resolveKey(key, tableRoots());
    if (path.empty()) {
        if (err) *err = "unknown folder";
        return nullptr;
    }
    auto t = std::make_shared<Wavetable>();
    if (!loadWavetable(path, *t, err)) return nullptr;
    return TableCache::get().put(key, std::move(t));
}

Loader::SlotType tableSlotType() {
    Loader::SlotType t;
    t.fallbackKey = "builtin:Classic";
    t.load = [](const std::string& key, std::string* err, int* info) -> std::shared_ptr<const void> {
        auto table = loadTable(key, err);
        if (table && info) *info = table->frames;
        return table;
    };
    return t;
}

// --- loader -----------------------------------------------------------------------------------

Loader::Loader(std::vector<SlotType> types) : slots_(types.size()) {
    for (size_t i = 0; i < types.size(); ++i) {
        Slot& s = slots_[i];
        s.type = std::move(types[i]);
        s.want = s.type.fallbackKey;
        int info = 0;
        std::string err;
        // The fallbacks are built-ins (no I/O): ready before the first block.
        auto obj = s.type.fallbackKey.empty() ? nullptr : s.type.load(s.type.fallbackKey, &err, &info);
        s.owned = obj;
        s.info = info;
        s.loadedKey = s.type.fallbackKey;
        s.live.store(obj.get(), std::memory_order_release);
    }
    thread_ = std::thread([this] { run(); });
}

Loader::~Loader() { stop(); }

void Loader::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Loader::setListener(Listener l) {
    std::lock_guard<std::mutex> lk(mtx_);
    listener_ = std::move(l);
}

void Loader::want(int slot, const std::string& key, bool now) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Slot& s = slots_[static_cast<size_t>(slot)];
        const std::string k = key.empty() ? s.type.fallbackKey : key;
        // Already wanted: nothing to do. A failed key is retried only on an explicit pick
        // (now), never by a scroll passing over it.
        if (s.want == k && (!s.missing || !now)) return;
        s.want = k;
        s.wantAt = std::chrono::steady_clock::now();
        s.now = now;
        s.missing = false;
        kick_ = true;
    }
    cv_.notify_all();
}

std::string Loader::wanted(int slot) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return slots_[static_cast<size_t>(slot)].want;
}

Loader::View Loader::view(int slot) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Slot& s = slots_[static_cast<size_t>(slot)];
    View v;
    v.key = s.want;
    v.loadedKey = s.loadedKey;
    v.info = s.info;
    v.error = s.error;
    v.state = s.missing ? Missing : (s.want != s.loadedKey ? Loading : Ready);
    v.ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.doneAt).count();
    return v;
}

bool Loader::busy() const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const Slot& s : slots_)
        if (!s.missing && s.want != s.loadedKey) return true;
    return false;
}

void Loader::publish(Slot& s, std::shared_ptr<const void> obj, const std::string& loadedKey) {
    graveyard_.push_back({std::move(s.owned), 0});
    s.owned = std::move(obj);
    s.loadedKey = loadedKey;
    s.live.store(s.owned.get(), std::memory_order_seq_cst);
    graveyard_.back().epoch = epoch_.load(std::memory_order_seq_cst);   // read AFTER the swap
}

void Loader::run() {
    std::unique_lock<std::mutex> lk(mtx_);
    while (!quit_) {
        // Nothing may leave this thread (std::terminate would end MPC): an out-of-memory
        // here is a round skipped; the next one, 20 ms on, tries again.
        try {
            cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return quit_ || kick_; });
            kick_ = false;
            if (quit_) break;

            // Free what no block can still be using: a block ended since the swap, or none is
            // running now (the next one reads the new pointers; read after every swap below).
            const bool idle = !inBlock_.load(std::memory_order_seq_cst);
            const uint32_t e = epoch_.load(std::memory_order_seq_cst);
            std::vector<std::shared_ptr<const void>> dead;
            graveyard_.erase(std::remove_if(graveyard_.begin(), graveyard_.end(),
                                            [&](Grave& g) {
                                                if (!idle && static_cast<int32_t>(e - g.epoch) <= 0) return false;
                                                dead.push_back(std::move(g.obj));
                                                return true;
                                            }),
                             graveyard_.end());

            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < slots_.size(); ++i) {
                Slot& s = slots_[i];
                if (s.want == s.loadedKey || s.missing) continue;
                if (!s.now && now - s.wantAt < std::chrono::milliseconds(kDebounceMs)) continue;
                const std::string key = s.want;
                const LoadFn load = s.type.load;
                const std::string fallback = s.type.fallbackKey;
                lk.unlock();
                dead.clear();   // free outside the lock too
                std::string err;
                int info = 0;
                std::shared_ptr<const void> obj;
                try {   // an exception here would end MPC (std::terminate): a throw is a failed load
                    obj = load(key, &err, &info);
                } catch (...) {
                    obj = nullptr;
                    err = "could not load";
                }
                bool ok = obj != nullptr;
                if (!ok && !fallback.empty()) {
                    std::string ignored;
                    try {
                        obj = load(fallback, &ignored, &info);
                    } catch (...) {
                        obj = nullptr;
                    }
                }
                lk.lock();
                if (s.want != key) continue;   // picked something else meanwhile: next round loads that
                publish(s, std::move(obj), ok ? key : fallback);
                s.missing = !ok;
                s.error = ok ? "" : err;
                s.info = info;
                s.doneAt = std::chrono::steady_clock::now();
                loads_.fetch_add(1);
                Listener l = listener_;
                lk.unlock();
                try {
                    if (l) l(static_cast<int>(i), key, ok);
                } catch (...) {
                }
                lk.lock();
            }
            lk.unlock();
            dead.clear();
            lk.lock();
        } catch (...) {
            if (!lk.owns_lock()) lk.lock();
        }
    }
    graveyard_.clear();   // after the audio thread is gone (effClose joins us first)
}

} // namespace pf
