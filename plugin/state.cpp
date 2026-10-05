#include "state.h"

#include "patch_map.h"
#include "../dsp/wavetable.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace pf {
namespace {
constexpr const char* kMagic = "polyforce ";

// Numbers in the C locale whatever the process's is (a "0,5" would misread every preset).
std::string number(float v) {
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, v, std::chars_format::general, 6);   // 6 digits: 333 Hz, not 332.9999
    return std::string(b, r.ptr);
}

bool parse(const std::string& s, float& out) {
    size_t a = 0, e = s.size();
    while (a < e && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    if (a < e && s[a] == '+') ++a;
    float v = 0.0f;
    const auto r = std::from_chars(s.data() + a, s.data() + e, v);
    if (r.ec != std::errc() || !std::isfinite(v)) return false;
    out = v;
    return true;
}
}

bool isStateText(const std::string& text) {
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;   // a UTF-8 byte-order mark (a text editor's)
    if (text.compare(at, std::strlen(kMagic), kMagic) != 0) return false;
    at += std::strlen(kMagic);
    int version = 0;
    const auto r = std::from_chars(text.data() + at, text.data() + text.size(), version);
    return r.ec == std::errc() && version >= 1;
}

std::string saveState(const Surface& s, bool asPreset) {
    std::string out = std::string(kMagic) + std::to_string(kStateVersion) + "\n";
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        out += PARAM_INFO[i].key;
        out += '=';
        out += number(paramValue(i, s.get(i)));
        out += '\n';
    }
    for (int o = 0; o < 2; ++o) out += "o" + std::to_string(o + 1) + "_table=" + s.tableKey(o) + "\n";
    out += "tuning=" + s.tuningKey() + "\n";
    if (!asPreset && !s.presetKey().empty()) out += "preset=" + s.presetKey() + "\n";
    return out;
}

bool loadState(Surface& s, const std::string& textIn, bool asPreset) {
    if (!isStateText(textIn)) return false;
    const std::string text = textIn.compare(0, 3, "\xEF\xBB\xBF") == 0 ? textIn.substr(3) : textIn;
    int version = 1;
    std::from_chars(text.data() + std::strlen(kMagic), text.data() + text.size(), version);
    const bool normalised = version < 2;
    Surface::Batch batch(s);   // the audio thread never plays a half-loaded sound
    if (asPreset)
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth) s.setValue(i, PARAM_INFO[i].def);
    // A preset is complete (what it doesn't name is the default); a project changes only what it lists.
    std::string tables[2] = {"builtin:Classic", "builtin:Classic"};
    std::string tuning = "builtin:12-TET", preset;
    bool sawTable[2] = {asPreset, asPreset}, sawTuning = asPreset;
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
            sawTable[key[1] - '1'] = true;
            continue;
        }
        if (key == "tuning") {
            tuning = val;
            sawTuning = true;
            continue;
        }
        if (key == "preset") {
            preset = val;
            continue;
        }
        if (version < 3 && (key == "o1_wave" || key == "o2_wave")) {   // v2: a built-in table by index
            constexpr int kV2Builtins = 4;   // Classic, PWM, Sync, Formant: all there was then
            float v = 0.0f;
            parse(val, v);
            const int idx = std::clamp(static_cast<int>(normalised ? std::lround(v * 3.0f) : std::lround(v)), 0,
                                       kV2Builtins - 1);
            tables[key[1] - '1'] = std::string("builtin:") + builtinName(idx);
            sawTable[key[1] - '1'] = true;
            continue;
        }
        sawRoute2 = sawRoute2 || key == "o2_route";
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth && key == PARAM_INFO[i].key) {
                float v = 0.0f;
                if (parse(val, v)) s.setValue(i, normalised ? std::clamp(v, 0.0f, 1.0f) : paramNorm(i, v));
                break;
            }
    }
    // Before version 4, Parallel meant osc 1 -> F1 and osc 2 -> F2; now every source has its
    // own route (default F1) and Parallel only stops F1 feeding F2.
    if (version < 4 && paramValue(P_ROUTING, s.get(P_ROUTING)) > 0.5f && !sawRoute2)
        s.setValue(P_O2_ROUTE, paramNorm(P_O2_ROUTE, RT_F2));
    for (int o = 0; o < 2; ++o)
        if (sawTable[o]) s.setTable(o, tables[o], true);
    if (sawTuning) s.setTuning(tuning, true);
    if (!asPreset) s.setPresetKey(preset);   // a project without one came from no preset
    s.refresh();
    return true;
}

} // namespace pf
