#include "state.h"

#include "patch_map.h"
#include "../dsp/wavetable.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pf {
namespace {
constexpr const char* kMagic = "polyforce ";
}

std::string saveState(const Surface& s, bool asPreset) {
    std::string out = std::string(kMagic) + std::to_string(kStateVersion) + "\n";
    char b[96];
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        std::snprintf(b, sizeof b, "%s=%.6g\n", PARAM_INFO[i].key,   // 6 digits: 333 Hz, not 332.9999
                      static_cast<double>(paramValue(i, s.get(i))));
        out += b;
    }
    for (int o = 0; o < 2; ++o) out += "o" + std::to_string(o + 1) + "_table=" + s.tableKey(o) + "\n";
    out += "tuning=" + s.tuningKey() + "\n";
    if (!asPreset && !s.presetKey().empty()) out += "preset=" + s.presetKey() + "\n";
    return out;
}

bool loadState(Surface& s, const std::string& text, bool asPreset) {
    const size_t magic = std::strlen(kMagic);
    if (text.compare(0, magic, kMagic) != 0) return false;
    const int version = std::atoi(text.c_str() + magic);
    const bool normalised = version < 2;
    if (asPreset)
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth) s.setValue(i, PARAM_INFO[i].def);
    std::string tables[2] = {"builtin:Classic", "builtin:Classic"};
    std::string tuning = "builtin:12-TET", preset;
    bool sawRoute2 = false;
    size_t at = text.find('\n');
    while (at != std::string::npos && at + 1 < text.size()) {
        const size_t end = text.find('\n', at + 1);
        std::string line = text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
        at = end;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "o1_table" || key == "o2_table") {
            tables[key[1] - '1'] = val;
            continue;
        }
        if (key == "tuning") {
            tuning = val;
            continue;
        }
        if (key == "preset") {
            preset = val;
            continue;
        }
        if (version < 3 && (key == "o1_wave" || key == "o2_wave")) {   // v2: a built-in table by index
            const auto& b = builtinTables();
            const float v = std::strtof(val.c_str(), nullptr);
            const int idx = std::isfinite(v) ? std::clamp(static_cast<int>(normalised ? std::lround(v * 3.0f) : std::lround(v)), 0,
                                                          static_cast<int>(b.size()) - 1) : 0;
            tables[key[1] - '1'] = "builtin:" + b[static_cast<size_t>(idx)].name;
            continue;
        }
        sawRoute2 = sawRoute2 || key == "o2_route";
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth && key == PARAM_INFO[i].key) {
                const float v = std::strtof(val.c_str(), nullptr);
                if (std::isfinite(v)) s.setValue(i, normalised ? std::clamp(v, 0.0f, 1.0f) : paramNorm(i, v));
                break;
            }
    }
    // Before version 4, Parallel meant osc 1 -> F1 and osc 2 -> F2; now every source has its
    // own route (default F1) and Parallel only stops F1 feeding F2.
    if (version < 4 && paramValue(P_ROUTING, s.get(P_ROUTING)) > 0.5f && !sawRoute2)
        s.setValue(P_O2_ROUTE, paramNorm(P_O2_ROUTE, RT_F2));
    for (int o = 0; o < 2; ++o) s.setTable(o, tables[o], true);
    s.setTuning(tuning, true);
    if (!asPreset) s.setPresetKey(preset);
    s.refresh();
    return true;
}

} // namespace pf
