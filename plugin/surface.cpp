#include "surface.h"

#include "library.h"
#include "patch_map.h"
#include "presets.h"
#include "state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

namespace pf {

int Surface::kFine = 48;

namespace {

constexpr int kMaxAutomatePerBlock = 48;   // spread big refreshes over a few blocks
constexpr int kTextEveryBlocks     = 4;    // at most one UpdateDisplay per ~12 ms
constexpr float kQuant             = 0.0015f;   // MPC rounds values to 1/1000
constexpr long long kGestureMs     = 300;   // sends closer than this belong to one gesture
constexpr float kFirstMoveMax      = 0.16f; // a gesture's first event is a turn, not a jump
constexpr int kTableSlots          = 2;    // loader slots 0, 1
constexpr int kTuningSlot          = 2;    // loader slot 2
constexpr int kPresetTarget        = 2;    // br_target: OSC 1, OSC 2, PRESETS
constexpr long long kMissingShowMs = 5000;  // MISSING stays in the status line this long

// A stepper's 0..1 range: one item per 1/1023, or per 1/(items-1) for longer lists.
int stepperRange(int items) { return std::max(kStepperRange, items - 1); }

// Text of these follows another parameter: the position knob shows frames, width or colour
// by the wave mode; a matrix amount shows its target's unit.
bool drivesText(int i) {
    if (i == P_O1_WAVE || i == P_O2_WAVE) return true;
    return i + 1 < P_COUNT && PARAM_SPECS[i + 1].fmt == Fmt::ModAmt;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// NaN from the host becomes 0: kept, it would reach the engine's smoothers and never leave.
float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int stepsOf(int i) {   // whole steps of a stepped parameter, 0 = continuous
    const ParamSpec& s = PARAM_SPECS[i];
    if (s.curve == Curve::Enum) return PARAM_INFO[i].nopts - 1;
    if (s.curve == Curve::Int) return static_cast<int>(std::lround(s.hi - s.lo));
    return 0;
}

bool exactOption(float n, int count) {   // a tap on an option (allowing MPC's 1/1000 rounding)
    if (count < 1) return true;
    return std::fabs(n - std::round(n * count) / count) <= kQuant;
}

int popupFlagOf(int param) {
    for (int j = 0; j < P_COUNT; ++j)
        if (PARAM_INFO[j].popupOf == param) return j;
    return -1;
}

const int kCatTiles[] = {
#define T(n) P_CAT_##n
    T(1), T(2), T(3), T(4), T(5), T(6), T(7), T(8), T(9), T(10), T(11), T(12), T(13), T(14), T(15), T(16)
#undef T
};
const int kItemTiles[] = {
#define T(n) P_TBL_##n
    T(1), T(2), T(3), T(4), T(5), T(6), T(7), T(8), T(9), T(10), T(11), T(12),
    T(13), T(14), T(15), T(16), T(17), T(18), T(19), T(20), T(21), T(22), T(23), T(24)
#undef T
};
static_assert(sizeof kCatTiles / sizeof kCatTiles[0] == kBrowserCats, "category tiles");
static_assert(sizeof kItemTiles / sizeof kItemTiles[0] == kBrowserItems, "item tiles");

constexpr int kTableParam[kTableSlots] = {P_O1_TABLE, P_O2_TABLE};
constexpr int kTablePrev[kTableSlots] = {P_O1_TABLE_PREV, P_O2_TABLE_PREV};
constexpr int kTableNext[kTableSlots] = {P_O1_TABLE_NEXT, P_O2_TABLE_NEXT};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

Surface::Surface(Loader& loader) : loader_(loader), texts_(P_COUNT) {
    for (int i = 0; i < P_COUNT; ++i) {
        want_[i].store(PARAM_INFO[i].def);
        shown_[i].store(PARAM_INFO[i].def);
        release_[i].store(false);
        lastN_[i] = -1.0f;
    }
    refresh();
}

// --- UI thread ----------------------------------------------------------------------------

float Surface::get(int i) const { return i >= 0 && i < P_COUNT ? want_[i].load(std::memory_order_relaxed) : 0.0f; }

bool Surface::automatable(int i) const { return i >= 0 && i < P_COUNT && PARAM_INFO[i].kind == Kind::Synth; }

void Surface::set(int i, float n) {
    if (i < 0 || i >= P_COUNT) return;
    const Kind k = PARAM_INFO[i].kind;
    if (k == Kind::Readout) return;   // MPC sets param 0 right after loading: ignore
    n = clamp01(n);
    shown_[i].store(n, std::memory_order_relaxed);   // that is what MPC shows now
    changes_.fetch_add(1, std::memory_order_release);
    if (k == Kind::Button) {
        const bool down = n > 0.5f;
        const bool rising = down && !held_[i];
        held_[i] = down;
        if (rising) {
            release_[i] = true;
            changes_.fetch_add(1, std::memory_order_release);   // after the flag: notify must see it
            apply(i, n);
            refresh();
        }
        return;
    }
    apply(i, n);
    if (k != Kind::Synth) refresh();   // a sound parameter's text is computed when MPC asks
    else if (drivesText(i)) textGen_.fetch_add(1, std::memory_order_release);   // MPC must re-read the dependent text
}

void Surface::beginBatch() {
    if (batchDepth_.fetch_add(1) == 0) {
        batchSeq_.fetch_add(1, std::memory_order_relaxed);   // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
    }
}

void Surface::endBatch() {
    if (batchDepth_.fetch_sub(1) == 1) {
        batchSeq_.fetch_add(1, std::memory_order_release);   // even: done
        // Many values at once (a preset, randomize, auto-assign): texts that depend on others
        // (frame / width, amounts in a target's unit) may have changed with no text of their
        // own changing, and MPC only re-reads texts when told.
        textGen_.fetch_add(1, std::memory_order_release);
    }
}

int Surface::stepperCur(int i, const Listing& L, const std::string& key) const {
    const int items = static_cast<int>(L.items.size());
    const int at = L.find(key);
    if (at >= 0) return at;
    // Not listed (a missing file, a randomized sound): where the stepper stands now, so a turn
    // moves from there instead of jumping to the first item.
    return clampi(static_cast<int>(std::lround(want_[i].load() * stepperRange(items))), 0, std::max(items - 1, 0));
}

void Surface::apply(int i, float n) {
    const ParamInfo& info = PARAM_INFO[i];
    switch (info.kind) {
        case Kind::Synth:
        case Kind::Ui: {
            const int steps = stepsOf(i);
            if (steps > 0 && steps < kFine) {
                const int cur = static_cast<int>(std::lround(want_[i].load() * steps));
                const int pick = stepIndex(i, n, steps, cur);
                put(i, static_cast<float>(pick) / static_cast<float>(steps));
                if (exactOption(n, steps)) {   // a tap on a list row closes its popup (a Q-Link nudge doesn't)
                    const int flag = popupFlagOf(i);
                    if (flag >= 0) put(flag, 0.0f);
                }
                if (info.kind == Kind::Ui && pick != cur)   // another page: an open list belongs to the old one
                    for (int j = 0; j < P_COUNT; ++j)
                        if (PARAM_INFO[j].kind == Kind::Popup) put(j, 0.0f);
                if (i == P_BR_TARGET && exactOption(n, steps))   // the browser opened: files may have come or gone
                    browseLibrary(pick).rescan();
            } else {
                put(i, n);
            }
            break;
        }
        case Kind::Popup: put(i, n > 0.5f ? 1.0f : 0.0f); break;
        case Kind::Stepper: {
            for (int o = 0; o < kTableSlots; ++o)
                if (i == kTableParam[o]) {
                    const auto L = tableLibrary().listing();
                    const int items = static_cast<int>(L->items.size());
                    const int cur = stepperCur(i, *L, loader_.wanted(o));
                    const int pick = stepItem(i, n, stepperRange(items), items, cur);
                    if (pick != cur && pick < items) loader_.want(o, L->items[static_cast<size_t>(pick)].key, false);
                }
            if (i == P_TUNING) {
                const auto L = tuningLibrary().listing();
                const int items = static_cast<int>(L->items.size());
                const int cur = stepperCur(i, *L, loader_.wanted(kTuningSlot));
                const int pick = stepItem(i, n, stepperRange(items), items, cur);
                if (pick != cur && pick < items) loader_.want(kTuningSlot, L->items[static_cast<size_t>(pick)].key, false);
            }
            if (i == P_PRESET) {
                const auto L = presetLibrary().listing();
                const int items = static_cast<int>(L->items.size());
                const int cur = stepperCur(i, *L, presetKey());
                const int pick = stepItem(i, n, stepperRange(items), items, cur);
                if (pick != cur && pick < items) loadPreset(L->items[static_cast<size_t>(pick)].key);
            }
            break;
        }
        case Kind::Button:
            for (int o = 0; o < kTableSlots; ++o) {
                if (i == kTablePrev[o]) stepTable(o, -1);
                if (i == kTableNext[o]) stepTable(o, +1);
            }
            if (i == P_TUNING_PREV || i == P_TUNING_NEXT) {
                const std::string k = stepKey(tuningLibrary(), loader_.wanted(kTuningSlot), i == P_TUNING_NEXT ? 1 : -1);
                if (!k.empty()) loader_.want(kTuningSlot, k, false);
            }
            if (i == P_PRESET_PREV || i == P_PRESET_NEXT) {
                const std::string k = stepKey(presetLibrary(), presetKey(), i == P_PRESET_NEXT ? 1 : -1);
                if (!k.empty()) loadPreset(k);
            }
            if (i == P_PRE_INIT) loadPreset("builtin:Init");
            if (i == P_PRE_SAVE) savePreset();
            if (i == P_PRE_RAND) randomize(paramValue(P_RAND_AMT, want_[P_RAND_AMT].load()));
            if (i == P_XY_AUTO) autoAssignXy();
            browserAction(i);
            break;
        case Kind::Tile:
        case Kind::Toggle: {
            const bool on = n > 0.5f;
            const bool lit = want_[i].load() > 0.5f;
            if (on == lit || toggleBounce(i, on)) break;   // nothing new, or the release echo
            browserAction(i);
            break;
        }
        case Kind::Readout: break;
    }
}

int Surface::stepIndex(int i, float n, int count, int cur) {
    if (count < 1) return 0;
    cur = clampi(cur, 0, count);
    const long long now = nowMs();
    const bool gesture = lastSentMs_[i] > 0 && now - lastSentMs_[i] < kGestureMs && lastN_[i] >= 0.0f;
    const float mpcPrev = lastN_[i];   // MPC's own previous value (never our pushes)
    lastSentMs_[i] = now;
    lastN_[i] = n;
    const float ours = static_cast<float>(cur) / count;
    const float r = std::round(n * count);

    if (count >= kFine) {
        // Follow MPC's value; a gesture that starts far from ours means MPC's idea of the
        // value was stale (a bump, not a jump: move at most kFirstMoveMax of the range).
        if (!gesture && std::fabs(n - ours) > kFirstMoveMax)
            return clampi(static_cast<int>(std::lround((ours + (n > ours ? kFirstMoveMax : -kFirstMoveMax)) * count)),
                          0, count);
        // A single slow detent (1/128) can be under half a step: it still moves one.
        if (!gesture && static_cast<int>(r) == cur && std::fabs(n - ours) > kQuant)
            return clampi(cur + (n > ours ? 1 : -1), 0, count);
        return clampi(static_cast<int>(r), 0, count);
    }
    // Coarse: a value that lands on a step is a tap (or our own value back); a toggle's
    // exact 0/1 is always a tap.
    const bool onStep = std::fabs(n - r / count) <= kQuant;
    if (onStep && (count == 1 || !gesture)) return clampi(static_cast<int>(r), 0, count);
    float delta = n - (gesture ? mpcPrev : ours);
    if (std::fabs(delta) <= kQuant) return cur;
    if (!gesture && std::fabs(delta) > kFirstMoveMax)   // MPC's idea of the value was stale
        delta = delta > 0 ? kFirstMoveMax : -kFirstMoveMax;
    const float steps = delta * count;
    int move = static_cast<int>(std::lround(steps));
    if (move == 0) move = steps > 0 ? 1 : -1;   // every detent moves at least one step
    return clampi(cur + move, 0, count);
}

int Surface::stepItem(int i, float n, int normRange, int items, int cur) {
    if (items < 1 || normRange < 1) return 0;
    cur = clampi(cur, 0, items - 1);
    const long long now = nowMs();
    const bool gesture = lastSentMs_[i] > 0 && now - lastSentMs_[i] < kGestureMs && lastN_[i] >= 0.0f;
    const float mpcPrev = lastN_[i];
    lastSentMs_[i] = now;
    lastN_[i] = n;
    const float ours = static_cast<float>(cur) / normRange;
    if (!gesture && std::fabs(n - ours) <= kQuant) return cur;          // our own value back
    const float delta = n - (gesture ? mpcPrev : ours);
    if (std::fabs(delta) <= kQuant) return cur;
    // One item per event, whatever the size of MPC's step (Q-Link detent 1/128, wheel click
    // 0.01, a drag ~0.04, a fast spin 1-3 detents): a long list must never jump.
    return clampi(cur + (delta > 0 ? 1 : -1), 0, items - 1);
}

bool Surface::toggleBounce(int i, bool on) {
    const long long now = nowMs();
    if (toggleMs_[i] > 0 && now - toggleMs_[i] < 1000 && on != toggleOn_[i]) return true;   // the release echo
    toggleMs_[i] = now;
    toggleOn_[i] = on;
    return false;
}

void Surface::stepTable(int osc, int delta) {
    const std::string k = stepKey(tableLibrary(), loader_.wanted(osc), delta);
    if (!k.empty()) loader_.want(osc, k, false);
}

// The next (delta +1) or previous item of a library's flat list; the first item if `cur` isn't listed.
std::string Surface::stepKey(FileLibrary& lib, const std::string& cur, int delta) {
    const auto L = lib.listing();
    const int items = static_cast<int>(L->items.size());
    if (items == 0) return {};
    const int at = L->find(cur);
    const int pick = at < 0 ? 0 : clampi(at + delta, 0, items - 1);
    return L->items[static_cast<size_t>(pick)].key;
}

std::vector<Surface::Category> Surface::categories(FileLibrary& lib, const Listing& L) const {
    std::vector<Category> out;
    std::vector<std::string> fav, rec;
    for (const std::string& k : lib.favorites()) if (L.find(k) >= 0) fav.push_back(k);
    for (const std::string& k : lib.recent()) if (L.find(k) >= 0) rec.push_back(k);
    out.push_back({"FAVORITES", std::move(fav)});
    out.push_back({"RECENT", std::move(rec)});
    for (size_t c = 0; c < L.categories.size(); ++c) {
        Category cat{L.categories[c], {}};
        for (int m : L.members[c]) cat.keys.push_back(L.items[static_cast<size_t>(m)].key);
        out.push_back(std::move(cat));
    }
    return out;
}

int Surface::browseTarget() const {
    return clampi(static_cast<int>(std::lround(want_[P_BR_TARGET].load() * 2.0f)), 0, kPresetTarget);
}

FileLibrary& Surface::browseLibrary(int target) { return target == kPresetTarget ? presetLibrary() : tableLibrary(); }

std::string Surface::browseKey(int target) const {
    return target == kPresetTarget ? presetKey_ : loader_.wanted(target);   // presetKey_: with mtx_ held
}

void Surface::browserAction(int i) {
    std::string load;   // a preset to load: done after the lock (loading refreshes the surface)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const int target = browseTarget();
        FileLibrary& lib = browseLibrary(target);
        const auto L = lib.listing();
        const std::vector<Category> cats = categories(lib, *L);
        const int ncat = static_cast<int>(cats.size());
        const std::string cur = browseKey(target);
        for (int t = 0; t < kBrowserCats; ++t)
            if (i == kCatTiles[t] && t < static_cast<int>(catTiles_.size()) && catTiles_[static_cast<size_t>(t)] >= 0) {
                brCat_ = catTiles_[static_cast<size_t>(t)];
                itemPage_ = 0;
                followed_ = cur;   // the next refresh must not jump back to its category
            }
        for (int t = 0; t < kBrowserItems; ++t)
            if (i == kItemTiles[t] && t < static_cast<int>(tileKeys_.size()) && !tileKeys_[static_cast<size_t>(t)].empty()) {
                const std::string key = tileKeys_[static_cast<size_t>(t)];
                if (target == kPresetTarget) load = key;
                else loader_.want(target, key, false);
                followed_ = key;   // picked here: stay on this category and page
            }
        const int catPages = std::max(1, (ncat + kBrowserCats - 1) / kBrowserCats);
        if (i == P_CAT_PREV) catPage_ = clampi(catPage_ - 1, 0, catPages - 1);
        if (i == P_CAT_NEXT) catPage_ = clampi(catPage_ + 1, 0, catPages - 1);
        const int nitems = brCat_ < ncat ? static_cast<int>(cats[static_cast<size_t>(brCat_)].keys.size()) : 0;
        const int itemPages = std::max(1, (nitems + kBrowserItems - 1) / kBrowserItems);
        if (i == P_TBL_PREV) itemPage_ = clampi(itemPage_ - 1, 0, itemPages - 1);
        if (i == P_TBL_NEXT) itemPage_ = clampi(itemPage_ + 1, 0, itemPages - 1);

        if (i == P_FAV && !cur.empty()) lib.setFavorite(cur, !lib.isFavorite(cur));
        if (i == P_RND && brCat_ < ncat) {   // a random item of this category, never the current one
            std::vector<std::string> pool;
            for (const std::string& k : cats[static_cast<size_t>(brCat_)].keys)
                if (k != cur) pool.push_back(k);
            if (!pool.empty()) {
                rng_ ^= rng_ << 13;
                rng_ ^= rng_ >> 17;
                rng_ ^= rng_ << 5;
                const std::string k = pool[rng_ % pool.size()];
                if (target == kPresetTarget) load = k;
                else loader_.want(target, k, true);
                followed_ = k;
            }
        }
        if (i == P_COPY) loader_.want(1, loader_.wanted(0), true);
        if (i == P_SWAP) {
            const std::string a = loader_.wanted(0), b = loader_.wanted(1);
            loader_.want(0, b, true);
            loader_.want(1, a, true);
        }
    }
    if (!load.empty()) loadPreset(load);
}

// --- presets ------------------------------------------------------------------------------

void Surface::loadPreset(const std::string& key) {
    std::string text;
    if (!presetText(key, text)) return;
    // The key first: the state's own refresh then sees the new preset, and a browser that
    // picked it (from FAVORITES, say) stays where it is instead of following the old one.
    const std::string old = presetKey();
    setPresetKey(key);
    if (!loadState(*this, text, true)) {
        setPresetKey(old);
        refresh();
        return;
    }
    presetLibrary().touchRecent(key);
    refresh();
}

void Surface::savePreset() {
    std::string key;
    const std::string path = nextUserPreset(&key);
    if (path.empty()) return;
    if (!writeFileAtomic(path, saveState(*this, true))) {
        std::remove(path.c_str());
        return;
    }
    presetLibrary().rescan();
    setPresetKey(key);
    refresh();
}

// Moves the sound toward a random one by `amount` (0..1), inside ranges that stay playable:
// envelopes, filters, oscillators, LFOs and the amounts of the matrix slots in use. Volume,
// voicing, glide, bend, the arp/sequencer and the matrix routing stay as they are.
void Surface::randomize(float amount) {
    Batch batch(*this);
    amount = clamp01(amount);
    auto rnd = [this] {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_ >> 8) / 16777216.0f;
    };
    auto ends = [](const std::string& k, const char* s) {
        const size_t n = std::char_traits<char>::length(s);
        return k.size() >= n && k.compare(k.size() - n, n, s) == 0;
    };
    const int stride = P_M2_SRC - P_M1_SRC;
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        const std::string k = PARAM_INFO[i].key;
        float lo = -1.0f, hi = -1.0f;   // the 0..1 range to draw from; lo < 0 = leave alone
        int pickLo = 0, pickFrom = 0;   // choices: draw an option index in [pickLo, pickFrom)
        const bool env = k.size() == 4 && k[0] == 'e' && k[2] == '_';
        const bool osc = k[0] == 'o' && (k[1] == '1' || k[1] == '2') && k[2] == '_';
        const bool flt = k[0] == 'f' && (k[1] == '1' || k[1] == '2') && k[2] == '_';
        const bool lfo = k[0] == 'l' && (k[1] == '1' || k[1] == '2') && k[2] == '_';
        if (env && ends(k, "_a")) { lo = 0.0f; hi = k[1] == '1' ? 0.4f : 0.6f; }
        else if (env && ends(k, "_d")) { lo = 0.15f; hi = 0.75f; }
        else if (env && ends(k, "_s")) { lo = 0.2f; hi = 1.0f; }
        else if (env && ends(k, "_r")) { lo = 0.15f; hi = 0.6f; }
        else if (k == "e2_pos") { lo = 0.25f; hi = 0.85f; }
        else if (flt && ends(k, "_cut")) { lo = 0.4f; hi = 1.0f; }
        else if (flt && ends(k, "_res")) { lo = 0.0f; hi = 0.75f; }
        else if (flt && ends(k, "_drive")) { lo = 0.0f; hi = 0.5f; }
        else if (flt && ends(k, "_env")) { lo = 0.35f; hi = 0.85f; }
        else if (flt && ends(k, "_key")) { lo = 0.0f; hi = 1.0f; }
        else if (k == "f1_type") { pickLo = F_LP12; pickFrom = F_PEAK + 1; }   // the classic types; combs and vowel by hand
        else if (k == "o1_wave" || k == "o2_wave") pickFrom = OW_PULSE + 1;   // no noise oscillator
        else if (osc && ends(k, "_pos")) { lo = 0.0f; hi = 1.0f; }
        else if (osc && ends(k, "_uni")) { lo = 0.0f; hi = 1.0f; }
        else if (osc && ends(k, "_detune")) { lo = 0.0f; hi = 0.7f; }
        else if (osc && ends(k, "_width")) { lo = 0.3f; hi = 1.0f; }
        else if (k == "o1_level") { lo = 0.6f; hi = 1.0f; }
        else if (k == "o2_level") { lo = 0.0f; hi = 0.9f; }
        else if (osc && ends(k, "_pan")) { lo = 0.35f; hi = 0.65f; }
        else if (osc && ends(k, "_sub_level")) { lo = 0.0f; hi = 0.5f; }
        else if (k == "noise_level") { lo = 0.0f; hi = 0.25f; }
        else if (k == "noise_color") { lo = 0.0f; hi = 1.0f; }
        else if (lfo && ends(k, "_rate")) { lo = 0.2f; hi = 0.8f; }
        else if (lfo && ends(k, "_depth")) { lo = 0.3f; hi = 1.0f; }
        else if (lfo && ends(k, "_wave")) pickFrom = LW_COUNT;
        else if (k == "engine") pickFrom = 3;
        else if (k[0] == 'm' && (ends(k, "_a1") || ends(k, "_a2") || ends(k, "_modamt"))) {
            const int slot = (i - P_M1_SRC) / stride;
            if (slot >= 0 && slot < kModSlots && want_[P_M1_SRC + slot * stride].load() > 0.0f) { lo = 0.2f; hi = 0.8f; }
        }
        pickFrom = std::min(pickFrom, PARAM_INFO[i].nopts);
        if (pickFrom > pickLo) {
            if (rnd() < amount) {
                const int steps = PARAM_INFO[i].nopts - 1;
                const int o = pickLo + std::min(static_cast<int>(rnd() * static_cast<float>(pickFrom - pickLo)), pickFrom - pickLo - 1);
                setValue(i, steps > 0 ? static_cast<float>(o) / static_cast<float>(steps) : 0.0f);
            }
            continue;
        }
        if (lo < 0.0f) continue;
        const float n = want_[i].load();
        setValue(i, n + amount * (lo + rnd() * (hi - lo) - n));
    }
    // New tables too, the further the more likely.
    for (int o = 0; o < kTableSlots; ++o)
        if (rnd() < amount * 0.6f) {
            const auto L = tableLibrary().listing();
            if (!L->items.empty()) {
                const size_t pick = std::min(L->items.size() - 1, static_cast<size_t>(rnd() * static_cast<float>(L->items.size())));
                loader_.want(o, L->items[pick].key, true);
            }
        }
    setPresetKey("");   // no longer any preset
}

