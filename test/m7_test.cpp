// Milestone 7: microtuning (.tun, .scl), presets (factory, browse, step, save, init),
// randomize.
#include "host.h"
#include "../dsp/tuning.h"
#include "../plugin/library.h"
#include "../plugin/paths.h"
#include "../plugin/presets.h"
#include "factory_presets.h"

#include <filesystem>
#include <fstream>

namespace pft {
namespace {
namespace fs = std::filesystem;

void tuningFiles() {
    // .scl: 19 equal steps per octave; degree 0 on note 60.
    std::string scl = "! 19-EDO\n19 equal\n 19\n";
    for (int i = 1; i <= 19; ++i) scl += std::to_string(1200.0 * i / 19.0) + "\n";
    pf::Tuning t;
    CHECK(pf::parseScl(scl, t));
    CHECK(std::fabs(t.pitch[60] - 60.0f) < 1e-4f);
    CHECK(std::fabs(t.pitch[61] - (60.0f + 12.0f / 19.0f)) < 1e-4f);
    CHECK(std::fabs(t.pitch[79] - 72.0f) < 1e-4f);   // 19 steps up = an octave
    CHECK(std::fabs(t.pitch[41] - 48.0f) < 1e-4f);
    // Ratios: just intonation, a pure fifth on degree 4 of a 7-note scale.
    pf::Tuning j;
    CHECK(pf::parseScl("just\n7\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n", j));
    CHECK(std::fabs(j.pitch[64] - (60.0f + 12.0f * std::log2(1.5f))) < 1e-4f);
    CHECK(!pf::parseScl("x\n3\n1.0\n", j));   // fewer degrees than it says
    // .tun: absolute cents from MIDI note 0; [Exact Tuning] wins over [Tuning].
    pf::Tuning u;
    CHECK(pf::parseTun("[Tuning]\nnote 69=6900\nnote 60=5990\n[Exact Tuning]\nnote 60=6000.5\n", u));
    CHECK(std::fabs(u.pitch[69] - 69.0f) < 1e-4f && std::fabs(u.pitch[60] - 60.005f) < 1e-4f);
    CHECK(u.pitch[61] == 61.0f);   // not listed: 12-TET
    CHECK(!pf::parseTun("nothing here\n", u));
    // Files in the tuning roots are listed and load through the plugin.
    fs::create_directories(fixtureDir() + "/tunings/Xen");
    std::ofstream(fixtureDir() + "/tunings/Xen/19-EDO.scl") << scl;
    pf::tuningLibrary().rescan();
    const auto L = pf::tuningLibrary().listing();
    CHECK(L->items.size() == 2 && L->items[0].key == "builtin:12-TET");
    CHECK(L->find("plugin:Xen/19-EDO.scl") == 1);

    Host h;
    h.bare();
    CHECK(h.display(pf::P_TUNING) == "TUNING  Built-in / 12-TET");
    h.press(pf::P_TUNING_NEXT);
    CHECK(h.until([&] { return h.display(pf::P_TUNING) == "TUNING  Xen / 19-EDO"; }));
    // 19-EDO: note 61 is 12/19 st above middle C -> 261.63 * 2^(1/19) = 271.31 Hz.
    h.on(61, 127);
    h.run(kBlocksPerSec / 4);
    h.run(kBlocksPerSec);
    int rises = 0;
    for (size_t i = 1; i < h.L.size(); ++i) rises += (h.L[i - 1] < 0.0f && h.L[i] >= 0.0f) ? 1 : 0;
    CHECK(std::abs(rises - static_cast<int>(271.31 * h.L.size() / 44100.0)) <= 2);
    CHECK(h.chunk().find("tuning=plugin:Xen/19-EDO.scl\n") != std::string::npos);
    CHECK(h.load("polyforce 4\ntuning=plugin:Xen/Gone.scl\n") == 1);
    CHECK(h.until([&] { return h.display(pf::P_TUNING) == "MISSING Gone"; }));
}

void presets() {
    pf::presetLibrary().rescan();
    const auto L = pf::presetLibrary().listing();
    // The factory set comes first, in its own categories (one per presets/Factory folder), Init leading.
    CHECK(L->categories.size() >= 14 && L->categories[0] == "Templates" && L->categories[1] == "Bass");
    std::vector<int> factory;
    for (size_t i = 0; i < L->items.size(); ++i)
        if (L->items[i].builtin >= 0) factory.push_back(static_cast<int>(i));
    CHECK(factory.size() == static_cast<size_t>(pf::kNumFactoryPresets) && L->items[0].name == "Init");
    CHECK(L->members[1].size() > 1 && L->items[static_cast<size_t>(L->members[1][1])].name == "Acid Bass");
    // A folder of the user's named like a factory category stays its own category.
    {
        fs::create_directories(fixtureDir() + "/presets/Bass");
        std::ofstream(fixtureDir() + "/presets/Bass/Mine.pfp") << "polyforce 4\nvolume=-7\n";
        pf::presetLibrary().rescan();
        const auto U = pf::presetLibrary().listing();
        const int mine = U->find("plugin:Bass/Mine.pfp");
        CHECK(mine >= 0 && U->items[static_cast<size_t>(mine)].category == "Bass (files)");
        CHECK(U->label("plugin:Bass/Mine.pfp") == "Bass (files) / Mine" && U->label("builtin:Acid Bass") == "Bass / Acid Bass");
        fs::remove_all(fixtureDir() + "/presets/Bass");
        pf::presetLibrary().rescan();
    }

    // Every factory preset loads its tables (built-ins, all there) and plays something finite and audible.
    for (int m : factory) {
        Host h;
        std::string text;
        CHECK(pf::presetText(L->items[static_cast<size_t>(m)].key, text));
        CHECK(h.load(text) == 1);
        CHECK(h.until([&] {
            return h.display(pf::P_O1_TABLE).rfind("LOADING", 0) != 0 && h.display(pf::P_O2_TABLE).rfind("LOADING", 0) != 0;
        }, 20000));
        const bool found = h.display(pf::P_O1_TABLE).rfind("MISSING", 0) != 0 && h.display(pf::P_O2_TABLE).rfind("MISSING", 0) != 0;
        if (!found) std::printf("  preset %s: a table is missing\n", L->items[static_cast<size_t>(m)].name.c_str());
        CHECK(found);
        for (int n : {48, 55, 60, 64}) h.on(n, 110);
        const float peak = h.run(kBlocksPerSec);
        if (!(h.finite && peak > 1e-3f && peak < 4.0f))
            std::printf("  preset %s: peak %g finite %d\n", L->items[static_cast<size_t>(m)].name.c_str(), peak, h.finite);
        CHECK(h.finite && peak > 1e-3f && peak < 4.0f);
    }

    Host h;
    // The preset stepper walks the list; a preset starts from the defaults.
    h.set(pf::P_F2_RES, 0.9f);
    h.press(pf::P_PRESET_NEXT);   // no preset yet: the first
    CHECK(h.display(pf::P_PRESET) == "PRESET  Templates / Init");
    h.press(pf::P_PRESET_NEXT);
    CHECK(h.display(pf::P_PRESET) == "PRESET  Templates / Init Mono");
    CHECK(h.value(pf::P_VMODE) == pf::VM_LEGATO && h.display(pf::P_F2_RES) == "25%");
    CHECK(h.chunk().find("preset=builtin:Init Mono\n") != std::string::npos);
    // INIT: everything back to the defaults.
    h.press(pf::P_PRE_INIT);
    CHECK(h.value(pf::P_VMODE) == pf::VM_POLY && h.display(pf::P_PRESET) == "PRESET  Templates / Init");

    // The browser in PRESETS mode: tap a preset tile to load it.
    h.setN(pf::P_BR_TARGET, 1.0f);
    h.run(2);
    CHECK(h.display(pf::P_BR_NOW) == "PRESET  Templates / Init");
    CHECK(h.get(pf::P_CAT_3) == 1.0f && h.display(pf::P_CAT_3) == "TEMPLATES");
    CHECK(h.display(pf::P_CAT_4) == "BASS");
    h.setN(pf::P_CAT_4, 1.0f);
    CHECK(h.get(pf::P_CAT_4) == 1.0f && h.display(pf::P_TBL_2) == "Acid Bass");
    h.setN(pf::P_TBL_2, 1.0f);
    CHECK(h.display(pf::P_PRESET) == "PRESET  Bass / Acid Bass" && h.value(pf::P_ENGINE) == pf::EN_DIRTY);
    CHECK(h.get(pf::P_TBL_2) == 1.0f);
    // Favorite presets have their own list.
    h.setN(pf::P_FAV, 1.0f);
    CHECK(pf::presetLibrary().isFavorite("builtin:Acid Bass"));

    // SAVE: a numbered user preset in the first preset root, listed and current at once.
    h.set(pf::P_F1_CUT, 777.0f);
    h.press(pf::P_PRE_SAVE);
    CHECK(fs::exists(fixtureDir() + "/presets/User/User 001.pfp"));
    CHECK(h.display(pf::P_PRESET) == "PRESET  User / User 001");
    h.press(pf::P_PRE_SAVE);
    CHECK(fs::exists(fixtureDir() + "/presets/User/User 002.pfp"));
    Host b;
    std::string text;
    CHECK(pf::presetText("plugin:User/User 001.pfp", text) && b.load(text) == 1);
    CHECK(b.display(pf::P_F1_CUT) == "777 Hz" && b.value(pf::P_ENGINE) == pf::EN_DIRTY);
    CHECK(text.find("preset=") == std::string::npos);   // a preset doesn't point at itself

    // RANDOM: the sound moves, volume and voicing don't, and it still plays.
    Host r;
    r.set(pf::P_RAND_AMT, 1.0f);
    const std::string before = r.chunk();
    r.press(pf::P_PRE_RAND);
    CHECK(r.chunk() != before);
    CHECK(r.display(pf::P_VOLUME) == "-6.0 dB" && r.value(pf::P_VOICES) == pf::kParamMaxVoices);
    CHECK(r.display(pf::P_PRESET) == "PRESET  -");
    for (int k = 0; k < 20; ++k) r.press(pf::P_PRE_RAND);
    const float a1 = r.value(pf::P_E1_A);
    CHECK(a1 <= 0.15f);   // attacks stay playable
    for (int n : {48, 60, 72}) r.on(n, 120);
    CHECK(r.run(kBlocksPerSec) < 4.0f && r.finite);
    r.set(pf::P_RAND_AMT, 0.0f);
    const std::string still = r.chunk();
    r.press(pf::P_PRE_RAND);
    CHECK(r.chunk().find("o1_pos=") != std::string::npos);
    CHECK(still.substr(0, still.find("preset")) == r.chunk().substr(0, r.chunk().find("preset")));   // amount 0: no change
}

} // namespace

void patchTests() {
    tuningFiles();
    presets();
}

} // namespace pft
