#include "tuning.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace pf {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

std::string stemOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

constexpr double kNote0Hz = 8.17579891564371;   // MIDI note 0 in 12-TET at A = 440

// Whole-token numbers: "abc", "6.8x", "3/2/7" or "101,5" are errors, not 0 or a prefix's value.
bool number(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(v);
}

bool integer(const std::string& s, long& v) {
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    if (s.size() > 9) return false;   // no overflow, far beyond any count or ratio term we take
    v = std::strtol(s.c_str(), nullptr, 10);
    return true;
}

} // namespace

const Tuning& equalTemperament() {
    static const Tuning t = [] {
        Tuning e;
        e.name = "12-TET";
        for (int n = 0; n < 128; ++n) e.pitch[n] = static_cast<float>(n);
        return e;
    }();
    return t;
}

bool parseTun(const std::string& text, Tuning& out, std::string* err) {
    double base = kNote0Hz;
    double cents[128];
    bool have[128] = {}, exact[128] = {};
    std::string section;
    std::istringstream in(text);
    int found = 0;
    for (std::string raw; std::getline(in, raw);) {
        std::string line = trim(raw);
        const size_t semi = line.find(';');
        if (semi != std::string::npos) line = trim(line.substr(0, semi));
        if (line.empty()) continue;
        if (line.front() == '[') {
            section = lower(line);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "basefreq") {
            double b = 0.0;
            if (number(val, b) && b > 0.0) base = b;
            continue;
        }
        if (key.compare(0, 4, "note") != 0) continue;
        long n = -1;   // "note 60": the index after "note", digits only
        if (!integer(trim(key.substr(4)), n) || n > 127) continue;
        double c = 0.0;
        if (!number(val, c)) continue;   // a line we can't read keeps the note's default
        const bool isExact = section == "[exact tuning]";
        if (section != "[tuning]" && !isExact) continue;
        if (exact[n] && !isExact) continue;   // [Exact Tuning] wins
        cents[static_cast<size_t>(n)] = c;
        have[n] = true;
        exact[n] = exact[n] || isExact;
        ++found;
    }
    if (found == 0) return fail(err, "no note lines");
    // AnaMark: [Tuning] cents count from the fixed 8.1757989 Hz; [Exact Tuning] ones from BaseFreq.
    const double offset = 12.0 * std::log2(base / kNote0Hz);
    const Tuning& et = equalTemperament();
    for (int n = 0; n < 128; ++n)
        out.pitch[n] = have[n] ? static_cast<float>(cents[n] / 100.0 + (exact[n] ? offset : 0.0)) : et.pitch[n];
    return true;
}

bool parseScl(const std::string& text, Tuning& out, std::string* err) {
    std::istringstream in(text);
    std::vector<std::string> lines;
    for (std::string raw; std::getline(in, raw);) {
        const std::string line = trim(raw);
        if (!line.empty() && line[0] == '!') continue;
        lines.push_back(line);
    }
    if (lines.size() < 2) return fail(err, "too short");
    std::string countText = lines[1];
    const size_t csp = countText.find_first_of(" \t");
    if (csp != std::string::npos) countText = countText.substr(0, csp);
    long countL = 0;
    if (!integer(countText, countL)) return fail(err, "bad degree count");
    const int count = static_cast<int>(countL);
    if (count < 1 || count > 1000 || static_cast<int>(lines.size()) < 2 + count) return fail(err, "bad degree count");
    std::vector<double> deg(static_cast<size_t>(count));   // cents
    for (int i = 0; i < count; ++i) {
        std::string v = lines[static_cast<size_t>(2 + i)];
        const size_t sp = v.find_first_of(" \t");
        if (sp != std::string::npos) v = v.substr(0, sp);
        double c = 0.0;
        if (v.find('.') != std::string::npos) {   // cents
            if (!number(v, c)) return fail(err, "bad degree");
        } else {                                   // a ratio, "3/2", or a whole number, "2"
            const size_t slash = v.find('/');
            long num = 0, den = 1;
            if (!integer(v.substr(0, slash), num) || (slash != std::string::npos && !integer(v.substr(slash + 1), den)))
                return fail(err, "bad ratio");
            if (num <= 0 || den <= 0) return fail(err, "bad ratio");
            c = 1200.0 * std::log2(static_cast<double>(num) / static_cast<double>(den));
        }
        if (!std::isfinite(c)) return fail(err, "bad degree");
        deg[static_cast<size_t>(i)] = c;
    }
    const double period = deg.back();
    if (period <= 0.0) return fail(err, "period must rise");
    for (int n = 0; n < 128; ++n) {
        const int d = n - 60;
        const int oct = static_cast<int>(std::floor(static_cast<double>(d) / count));
        const int idx = d - oct * count;
        const double c = oct * period + (idx == 0 ? 0.0 : deg[static_cast<size_t>(idx - 1)]);
        out.pitch[n] = static_cast<float>(60.0 + c / 100.0);
    }
    return true;
}

bool loadTuning(const std::string& path, Tuning& out, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(err, "cannot open");
    const std::streamoff size = f.tellg();
    if (size < 0 || size > (1 << 20)) return fail(err, "too big");   // checked before reading it
    f.seekg(0);
    std::string text(static_cast<size_t>(size), '\0');
    if (size > 0 && !f.read(&text[0], size)) return fail(err, "cannot read");
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a UTF-8 byte-order mark
    const std::string ext = lower(path.size() >= 4 ? path.substr(path.size() - 4) : path);
    Tuning t;
    t.name = stemOf(path);
    const bool ok = ext == ".scl" ? parseScl(text, t, err) : parseTun(text, t, err);
    if (!ok) return false;
    // Wide scales (a tritave per degree, 2-EDO) run far out at the keyboard's ends: those
    // notes are clamped (inaudible either way), the scale still loads.
    for (float& p : t.pitch) {
        if (!std::isfinite(p)) return fail(err, "pitch out of range");
        p = std::max(-200.0f, std::min(p, 400.0f));
    }
    out = std::move(t);
    return true;
}

} // namespace pf