std::string Surface::display(int i) const {
    if (i < 0 || i >= P_COUNT) return {};
    const ParamInfo& info = PARAM_INFO[i];
    switch (info.kind) {
        case Kind::Synth:
        case Kind::Ui: {
            const Fmt f = PARAM_SPECS[i].fmt;
            if (f == Fmt::Frame1 || f == Fmt::Frame2) return frameText(f == Fmt::Frame1 ? 0 : 1, want_[i].load());
            if (f == Fmt::ModAmt) return amountText(i);
            return paramDisplay(i, want_[i].load());
        }
        case Kind::Stepper:
        case Kind::Tile:
        case Kind::Readout: {
            std::lock_guard<std::mutex> lk(mtx_);
            return texts_[static_cast<size_t>(i)];
        }
        case Kind::Toggle: return want_[i].load() > 0.5f ? "On" : "Off";
        case Kind::Button:
        case Kind::Popup: return {};
    }
    return {};
}

// A matrix amount in its target's units: "+0.5 st", "+12 st", "x2.0", "+40%".
std::string Surface::amountText(int i) const {
    const int tgtParam = i - 1;   // m<k>_t<j> sits right before m<k>_a<j> (surface.py)
    const float a = paramValue(i, want_[i].load());
    const int t = static_cast<int>(paramValue(tgtParam, want_[tgtParam].load()));
    char b[32];
    switch (targetUnit(t)) {
        case Unit::Semis: std::snprintf(b, sizeof b, "%+.2f st", a * std::fabs(a) * kPitchRange); break;
        case Unit::Cutoff: std::snprintf(b, sizeof b, "%+.1f st", a * kCutoffRange); break;
        case Unit::EnvTime: std::snprintf(b, sizeof b, "x%.2f", std::exp2(std::fabs(a) * kEnvOctavesMod)); break;
        case Unit::LfoRate: std::snprintf(b, sizeof b, "x%.2f", std::exp2(std::fabs(a) * kLfoOctavesMod)); break;
        default: return paramDisplay(i, want_[i].load());
    }
    if ((targetUnit(t) == Unit::EnvTime || targetUnit(t) == Unit::LfoRate) && a < 0.0f) b[0] = '/';
    return b;
}

