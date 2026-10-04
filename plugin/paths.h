#pragma once
// Where PolyForce finds its files on the device, with test overrides.
//
//   pluginDir()   the folder our own .so was loaded from ("/sdcard/Synths/Devko - VST - PolyForce"),
//                 read from /proc/self/maps (the mapping that holds this code). Not dladdr(): built
//                 against glibc >= 2.34 that binds GLIBC_2.34. "" when it can't tell (tests).
//   tableRoots()  wavetable folders, in order: <plugin dir>/Wavetables ("plugin"), then the SSD
//                 /media/AkaiForce/Wavetables ("ssd"; noexec only stops binaries). A missing folder
//                 simply contributes nothing. PF_TABLE_ROOTS (colon-separated) replaces both.
//   dataDir()     favorites.txt, recent.txt, user presets: the plugin folder. PF_DATA_DIR overrides;
//                 "" = nothing is persisted.
#include <string>
#include <vector>

namespace pf {

struct Root {
    std::string label;   // "plugin", "ssd", then "root3"... for extra test roots
    std::string dir;
};

std::string pluginDir();
std::vector<Root> tableRoots();
std::vector<Root> presetRoots();   // <plugin dir>/Presets, /media/AkaiForce/PolyForce Presets; PF_PRESET_ROOTS
std::vector<Root> tuningRoots();   // <plugin dir>/Tunings, /media/AkaiForce/Tunings; PF_TUNING_ROOTS
std::string dataDir();

// "plugin:Analog/Saw.wav" + roots -> "<dir of plugin root>/Analog/Saw.wav"; "" if the label is unknown.
std::string resolveKey(const std::string& key, const std::vector<Root>& roots);

// Writes `text` to `path` via path.new + rename (a crash never leaves half a file). False on error.
bool writeFileAtomic(const std::string& path, const std::string& text);
bool readFile(const std::string& path, std::string& out);

} // namespace pf
