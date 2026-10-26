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
//              built-ins have their own category (or categories), listed first; the rest
//              A -> Z; a folder named like a built-in category shows as "<name> (files)".
//   name       the file stem minus the longest " - "-terminated prefix that every file in the
//              category shares: "ESW Analog - Jupiter 8 Saw" -> "Jupiter 8 Saw".
//   flat order categories in order, names A -> Z inside each: what a knob scroll walks.
//
// A scan is a directory listing only (no file is opened): a few ms for ~400 files.
//
// Favorites and Recent change in memory at once and reach the data folder later, from flush():
// a preset scroll touches Recent on every step, and a write (with its fsync) on MPC's UI thread
// per step stalled the scroll.
#include "paths.h"

#include <chrono>
#include <cstdint>
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
        // Per built-in, its category instead of builtinCategory (empty: all in builtinCategory).
        // Categories keep the order they first appear in, names their order inside each.
        std::vector<std::string> builtinCategories;
        std::function<std::vector<Root>()> roots;
        std::string favFile, recentFile;                // under dataDir(); "" = not kept
        size_t recentMax = 12;
        int saveDelayMs = 1000;                         // flush(): a list saves once it held still this long
    };
    explicit FileLibrary(Config cfg);

    std::shared_ptr<const Listing> listing() const;   // scans on first use
    void rescan();                              // e.g. after a preset was saved

    bool isFavorite(const std::string& key) const;
    void setFavorite(const std::string& key, bool on);   // saved by flush()
    std::vector<std::string> favorites() const;          // in flat order (listed keys only)
    void touchRecent(const std::string& key);            // saved by flush()
    std::vector<std::string> recent() const;             // newest first (listed keys only)
    // Writes the lists that changed: those unchanged for saveDelayMs, or all of them (`now`).
    // Touches the disk: the loaders' workers call it every round, an instance's close with `now`.
    void flush(bool now);

private:
    // A list's changes not yet handed to saveList (under mtx_).
    struct Unsaved {
        uint64_t gen = 0, queued = 0;                       // its last change; the last one handed to a save
        std::chrono::steady_clock::time_point at{};         // when it last changed
    };
    void loadLists() const;   // with mtx_ held: favorites/recent files, read on first use
    void changed(Unsaved& u);   // with mtx_ held
    void saveList(const std::string& file, const std::vector<std::string>& keys, uint64_t gen) const;

    Config cfg_;
    mutable std::mutex mtx_;
    mutable std::shared_ptr<const Listing> listing_;
    mutable bool listsLoaded_ = false;
    uint64_t gen_ = 0;                          // list changes, under mtx_
    Unsaved favUnsaved_, recentUnsaved_;
    mutable std::mutex saveMtx_;                // one save at a time...
    mutable uint64_t savedFav_ = 0, savedRecent_ = 0;   // ...never an older list over a newer one
    mutable std::set<std::string> fav_;
    mutable std::vector<std::string> recent_;
};

// Builds a listing from explicit roots (used by FileLibrary; exposed for tests).
std::shared_ptr<const Listing> scanLibrary(const FileLibrary::Config& cfg);

FileLibrary& tableLibrary();    // *.wav; built-ins = dsp/wavetable.h builtinName()
FileLibrary& presetLibrary();   // *.pfp; built-ins = the factory presets
FileLibrary& tuningLibrary();   // *.tun, *.scl; built-in = "12-TET"

} // namespace pf