// XY auto-assign: every pad axis that no slot uses yet gets a free slot and the first target
// on its list that nothing modulates yet.
void Surface::autoAssignXy() {
    Batch batch(*this);   // a slot is source, target and amount: never seen half-written
    struct Pick { int target; float amount; };
    static const Pick prefs[kXyAxes][3] = {
        {{MT_F1_CUT, 0.5f}, {MT_CUT, 0.5f}, {MT_F2_CUT, 0.5f}},
        {{MT_F1_RES, 0.6f}, {MT_F2_RES, 0.6f}, {MT_NOISE_LEVEL, 0.5f}},
        {{MT_O1_POS, 1.0f}, {MT_O2_POS, 1.0f}, {MT_O1_DETUNE, 0.5f}},
        {{MT_O2_POS, 1.0f}, {MT_O1_POS, 1.0f}, {MT_O2_DETUNE, 0.5f}},
        {{MT_L1_RATE, 0.4f}, {MT_L2_RATE, 0.4f}, {MT_E2_D, 0.5f}},
        {{MT_L1_DEPTH, 1.0f}, {MT_L2_DEPTH, 1.0f}, {MT_SUB1_LEVEL, 0.5f}},
        {{MT_O1_DETUNE, 0.5f}, {MT_O2_DETUNE, 0.5f}, {MT_E1_A, 0.5f}},
        {{MT_NOISE_LEVEL, 0.5f}, {MT_SUB1_LEVEL, 0.5f}, {MT_VOLUME, -0.5f}},
    };
    const int stride = P_M2_SRC - P_M1_SRC;
    auto val = [this](int id) { return static_cast<int>(paramValue(id, want_[id].load())); };
    bool srcUsed[MS_COUNT] = {}, tgtUsed[MT_COUNT] = {};
    for (int k = 0; k < kModSlots; ++k) {
        const int d = k * stride;
        if (val(P_M1_SRC + d) == MS_NONE) continue;   // an empty slot's leftovers modulate nothing
        srcUsed[std::clamp(val(P_M1_SRC + d), 0, MS_COUNT - 1)] = true;
        srcUsed[std::clamp(val(P_M1_VIA + d), 0, MS_COUNT - 1)] = true;
        tgtUsed[std::clamp(val(P_M1_T1 + d), 0, MT_COUNT - 1)] = true;
        tgtUsed[std::clamp(val(P_M1_T2 + d), 0, MT_COUNT - 1)] = true;
    }
    int slot = 0;
    for (int axis = 0; axis < kXyAxes; ++axis) {
        if (srcUsed[MS_X1 + axis]) continue;
        while (slot < kModSlots && val(P_M1_SRC + slot * stride) != MS_NONE) ++slot;
        if (slot == kModSlots) return;   // the matrix is full
        for (const Pick& pk : prefs[axis]) {
            if (tgtUsed[pk.target]) continue;
            const int d = slot * stride;
            setValue(P_M1_SRC + d, paramNorm(P_M1_SRC + d, static_cast<float>(MS_X1 + axis)));
            setValue(P_M1_T1 + d, paramNorm(P_M1_T1 + d, static_cast<float>(pk.target)));
            setValue(P_M1_A1 + d, paramNorm(P_M1_A1 + d, pk.amount));
            // The rest of the slot starts clean: no leftover via, modifier or second target.
            setValue(P_M1_VIA + d, paramNorm(P_M1_VIA + d, static_cast<float>(MS_NONE)));
            setValue(P_M1_MOD + d, paramNorm(P_M1_MOD + d, static_cast<float>(MM_NONE)));
            setValue(P_M1_MODAMT + d, paramNorm(P_M1_MODAMT + d, 0.0f));
            setValue(P_M1_T2 + d, paramNorm(P_M1_T2 + d, static_cast<float>(MT_OFF)));
            setValue(P_M1_A2 + d, paramNorm(P_M1_A2 + d, 0.0f));
            tgtUsed[pk.target] = true;
            ++slot;
            break;
        }
    }
}

