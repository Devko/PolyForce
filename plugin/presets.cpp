#include "presets.h"

#include "factory_presets.h"
#include "paths.h"
#include "../dsp/tuning.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace pf {
namespace fs = std::filesystem;

namespace {
constexpr const char* kBuiltin = "builtin:";
}

FileLibrary& presetLibrary() {
    static FileLibrary lib([] {
        FileLibrary::Config c;
        c.exts = {".pfp"};
        c.builtinCategory = "Factory";
        for (int i = 0; i < kNumFactoryPresets; ++i) c.builtinNames.push_back(kFactoryPresets[i].name);
        c.roots = presetRoots;
        c.favFile = "preset_favorites.txt";
        c.recentFile = "preset_recent.txt";
        return c;
    }());
    return lib;
}

FileLibrary& tuningLibrary() {
    static FileLibrary lib([] {
        FileLibrary::Config c;
        c.exts = {".tun", ".scl"};
        c.builtinCategory = "Built-in";
        c.builtinNames = {equalTemperament().name};
        c.roots = tuningRoots;
        return c;
    }());
    return lib;
}

bool presetText(const std::string& key, std::string& out) {
    if (key.compare(0, 8, kBuiltin) == 0) {
        const std::string name = key.substr(8);
        for (int i = 0; i < kNumFactoryPresets; ++i)
            if (name == kFactoryPresets[i].name) {
                out = kFactoryPresets[i].text;
                return true;
            }
        return false;
    }
    const std::string path = resolveKey(key, presetRoots());
    return !path.empty() && readFile(path, out);
}

std::string nextUserPreset(std::string* key) {
    const auto roots = presetRoots();
    if (roots.empty()) return {};
    const Root& r = roots.front();
    std::error_code ec;
    fs::create_directories(r.dir + "/User", ec);
    for (int n = 1; n < 10000; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "User %03d.pfp", n);
        const std::string path = r.dir + "/User/" + name;
        if (!fs::exists(path, ec)) {
            if (key) *key = r.label + ":User/" + name;
            return path;
        }
    }
    return {};
}

Loader::SlotType tuningSlotType() {
    Loader::SlotType t;
    t.fallbackKey = std::string(kBuiltin) + equalTemperament().name;
    t.load = [](const std::string& key, std::string* err, int* info) -> std::shared_ptr<const void> {
        if (info) *info = 0;
        if (key.compare(0, 8, kBuiltin) == 0) {
            if (key.substr(8) == equalTemperament().name)
                return std::shared_ptr<const Tuning>(&equalTemperament(), [](const Tuning*) {});
            if (err) *err = "no such built-in";
            return nullptr;
        }
        const std::string path = resolveKey(key, tuningRoots());
        if (path.empty()) {
            if (err) *err = "unknown folder";
            return nullptr;
        }
        auto t = std::make_shared<Tuning>();
        if (!loadTuning(path, *t, err)) return nullptr;
        return t;
    };
    return t;
}

} // namespace pf
