#include "surface.h"

#include "library.h"
#include "patch_map.h"

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
constexpr int kTableSlots          = 2;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
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
    if (k == Kind::Button) {
        const bool down = n > 0.5f;
        const bool rising = down && !held_[i];
        held_[i] = down;
        if (rising) {
            release_[i] = true;
            apply(i, n);
            refresh();
        }
        return;
    }
    apply(i, n);
    if (k != Kind::Synth) refresh();   // a sound parameter's text is computed when MPC asks
}

void Surface::apply(int i, float n) {
    const ParamInfo& info = PARAM_INFO[i];
    switch (info.kind) {
        case Kind::Synth:
        case Kind::Ui: {
            const int steps = stepsOf(i);
            if (steps > 0 && steps < kFine) {
                const int cur = static_cast<int>(std::lround(want_[i].load() * steps));
                want_[i].store(static_cast<float>(stepIndex(i, n, steps, cur)) / static_cast<float>(steps));
                if (exactOption(n, steps)) {   // a tap on a list row closes its popup (a Q-Link nudge doesn't)
                    const int flag = popupFlagOf(i);
                    if (flag >= 0) want_[flag].store(0.0f);
                }
            } else {
                want_[i].store(n);
            }
            break;
        }
        case Kind::Popup: want_[i].store(n > 0.5f ? 1.0f : 0.0f); break;
        case Kind::Stepper:
            for (int o = 0; o < kTableSlots; ++o)
                if (i == kTableParam[o]) {
                    const auto L = tableLibrary().listing();
                    const int items = static_cast<int>(L->items.size());
                    const int cur = std::max(0, L->find(loader_.wanted(o)));
                    const int pick = stepItem(i, n, kStepperRange, items, cur);
                    if (pick != cur && pick < items) loader_.want(o, L->items[static_cast<size_t>(pick)].key, false);
                }
            break;
        case Kind::Button:
            for (int o = 0; o < kTableSlots; ++o) {
                if (i == kTablePrev[o]) stepTable(o, -1);
                if (i == kTableNext[o]) stepTable(o, +1);
            }
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
    const auto L = tableLibrary().listing();
    const int items = static_cast<int>(L->items.size());
    if (items == 0) return;
    const int cur = L->find(loader_.wanted(osc));
    const int pick = cur < 0 ? 0 : clampi(cur + delta, 0, items - 1);
    loader_.want(osc, L->items[static_cast<size_t>(pick)].key, false);
}

std::vector<Surface::Category> Surface::categories() const {
    FileLibrary& lib = tableLibrary();
    const auto L = lib.listing();
    std::vector<Category> out;
    out.push_back({"FAVORITES", lib.favorites()});
    out.push_back({"RECENT", lib.recent()});
    for (size_t c = 0; c < L->categories.size(); ++c) {
        Category cat{L->categories[c], {}};
        for (int m : L->members[c]) cat.keys.push_back(L->items[static_cast<size_t>(m)].key);
        out.push_back(std::move(cat));
    }
    return out;
}

void Surface::browserAction(int i) {
    const int target = clampi(static_cast<int>(std::lround(want_[P_BR_TARGET].load())), 0, kTableSlots - 1);
    FileLibrary& lib = tableLibrary();
    std::lock_guard<std::mutex> lk(mtx_);
    const std::vector<Category> cats = categories();
    const int ncat = static_cast<int>(cats.size());
    for (int t = 0; t < kBrowserCats; ++t)
        if (i == kCatTiles[t] && t < static_cast<int>(catTiles_.size()) && catTiles_[static_cast<size_t>(t)] >= 0) {
            brCat_ = catTiles_[static_cast<size_t>(t)];
            itemPage_ = 0;
            followed_ = loader_.wanted(target);   // the next refresh must not jump back to its category
        }
    for (int t = 0; t < kBrowserItems; ++t)
        if (i == kItemTiles[t] && t < static_cast<int>(tileKeys_.size()) && !tileKeys_[static_cast<size_t>(t)].empty()) {
            const std::string key = tileKeys_[static_cast<size_t>(t)];
            loader_.want(target, key, false);
            followed_ = key;   // picked here: stay on this category and page
        }
    const int catPages = std::max(1, (ncat + kBrowserCats - 1) / kBrowserCats);
    if (i == P_CAT_PREV) catPage_ = clampi(catPage_ - 1, 0, catPages - 1);
    if (i == P_CAT_NEXT) catPage_ = clampi(catPage_ + 1, 0, catPages - 1);
    const int nitems = brCat_ < ncat ? static_cast<int>(cats[static_cast<size_t>(brCat_)].keys.size()) : 0;
    const int itemPages = std::max(1, (nitems + kBrowserItems - 1) / kBrowserItems);
    if (i == P_TBL_PREV) itemPage_ = clampi(itemPage_ - 1, 0, itemPages - 1);
    if (i == P_TBL_NEXT) itemPage_ = clampi(itemPage_ + 1, 0, itemPages - 1);

    const std::string cur = loader_.wanted(target);
    if (i == P_FAV) lib.setFavorite(cur, !lib.isFavorite(cur));
    if (i == P_RND && brCat_ < ncat) {   // a random table from this category, never the current one
        std::vector<std::string> pool;
        for (const std::string& k : cats[static_cast<size_t>(brCat_)].keys)
            if (k != cur) pool.push_back(k);
        if (!pool.empty()) {
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            const std::string k = pool[rng_ % pool.size()];
            loader_.want(target, k, true);
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

std::string Surface::display(int i) const {
    if (i < 0 || i >= P_COUNT) return {};
    const ParamInfo& info = PARAM_INFO[i];
    switch (info.kind) {
        case Kind::Synth:
        case Kind::Ui: {
            const Fmt f = PARAM_SPECS[i].fmt;
            if (f == Fmt::Frame1 || f == Fmt::Frame2) return frameText(f == Fmt::Frame1 ? 0 : 1, want_[i].load());
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

std::string Surface::busyText() const {
    for (int o = 0; o < kTableSlots; ++o) {
        const Loader::View v = loader_.view(o);
        if (v.state == Loader::Loading) return "OSC " + std::to_string(o + 1) + " LOADING " + tableLibrary().listing()->label(v.key);
        if (v.state == Loader::Missing) return "OSC " + std::to_string(o + 1) + " MISSING " + tableLibrary().listing()->label(v.key);
    }
    return {};
}

// --- any thread ---------------------------------------------------------------------------

void Surface::refresh() {
    const auto L = tableLibrary().listing();
    FileLibrary& lib = tableLibrary();
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> t(P_COUNT);

    // Table steppers: position in the flat list, text = the table.
    for (int o = 0; o < kTableSlots; ++o) {
        const int idx = L->find(loader_.wanted(o));
        if (idx >= 0) want_[kTableParam[o]].store(static_cast<float>(idx) / kStepperRange);
        t[static_cast<size_t>(kTableParam[o])] = tableText(o);
    }

    // Browser: follow the target oscillator's table when it changed from outside the browser.
    const int target = clampi(static_cast<int>(std::lround(want_[P_BR_TARGET].load())), 0, kTableSlots - 1);
    const std::string key = loader_.wanted(target);
    const std::vector<Category> cats = categories();
    const int ncat = static_cast<int>(cats.size());
    brCat_ = clampi(brCat_, 0, ncat - 1);
    auto pos = [&cats](int c, const std::string& k) {
        const auto& keys = cats[static_cast<size_t>(c)].keys;
        const auto it = std::find(keys.begin(), keys.end(), k);
        return it == keys.end() ? -1 : static_cast<int>(it - keys.begin());
    };
    if (key != followed_) {
        followed_ = key;
        if (pos(brCat_, key) < 0) {
            const int item = L->find(key);
            const int c = L->categoryOf(item);
            if (c >= 0) brCat_ = c + 2;   // past FAVORITES and RECENT
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
        want_[kCatTiles[k]].store(has && c == brCat_ ? 1.0f : 0.0f);
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
        want_[kItemTiles[k]].store(has && tk == key ? 1.0f : 0.0f);
        if (has) {
            const int idx = L->find(tk);
            t[static_cast<size_t>(kItemTiles[k])] = idx >= 0 ? L->items[static_cast<size_t>(idx)].name : L->label(tk);
        }
    }
    char b[64];
    std::snprintf(b, sizeof b, "PAGE %d / %d", itemPage_ + 1, itemPages);
    t[P_TBL_PAGE] = b;
    const Loader::View v = loader_.view(target);
    std::snprintf(b, sizeof b, "  %d FR", std::max(1, v.info));
    t[P_BR_NOW] = "OSC " + std::to_string(target + 1) + "  " + tableText(target) + (v.state == Loader::Ready ? b : "");
    want_[P_FAV].store(lib.isFavorite(key) ? 1.0f : 0.0f);

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
}

// --- state --------------------------------------------------------------------------------

void Surface::setValue(int i, float n) {
    if (i < 0 || i >= P_COUNT) return;
    const int steps = stepsOf(i);
    n = clamp01(n);
    if (steps > 0) n = std::round(n * steps) / steps;
    want_[i].store(n);
}

void Surface::setTable(int osc, const std::string& key, bool now) {
    if (osc >= 0 && osc < kTableSlots) loader_.want(osc, key, now);
}

std::string Surface::tableKey(int osc) const { return loader_.wanted(osc); }

// --- audio thread -------------------------------------------------------------------------

void Surface::snapshot(float* out) const {
    for (int i = 0; i < P_COUNT; ++i) out[i] = want_[i].load(std::memory_order_relaxed);
}

void Surface::notify(AutomateFn automate, UpdateFn update, void* ctx) {
    int pushed = 0;
    for (int n = 0; n < P_COUNT && pushed < kMaxAutomatePerBlock; ++n) {
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
    if (++sinceText_ >= kTextEveryBlocks) {
        const uint32_t g = textGen_.load(std::memory_order_acquire);
        if (g != textSeen_) {
            textSeen_ = g;
            sinceText_ = 0;
            update(ctx);
        }
    }
}

} // namespace pf