std::string Surface::frameText(int osc, float n) const {
    char b[32];
    const int waveParam = osc == 0 ? P_O1_WAVE : P_O2_WAVE;
    const int wave = static_cast<int>(std::lround(want_[waveParam].load() * (PARAM_INFO[waveParam].nopts - 1)));
    if (wave == OW_PULSE) {   // the pulse table's frames run from 50% to 3% width
        std::snprintf(b, sizeof b, "WIDTH %.0f%%", 50.0f - 47.0f * clamp01(n));
        return b;
    }
    if (wave == OW_NOISE) {
        const float c = 2.0f * clamp01(n) - 1.0f;
        if (std::fabs(c) < 0.01f) return "WHITE";
        std::snprintf(b, sizeof b, "%s %.0f%%", c < 0.0f ? "DARK" : "BRIGHT", std::fabs(c) * 100.0f);
        return b;
    }
    if (wave != OW_TABLE) return "-";   // one-frame shapes: nothing to scan
    const Loader::View v = loader_.view(osc);
    const int frames = std::max(1, v.info);
    std::snprintf(b, sizeof b, "FRAME %d / %d", 1 + static_cast<int>(std::lround(clamp01(n) * (frames - 1))), frames);
    return b;
}

std::string Surface::tableText(int osc) const {
    const Loader::View v = loader_.view(osc);
    const std::string label = tableLibrary().listing()->label(v.key);
    if (v.state == Loader::Loading) return "LOADING " + label;
    if (v.state == Loader::Missing) return "MISSING " + label;
    return label;
}

