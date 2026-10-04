#pragma once
// File libraries: wavetables, presets and tunings, each a process-wide listing of the files
// under its roots (plugin/paths.h) plus built-in entries. UI and worker threads only: the
// audio thread never touches a library.
//
//   key        what state, favorites and recent store: "<root label>:<relative path>"
//              ("plugin:Analog/ESW Analog - Saw.wav") or "builtin:<Name>". Never an index:
//              indices move when files are added.
//   category   the first folder under the root; deeper folders fold into it; files directly
//              in a root are "Unsorted"; same-named categories from both roots merge. The
//              built-ins have their own category, listed first; the rest A -> Z.
//   name       the file stem minus the longest " - "-terminated prefix that every file in the
//              category shares: "ESW Analog - Jupiter 8 Saw" -> "Jupiter 8 Saw".
//   flat order categories in order, names A -> Z inside each: what a knob scroll walks.
//
// A scan is a directory listing only (no file is opened): a few ms for ~400 files.
#include "paths.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace pf {

struct Entry {
    std::string key;
    std::string path;       // "" for built-ins
    std::string category;
    std::string name;
    int builtin = -1;       // index into the library's built-ins, else -1
};

struct Listing {
    std::vector<Entry> items;                  // flat order
    std::vector<std::string> categories;       // in order
    std::vector<std::vector<int>> members;     // per category: indices into items
    std::map<std::string, int> byKey;

    int find(const std::string& key) const {
        const auto it = byKey.find(key);
        return it == byKey.end() ? -1 : it->second;
    }
    int categoryOf(int item) const;            // index into categories, -1 if none
    // "Category / Name"; for a key that isn't listed (a file that went away), its file stem.
    std::string label(const std::string& key) const;
};

class FileLibrary {
public:
    struct Config {
        std::vector<std::string> exts;                  // lower case, with the dot
        std::string builtinCategory;                    // "Built-in"
        std::vector<std::string> builtinNames;          // keys become "builtin:<name>"
        std::function<std::vector<Root>()> roots;
        std::string favFile, recentFile;                // under dataDir(); "" = not kept
        size_t recentMax = 12;
    };
    explicit FileLibrary(Config cfg);

    std::shared_ptr<const Listing> listing() const;   // scans on first use
    void rescan();                              // e.g. after a preset was saved

    bool isFavorite(const std::string& key) const;
    void setFavorite(const std::string& key, bool on);   // persisted at once
    std::vector<std::string> favorites() const;          // in flat order (listed keys only)
    void touchRecent(const std::string& key);            // persisted at once
    std::vector<std::string> recent() const;             // newest first (listed keys only)

private:
    void loadLists() const;   // with mtx_ held: favorites/recent files, read on first use
    void saveList(const std::string& file, const std::vector<std::string>& keys) const;

    Config cfg_;
    mutable std::mutex mtx_;
    mutable std::shared_ptr<const Listing> listing_;
    mutable bool listsLoaded_ = false;
    mutable std::set<std::string> fav_;
    mutable std::vector<std::string> recent_;
};

// Builds a listing from explicit roots (used by FileLibrary; exposed for tests).
std::shared_ptr<const Listing> scanLibrary(const FileLibrary::Config& cfg);

FileLibrary& tableLibrary();    // *.wav; built-ins = builtinTables()
FileLibrary& presetLibrary();   // *.pfp; built-ins = the factory presets
FileLibrary& tuningLibrary();   // *.tun, *.scl; built-in = "12-TET"

} // namespace pf
