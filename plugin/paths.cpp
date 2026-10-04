#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pf {
namespace {

std::vector<Root> roots(const char* env, const char* sub, const char* ssd) {
    std::vector<Root> out;
    if (const char* e = std::getenv(env)) {   // tests: replaces the device folders
        std::string s = e;
        size_t at = 0;
        int n = 0;
        while (at <= s.size()) {
            const size_t colon = s.find(':', at);
            const std::string dir = s.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
            if (!dir.empty()) {
                const char* label = n == 0 ? "plugin" : n == 1 ? "ssd" : nullptr;
                out.push_back({label ? label : "root" + std::to_string(n + 1), dir});
                ++n;
            }
            if (colon == std::string::npos) break;
            at = colon + 1;
        }
        return out;
    }
    const std::string plug = pluginDir();
    if (!plug.empty()) out.push_back({"plugin", plug + "/" + sub});
    out.push_back({"ssd", ssd});
    return out;
}

} // namespace

std::string pluginDir() {
    static const std::string dir = [] {
        FILE* f = std::fopen("/proc/self/maps", "r");
        if (!f) return std::string();
        char line[1024];
        const unsigned long me = reinterpret_cast<unsigned long>(&pluginDir);
        std::string found;
        while (found.empty() && std::fgets(line, sizeof line, f)) {
            unsigned long lo = 0, hi = 0;
            char* p = std::strchr(line, '/');
            if (std::sscanf(line, "%lx-%lx", &lo, &hi) == 2 && me >= lo && me < hi && p) {
                p[std::strcspn(p, "\n")] = 0;
                const char* slash = std::strrchr(p, '/');
                if (slash && slash != p) found.assign(p, static_cast<size_t>(slash - p));
            }
        }
        std::fclose(f);
        return found;
    }();
    return dir;
}

std::vector<Root> tableRoots() { return roots("PF_TABLE_ROOTS", "Wavetables", "/media/AkaiForce/Wavetables"); }
std::vector<Root> presetRoots() { return roots("PF_PRESET_ROOTS", "Presets", "/media/AkaiForce/PolyForce Presets"); }
std::vector<Root> tuningRoots() { return roots("PF_TUNING_ROOTS", "Tunings", "/media/AkaiForce/Tunings"); }

std::string dataDir() {
    if (const char* e = std::getenv("PF_DATA_DIR")) return e;
    return pluginDir();
}

std::string resolveKey(const std::string& key, const std::vector<Root>& rs) {
    const size_t colon = key.find(':');
    if (colon == std::string::npos) return {};
    const std::string label = key.substr(0, colon), rel = key.substr(colon + 1);
    if (rel.empty() || rel.find("..") != std::string::npos) return {};   // keys come from saved state text
    for (const Root& r : rs)
        if (r.label == label) return r.dir + "/" + rel;
    return {};
}

bool writeFileAtomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f.flush()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

} // namespace pf