std::string Surface::tuningText() const {
    const Loader::View v = loader_.view(kTuningSlot);
    const std::string label = tuningLibrary().listing()->label(v.key);
    if (v.state == Loader::Loading) return "LOADING " + label;
    if (v.state == Loader::Missing) return "MISSING " + label;
    return "TUNING  " + label;
}

// LOADING while it lasts; MISSING for a few seconds (the stepper keeps saying it), then the
// CPU meter comes back.
std::string Surface::busyText() const {
    for (int o = 0; o < kTableSlots; ++o) {
        const Loader::View v = loader_.view(o);
        if (v.state == Loader::Loading) return "OSC " + std::to_string(o + 1) + " LOADING " + tableLibrary().listing()->label(v.key);
        if (v.state == Loader::Missing && v.ageMs < kMissingShowMs)
            return "OSC " + std::to_string(o + 1) + " MISSING " + tableLibrary().listing()->label(v.key);
    }
    const Loader::View t = loader_.view(kTuningSlot);
    if (t.state == Loader::Loading) return "TUNING LOADING " + tuningLibrary().listing()->label(t.key);
    if (t.state == Loader::Missing && t.ageMs < kMissingShowMs) return "TUNING MISSING " + tuningLibrary().listing()->label(t.key);
    return {};
}

