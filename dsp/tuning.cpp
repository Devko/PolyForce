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
            const double b = std::atof(val.c_str());
            if (b > 0.0 && std::isfinite(b)) base = b;
            continue;
        }
        if (key.compare(0, 4, "note") != 0) continue;
        const int n = std::atoi(key.c_str() + 4);
        if (n < 0 || n > 127) continue;
        const double c = std::atof(val.c_str());
        if (!std::isfinite(c)) continue;
        const bool isExact = section == "[exact tuning]";
        if (section != "[tuning]" && !isExact) continue;
        if (exact[n] && !isExact) continue;   // [Exact Tuning] wins
        cents[n] = c;
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
    const int count = std::atoi(lines[1].c_str());
    if (count < 1 || count > 1000 || static_cast<int>(lines.size()) < 2 + count) return fail(err, "bad degree count");
    std::vector<double> deg(static_cast<size_t>(count));   // cents
    for (int i = 0; i < count; ++i) {
        std::string v = lines[static_cast<size_t>(2 + i)];
        const size_t sp = v.find_first_of(" \t");
        if (sp != std::string::npos) v = v.substr(0, sp);
        double c;
        if (v.find('.') != std::string::npos) {
            c = std::atof(v.c_str());
        } else {
            const size_t slash = v.find('/');
            const double num = std::atof(v.c_str());
            const double den = slash == std::string::npos ? 1.0 : std::atof(v.c_str() + slash + 1);
            if (num <= 0.0 || den <= 0.0) return fail(err, "bad ratio");
            c = 1200.0 * std::log2(num / den);
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
    const std::string ext = lower(path.size() >= 4 ? path.substr(path.size() - 4) : path);
    Tuning t;
    t.name = stemOf(path);
    const bool ok = ext == ".scl" ? parseScl(text, t, err) : parseTun(text, t, err);
    if (!ok) return false;
    for (float p : t.pitch)
        if (!std::isfinite(p) || p < -200.0f || p > 400.0f) return fail(err, "pitch out of range");
    out = std::move(t);
    return true;
}

} // namespace pf