// --- any thread ---------------------------------------------------------------------------

void Surface::refresh() {
    // Everything under the lock, the target included: a refresh from the loader's thread must
    // not finish last with a browser target the UI has changed meanwhile.
    std::lock_guard<std::mutex> lk(mtx_);
    const int target = browseTarget();
    FileLibrary& lib = browseLibrary(target);
    const auto L = lib.listing();
    const auto T = tableLibrary().listing();
    const auto TU = tuningLibrary().listing();
    const auto PR = presetLibrary().listing();
    std::vector<std::string> t(P_COUNT);

    // Steppers: position in the flat list, text = the item.
    auto place = [this](int param, const Listing& lst, const std::string& key) {
        const int idx = lst.find(key);
        const int range = stepperRange(static_cast<int>(lst.items.size()));
        if (idx >= 0) put(param, std::min(1.0f, static_cast<float>(idx) / static_cast<float>(range)));
    };
    for (int o = 0; o < kTableSlots; ++o) {
        place(kTableParam[o], *T, loader_.wanted(o));
        t[static_cast<size_t>(kTableParam[o])] = tableText(o);
    }
    place(P_TUNING, *TU, loader_.wanted(kTuningSlot));
    t[P_TUNING] = tuningText();
    place(P_PRESET, *PR, presetKey_);
    t[P_PRESET] = presetKey_.empty() ? "PRESET  -" : "PRESET  " + PR->label(presetKey_);

    // Browser: follow the target's table (or preset) when it changed from outside the browser.
    const std::string key = browseKey(target);
    const std::vector<Category> cats = categories(lib, *L);
    const int ncat = static_cast<int>(cats.size());
    if (target != browsed_) {   // another library: start from its own place
        browsed_ = target;
        followed_ = "\x01";
    }
    brCat_ = clampi(brCat_, 0, ncat - 1);
    auto pos = [&cats](int c, const std::string& k) {
        const auto& keys = cats[static_cast<size_t>(c)].keys;
        const auto it = std::find(keys.begin(), keys.end(), k);
        return it == keys.end() ? -1 : static_cast<int>(it - keys.begin());
    };
    if (key != followed_) {
        followed_ = key;
        if (pos(brCat_, key) < 0) {
            const int c = L->categoryOf(L->find(key));
            brCat_ = c >= 0 ? c + 2 : 2;   // past FAVORITES and RECENT
            brCat_ = clampi(brCat_, 0, ncat - 1);
            itemPage_ = 0;
        }
        const int p = pos(brCat_, key);
        if (p >= 0) itemPage_ = p / kBrowserItems;
        catPage_ = brCat_ / kBrowserCats;
    }
    const int catPages = std::max(1, (ncat + kBrowserCats - 1) / kBrowserCats);
    catPage_ = clampi(catPage_, 0, catPages - 1);
    catTiles_.assign(kBrowserCats, -1);
    for (int k = 0; k < kBrowserCats; ++k) {
        const int c = catPage_ * kBrowserCats + k;
        const bool has = c < ncat;
        catTiles_[static_cast<size_t>(k)] = has ? c : -1;
        put(kCatTiles[k], has && c == brCat_ ? 1.0f : 0.0f);
        t[static_cast<size_t>(kCatTiles[k])] = has ? upper(cats[static_cast<size_t>(c)].name) : "";
    }
    const auto& keys = cats[static_cast<size_t>(brCat_)].keys;
    const int nitems = static_cast<int>(keys.size());
    const int itemPages = std::max(1, (nitems + kBrowserItems - 1) / kBrowserItems);
    itemPage_ = clampi(itemPage_, 0, itemPages - 1);
    tileKeys_.assign(kBrowserItems, std::string());
    for (int k = 0; k < kBrowserItems; ++k) {
        const int j = itemPage_ * kBrowserItems + k;
        const bool has = j < nitems;
        const std::string& tk = has ? keys[static_cast<size_t>(j)] : std::string();
        tileKeys_[static_cast<size_t>(k)] = tk;
        put(kItemTiles[k], has && tk == key ? 1.0f : 0.0f);
        if (has) {
            const int idx = L->find(tk);
            t[static_cast<size_t>(kItemTiles[k])] = idx >= 0 ? L->items[static_cast<size_t>(idx)].name : L->label(tk);
        }
    }
    char b[64];
    std::snprintf(b, sizeof b, "PAGE %d / %d", itemPage_ + 1, itemPages);
    t[P_TBL_PAGE] = b;
    if (target == kPresetTarget) {
        t[P_BR_NOW] = presetKey_.empty() ? "PRESET  -" : "PRESET  " + PR->label(presetKey_);
    } else {
        const Loader::View v = loader_.view(target);
        std::snprintf(b, sizeof b, "  %d FR", std::max(1, v.info));
        t[P_BR_NOW] = "OSC " + std::to_string(target + 1) + "  " + tableText(target) + (v.state == Loader::Ready ? b : "");
    }
    put(P_FAV, !key.empty() && lib.isFavorite(key) ? 1.0f : 0.0f);

    size_t h = 0;
    std::hash<std::string> hs;
    for (int i = 0; i < P_COUNT; ++i) h = h * 1000003u ^ hs(t[static_cast<size_t>(i)]);
    // Frame counts show in the position knobs' text: a new table must refresh them too.
    for (int o = 0; o < kTableSlots; ++o) h = h * 1000003u ^ static_cast<size_t>(loader_.view(o).info);
    texts_.swap(t);
    if (h != textHash_) {
        textHash_ = h;
        textGen_.fetch_add(1, std::memory_order_release);
    }
    // A MISSING note in the status line ends by itself: tell the audio thread when, so it has
    // MPC re-read the line then (nothing else may change to prompt it).
    const long long now = nowMs();
    long long until = 0;
    for (int slot : {0, 1, kTuningSlot}) {
        const Loader::View v = loader_.view(slot);
        if (v.state == Loader::Missing && v.ageMs < kMissingShowMs) until = std::max(until, now + kMissingShowMs - v.ageMs + 1);
    }
    if (until > 0) statusUntil_.store(until, std::memory_order_release);
}

bool Surface::statusExpired() {
    long long until = statusUntil_.load(std::memory_order_acquire);
    if (until == 0 || nowMs() < until) return false;
    return statusUntil_.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
}

// --- state --------------------------------------------------------------------------------

void Surface::setValue(int i, float n) {
    if (i < 0 || i >= P_COUNT) return;
    const int steps = stepsOf(i);
    n = clamp01(n);
    if (steps > 0) n = std::round(n * steps) / steps;
    put(i, n);
}

void Surface::setTable(int osc, const std::string& key, bool now) {
    if (osc >= 0 && osc < kTableSlots) loader_.want(osc, key, now);
}

std::string Surface::tableKey(int osc) const { return loader_.wanted(osc); }

void Surface::setTuning(const std::string& key, bool now) { loader_.want(kTuningSlot, key, now); }

std::string Surface::tuningKey() const { return loader_.wanted(kTuningSlot); }

std::string Surface::presetKey() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return presetKey_;
}

void Surface::setPresetKey(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    presetKey_ = key;
}

// --- audio thread -------------------------------------------------------------------------

bool Surface::snapshot(float* out) const {
    const uint32_t before = batchSeq_.load(std::memory_order_acquire);
    if (before & 1u) return false;   // a preset is half written
    float tmp[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) tmp[i] = want_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (batchSeq_.load(std::memory_order_relaxed) != before) return false;   // one started meanwhile
    std::copy(tmp, tmp + P_COUNT, out);
    return true;
}

void Surface::notify(AutomateFn automate, UpdateFn update, void* ctx) {
    // Nothing changed since the last full pass: no need to look at 436 values every block.
    const uint32_t changes = changes_.load(std::memory_order_acquire);
    if (changes != scanned_ || scanPending_) {
        int pushed = 0, n = 0;
        for (; n < P_COUNT && pushed < kMaxAutomatePerBlock; ++n) {
            const int i = cursor_;
            cursor_ = (cursor_ + 1) % P_COUNT;
            if (PARAM_INFO[i].kind == Kind::Button) {
                if (release_[i].exchange(false, std::memory_order_acq_rel)) {
                    automate(ctx, i, 0.0f);
                    shown_[i].store(0.0f, std::memory_order_relaxed);
                    ++pushed;
                }
                continue;
            }
            if (PARAM_INFO[i].kind == Kind::Readout) continue;
            const float w = want_[i].load(std::memory_order_relaxed);
            if (std::fabs(w - shown_[i].load(std::memory_order_relaxed)) > 1e-4f) {
                automate(ctx, i, w);
                shown_[i].store(w, std::memory_order_relaxed);
                ++pushed;
            }
        }
        scanPending_ = n < P_COUNT;   // stopped at the per-block cap: go on next block
        if (!scanPending_) scanned_ = changes;
    }
    if (sinceText_ < kTextEveryBlocks) ++sinceText_;   // saturates: no overflow in a long session
    if (sinceText_ >= kTextEveryBlocks) {
        const uint32_t g = textGen_.load(std::memory_order_acquire);
        if (g != textSeen_) {
            textSeen_ = g;
            sinceText_ = 0;
            update(ctx);
        }
    }
}

} // namespace pf
