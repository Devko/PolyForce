// Milestone 1: the faster import, the table library, the loader, the browser page and the
// one-step-per-event stepping. Fixtures are wavetable WAVs written here, so nothing depends
// on third-party tables.
#include "host.h"
#include "../dsp/wavetable.h"
#include "../plugin/library.h"
#include "../plugin/loader.h"
#include "../plugin/paths.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>

namespace pft {
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Frame f of a fixture table: harmonics 1..1+3f (capped below frameSize/2) at 1/h, phases
// varying with h and f. Band-limited, so a correct import reproduces it exactly.
double fixtureSample(int f, int i, int frameSize) {
    const int top = std::min(1 + 3 * f, frameSize / 2 - 1);
    double x = 0.0;
    for (int h = 1; h <= top; ++h)
        x += std::sin(2.0 * kPi * h * i / frameSize + 0.37 * h + 0.11 * f) / h;
    return x;
}

void put16(std::string& b, uint16_t v) { b += static_cast<char>(v & 0xFF); b += static_cast<char>(v >> 8); }
void put32(std::string& b, uint32_t v) { put16(b, static_cast<uint16_t>(v & 0xFFFF)); put16(b, static_cast<uint16_t>(v >> 16)); }

std::string fx(const std::string& rel) { return fixtureDir() + "/" + rel; }

void makeFixtures() {
    static bool done = false;
    if (done) return;
    done = true;
    writeTable(fx("plugin/Analog/ESW Analog - Saw.wav"), 8);
    writeTable(fx("plugin/Analog/ESW Analog - Square.wav"), 4);
    writeTable(fx("ssd/Analog/ESW Analog - Pulse.wav"), 16, 2048, false, false);   // 16-bit, no marker
    writeTable(fx("plugin/Digital/Bells.wav"), 256);
    writeTable(fx("plugin/Digital/Deep/Nested.wav"), 3);
    writeTable(fx("plugin/loose.wav"), 1, 1024);
    writeTable(fx("plugin/.hidden.wav"), 1);
    writeTable(fx("plugin/Analog/._Saw.wav"), 1);
    std::ofstream(fx("plugin/Analog/readme.txt")) << "not a table";
    std::ofstream(fx("plugin/broken.wav")) << "RIFF....WAVEjunk";
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void writeTable(const std::string& path, int frames, int frameSize, bool float32, bool marker) {
    fs::create_directories(fs::path(path).parent_path());
    std::vector<double> x;
    double peak = 0.0;
    for (int f = 0; f < frames; ++f)
        for (int i = 0; i < frameSize; ++i) {
            x.push_back(fixtureSample(f, i, frameSize));
            peak = std::max(peak, std::fabs(x.back()));
        }
    std::string data;
    for (double v : x) {
        const double s = 0.5 * v / peak;
        if (float32) {
            float fv = static_cast<float>(s);
            uint32_t u;
            std::memcpy(&u, &fv, 4);
            put32(data, u);
        } else {
            put16(data, static_cast<uint16_t>(static_cast<int16_t>(std::lround(s * 32767.0))));
        }
    }
    std::string b = "RIFF";
    std::string body = "WAVE";
    body += "fmt ";
    put32(body, 16);
    put16(body, float32 ? 3 : 1);
    put16(body, 1);
    put32(body, 44100);
    put32(body, 44100u * (float32 ? 4 : 2));
    put16(body, float32 ? 4 : 2);
    put16(body, float32 ? 32 : 16);
    if (marker) {
        const std::string clm = "<!>" + std::to_string(frameSize) + " 10000000 wavetable (www.xferrecords.com)";
        body += "clm ";
        put32(body, static_cast<uint32_t>(clm.size()));
        body += clm;
        if (clm.size() & 1) body += '\0';
    }
    body += "data";
    put32(body, static_cast<uint32_t>(data.size()));
    body += data;
    put32(b, static_cast<uint32_t>(body.size()));
    b += body;
    std::ofstream(path, std::ios::binary) << b;
}

// --- import -----------------------------------------------------------------------------------

void tablesTests() {
    makeFixtures();
    // The classic shapes: one frame each, the pulse table 32 frames.
    CHECK(pf::classicTable(pf::CW_SINE).frames == 1);
    CHECK(pf::classicTable(pf::CW_PULSE).frames == 32);
    CHECK(pf::classicTable(pf::CW_SAW).name == "Saw");

    // An imported table reproduces its band-limited input at level 0, frames paired or not.
    for (int frames : {7, 8}) {
        const std::string path = fx("import_" + std::to_string(frames) + ".wav");
        writeTable(path, frames);
        pf::Wavetable t;
        std::string err;
        CHECK(pf::loadWavetable(path, t, &err));
        CHECK(t.frames == frames);
        CHECK(t.data.size() == static_cast<size_t>(frames) * pf::kFrameStride);
        double peak = 0.0;
        for (int f = 0; f < frames; ++f)
            for (int i = 0; i < 2048; ++i) peak = std::max(peak, std::fabs(fixtureSample(f, i, 2048)));
        double worst = 0.0;
        bool guards = true;
        for (int f = 0; f < frames; ++f) {
            const float* m0 = t.get(f, 0);
            for (int i = 0; i < 2048; ++i) worst = std::max(worst, std::fabs(m0[i] - fixtureSample(f, i, 2048) / peak));
            for (int k = 0; k < pf::kMipLevels; ++k) guards = guards && t.get(f, k)[pf::mipLength(k)] == t.get(f, k)[0];
        }
        CHECK(worst < 1e-4);   // float32 input, exact spectrum
        CHECK(guards);
        // The last frame of an odd count (no partner) and a paired one alike: level 3 keeps
        // harmonics 1..128 at 1024 samples = every other sample of level 0 for these frames.
        const int f = frames - 1;
        double lvErr = 0.0;
        for (int i = 0; i < 1024; ++i) lvErr = std::max(lvErr, static_cast<double>(std::fabs(t.get(f, 3)[i] - t.get(f, 0)[2 * i])));
        CHECK(lvErr < 1e-4);
    }

    // A 1024-sample frame (Serum marker) plays the same cycle from the 2048-sample level.
    {
        pf::Wavetable t;
        CHECK(pf::loadWavetable(fx("plugin/loose.wav"), t));
        double peak = 0.0, worst = 0.0;
        for (int i = 0; i < 1024; ++i) peak = std::max(peak, std::fabs(fixtureSample(0, i, 1024)));
        for (int i = 0; i < 1024; ++i) worst = std::max(worst, std::fabs(t.get(0, 0)[2 * i] - fixtureSample(0, i, 1024) / peak));
        CHECK(t.frames == 1 && worst < 1e-4);
    }
    // 16-bit PCM without the marker: 2048-sample frames assumed, quantisation-level error.
    {
        pf::Wavetable t;
        CHECK(pf::loadWavetable(fx("ssd/Analog/ESW Analog - Pulse.wav"), t));
        CHECK(t.frames == 16);
    }
    // The importer refuses what it can't use instead of guessing.
    pf::Wavetable junk;
    std::string err;
    CHECK(!pf::loadWavetable(fx("plugin/broken.wav"), junk, &err) && !err.empty());

    // Cost of a full-size table (x86 numbers: relative only; the Force is ~10x slower).
    pf::Wavetable big;
    const double t0 = nowMs();
    CHECK(pf::loadWavetable(fx("plugin/Digital/Bells.wav"), big));
    const double ms = nowMs() - t0;
    std::printf("  256-frame import: %.0f ms, %.1f MB (was 22 MB)\n", ms, static_cast<double>(big.bytes()) / (1 << 20));
    CHECK(big.frames == 256);
    CHECK(big.bytes() < 10u << 20);   // design target: under 10 MB
}

// --- library ----------------------------------------------------------------------------------

void libraryTests() {
    makeFixtures();
    pf::FileLibrary& lib = pf::tableLibrary();
    lib.rescan();
    const auto L = lib.listing();
    const std::vector<std::string> cats = {"Built-in", "Analog", "Digital", "Unsorted"};
    CHECK(L->categories == cats);
    CHECK(L->items.size() == static_cast<size_t>(pf::builtinCount()) + 3 + 2 + 2);   // hidden files, macOS junk and readme.txt ignored
    CHECK(L->items[0].key == "builtin:Classic" && L->items[3].key == "builtin:Formant");
    // Analog merges both roots; the shared "ESW Analog - " prefix is dropped; names A -> Z.
    std::vector<std::string> analog;
    for (int m : L->members[1]) analog.push_back(L->items[static_cast<size_t>(m)].name);
    CHECK((analog == std::vector<std::string>{"Pulse", "Saw", "Square"}));
    const int pulse = L->find("ssd:Analog/ESW Analog - Pulse.wav");
    CHECK(pulse == pf::builtinCount());
    CHECK(L->label("plugin:Analog/ESW Analog - Saw.wav") == "Analog / Saw");
    CHECK(L->label("plugin:Gone/Lost Table.wav") == "Lost Table");   // not listed: its stem
    CHECK(L->find("plugin:Digital/Deep/Nested.wav") >= 0);           // deeper folders fold in
    CHECK(L->items[static_cast<size_t>(L->find("plugin:Digital/Deep/Nested.wav"))].category == "Digital");
    // Keys resolve back to the files, and refuse to leave their root.
    CHECK(pf::resolveKey("ssd:Analog/ESW Analog - Pulse.wav", pf::tableRoots()) == fx("ssd/Analog/ESW Analog - Pulse.wav"));
    CHECK(pf::resolveKey("plugin:../escape.wav", pf::tableRoots()).empty());
    CHECK(pf::resolveKey("nowhere:x.wav", pf::tableRoots()).empty());

    // Favorites and recent persist in the data folder and come back in a new library.
    pf::FileLibrary::Config cfg;
    cfg.exts = {".wav"};
    cfg.builtinCategory = "Built-in";
    cfg.roots = pf::tableRoots;
    cfg.favFile = "fav_test.txt";
    cfg.recentFile = "recent_test.txt";
    {
        pf::FileLibrary a(cfg);
        a.setFavorite("plugin:Analog/ESW Analog - Saw.wav", true);
        a.setFavorite("plugin:loose.wav", true);
        a.setFavorite("plugin:loose.wav", false);
        for (int i = 0; i < 14; ++i) a.touchRecent("plugin:Digital/Bells.wav");   // repeats count once
        a.touchRecent("plugin:loose.wav");
    }
    pf::FileLibrary b(cfg);
    CHECK(b.isFavorite("plugin:Analog/ESW Analog - Saw.wav") && !b.isFavorite("plugin:loose.wav"));
    CHECK((b.recent() == std::vector<std::string>{"plugin:loose.wav", "plugin:Digital/Bells.wav"}));
    for (int i = 0; i < 20; ++i) b.touchRecent("k" + std::to_string(i));
    pf::FileLibrary c(cfg);
    CHECK(c.recent().empty());   // 12 newest kept, none of them listed tables
}

// --- stepping ---------------------------------------------------------------------------------

void steppingTests() {
    Host h;
    // A two-option choice moves one whole option per Q-Link detent (1/128), and the snapped
    // value goes back to MPC.
    CHECK(h.get(pf::P_ROUTING) == 0.0f);
    h.setN(pf::P_ROUTING, 1.0f / 128.0f);
    CHECK(h.get(pf::P_ROUTING) == 1.0f);
    h.run(2);
    CHECK(h.log.automated.count(pf::P_ROUTING) && h.log.automated[pf::P_ROUTING] == 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(320));   // a new gesture
    h.setN(pf::P_ROUTING, 0.0f);                                     // a tap on the first option
    CHECK(h.get(pf::P_ROUTING) == 0.0f);
    // A small whole number: one step per detent from wherever MPC thinks it is.
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    h.setN(pf::P_VOICES, 1.0f - 1.0f / 128.0f);
    CHECK(h.value(pf::P_VOICES) == pf::kParamMaxVoices - 1);
    // Continuous parameters follow MPC's value exactly; the status line ignores sets.
    h.setN(pf::P_F1_RES, 0.4321f);
    CHECK(h.get(pf::P_F1_RES) == 0.4321f);
    h.setN(pf::P_STATUS, 1.0f);
    CHECK(h.get(pf::P_STATUS) == 0.0f);
    CHECK(!h.e->dispatcher(h.e, vst::effCanBeAutomated, pf::P_TBL_1, 0, nullptr, 0.0f));
    CHECK(h.e->dispatcher(h.e, vst::effCanBeAutomated, pf::P_F1_CUT, 0, nullptr, 0.0f));
}

// --- loader -----------------------------------------------------------------------------------

void loaderTests() {
    makeFixtures();
    pf::tableLibrary().rescan();
    const std::string saw = "plugin:Analog/ESW Analog - Saw.wav";
    {
        Host h;
        CHECK(h.display(pf::P_O1_TABLE) == "Built-in / Classic");
        CHECK(h.display(pf::P_O1_POS) == "FRAME 11 / 16");   // 0.66 of 16 frames
        h.press(pf::P_O1_TABLE_NEXT);
        const std::string now = h.display(pf::P_O1_TABLE);   // debounced: not yet (unless this machine stalled 150 ms)
        CHECK(now == "LOADING Built-in / PWM" || now == "Built-in / PWM");
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Built-in / PWM"; }));
        h.press(pf::P_O1_TABLE_PREV);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Built-in / Classic"; }));

        // A file table by state: loads off the audio thread, shows its frame count.
        CHECK(h.load("polyforce 3\no1_table=" + saw + "\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Analog / Saw"; }));
        CHECK(h.display(pf::P_O1_POS) == "FRAME 6 / 8");
        CHECK(h.chunk().find("o1_table=" + saw + "\n") != std::string::npos);
        h.on(60);
        CHECK(h.run(20) > 1e-3f);
        CHECK(h.finite);
        CHECK(h.log.updates > 0);   // the new texts were announced

        // Missing and broken files: the fallback plays, the reference is kept.
        CHECK(h.load("polyforce 3\no1_table=" + saw + "\no2_table=plugin:Analog/Gone.wav\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O2_TABLE) == "MISSING Gone"; }));
        CHECK(h.display(pf::P_STATUS) == "OSC 2 MISSING Gone");
        CHECK(h.chunk().find("o2_table=plugin:Analog/Gone.wav\n") != std::string::npos);
        h.on(64);
        CHECK(h.run(10) > 1e-3f);
        CHECK(h.load("polyforce 3\no2_table=plugin:broken.wav\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O2_TABLE) == "MISSING Unsorted / broken"; }));
        // Older states: version 2 picked a built-in by index.
        CHECK(h.load("polyforce 2\no1_wave=2\no2_wave=3\n") == 1);
        CHECK(h.until([&] {
            return h.display(pf::P_O1_TABLE) == "Built-in / Sync" && h.display(pf::P_O2_TABLE) == "Built-in / Formant";
        }));
    }
    // A project reload in a fresh instance restores both tables.
    {
        Host a;
        CHECK(a.load("polyforce 3\no1_table=" + saw + "\no2_table=ssd:Analog/ESW Analog - Pulse.wav\n") == 1);
        CHECK(a.until([&] { return a.display(pf::P_O2_TABLE) == "Analog / Pulse"; }));
        const std::string state = a.chunk();
        Host b;
        CHECK(b.load(state) == 1);
        CHECK(b.until([&] {
            return b.display(pf::P_O1_TABLE) == "Analog / Saw" && b.display(pf::P_O2_TABLE) == "Analog / Pulse";
        }));
    }
    // Scrolling through ten tables loads only the one the scroll stops on (150 ms debounce):
    // detents 30 ms apart, slow enough for the loader to load (or build) every one if it didn't
    // wait. From the fourth-last built-in: three built-ins and six files pass, then "loose".
    {
        Host h;
        const std::string from = pf::builtinName(pf::builtinCount() - 4);
        CHECK(h.load("polyforce 4\no1_table=builtin:" + from + "\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Built-in / " + from; }));
        const auto before = pf::tableLibrary().recent();
        // A Q-Link spin: MPC sends its own running value + 1/128 per detent, 10 detents.
        const float base = h.get(pf::P_O1_TABLE);
        auto last = std::chrono::steady_clock::now();
        auto maxGap = std::chrono::steady_clock::duration::zero();   // a stalled test machine
        for (int k = 1; k <= 10; ++k) {
            h.setN(pf::P_O1_TABLE, base + static_cast<float>(k) / 128.0f);
            const auto now = std::chrono::steady_clock::now();
            if (k > 1) maxGap = std::max(maxGap, now - last);
            last = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Unsorted / loose"; }));
        const auto after = pf::tableLibrary().recent();
        CHECK(!after.empty() && after.front() == "plugin:loose.wav");
        int fresh = 0;   // file tables loaded by this scroll
        for (const std::string& k : after)
            fresh += std::find(before.begin(), before.end(), k) == before.end() ? 1 : 0;
        // A detent more than the debounce apart is a real stop: then that table loads too.
        if (maxGap < std::chrono::milliseconds(140)) CHECK(fresh == 1);
        else CHECK(fresh >= 1);
    }
    // The handoff under ASan: tables swap and get freed while another thread renders.
    {
        Host h;
        pf::TableCache::get().setCap(1);   // evict every table nobody plays: real frees happen
        for (int n : {48, 55, 60, 67}) h.on(n);
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            float L[kBlock], R[kBlock];
            float* out[2] = {L, R};
            while (!stop.load()) h.e->processReplacing(h.e, nullptr, out, kBlock);
        });
        const std::string keys[] = {saw, "plugin:Analog/ESW Analog - Square.wav", "ssd:Analog/ESW Analog - Pulse.wav",
                                    "plugin:Digital/Deep/Nested.wav"};
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        for (int i = 0; std::chrono::steady_clock::now() < end; ++i) {
            h.load("polyforce 3\no1_table=" + keys[i % 4] + "\no2_table=" + keys[(i + 1) % 4] + "\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        stop = true;
        audio.join();
        pf::TableCache::get().setCap(96u << 20);
        // Reaching here under ASan is half the check; the swaps really happened is the other.
        CHECK(h.until([&] {
            const std::string a = h.display(pf::P_O1_TABLE);
            return a.compare(0, 8, "LOADING ") != 0 && a.compare(0, 9, "Built-in ") != 0;
        }));
    }
}

// --- browser ----------------------------------------------------------------------------------

void browserTests() {
    makeFixtures();
    pf::tableLibrary().rescan();
    const std::string saw = "plugin:Analog/ESW Analog - Saw.wav";
    Host h;
    auto catTile = [&](const std::string& name) {
        for (int i = 0; i < pf::kBrowserCats; ++i)
            if (h.display(pf::P_CAT_1 + i) == name) return pf::P_CAT_1 + i;
        return -1;
    };
    CHECK(h.display(pf::P_CAT_1) == "FAVORITES" && h.display(pf::P_CAT_2) == "RECENT");
    CHECK(h.get(catTile("BUILT-IN")) == 1.0f);   // follows OSC 1's table: Built-in / Classic
    CHECK(h.display(pf::P_TBL_1) == "Classic" && h.get(pf::P_TBL_1) == 1.0f);
    CHECK(h.display(pf::P_TBL_PAGE) == "PAGE 1 / " + std::to_string((pf::builtinCount() + pf::kBrowserItems - 1) / pf::kBrowserItems));

    // A table loaded from elsewhere: the browser follows it to its category, its tile lit,
    // and MPC hears about the lit tile through audioMasterAutomate.
    CHECK(h.load("polyforce 3\no1_table=" + saw + "\n") == 1);
    CHECK(h.get(pf::P_TBL_2) == 1.0f);   // lit at once (before the table has even loaded)
    CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Analog / Saw"; }));
    CHECK(h.get(catTile("ANALOG")) == 1.0f);
    CHECK(h.display(pf::P_TBL_1) == "Pulse" && h.display(pf::P_TBL_2) == "Saw" && h.display(pf::P_TBL_3) == "Square");
    CHECK(h.display(pf::P_TBL_4).empty());
    CHECK(h.log.automated[pf::P_TBL_2] == 1.0f);
    CHECK(h.display(pf::P_BR_NOW) == "OSC 1  Analog / Saw  8 FR");

    // A tap on a table loads it into the target; the release echo a moment later is ignored.
    h.setN(pf::P_TBL_1, 1.0f);
    h.setN(pf::P_TBL_1, 0.0f);
    CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Analog / Pulse"; }));
    CHECK(h.get(pf::P_TBL_1) == 1.0f && h.get(pf::P_TBL_2) == 0.0f);

    // Another category: its tables fill the tiles; nothing loads until one is tapped.
    const int digital = catTile("DIGITAL");
    h.setN(digital, 1.0f);
    h.run(2);
    CHECK(h.get(digital) == 1.0f);
    CHECK(h.display(pf::P_TBL_1) == "Bells" && h.display(pf::P_TBL_2) == "Nested");
    CHECK(h.get(pf::P_TBL_1) == 0.0f);
    CHECK(h.display(pf::P_O1_TABLE) == "Analog / Pulse");

    // Favorite: lit for the current table, listed under FAVORITES, saved in the data folder.
    h.setN(pf::P_FAV, 1.0f);
    CHECK(h.get(pf::P_FAV) == 1.0f);
    std::string favs;
    CHECK(pf::readFile(fixtureDir() + "/data/favorites.txt", favs) && favs.find("ssd:Analog/ESW Analog - Pulse.wav") != std::string::npos);
    h.setN(pf::P_CAT_1, 1.0f);
    h.run(2);
    CHECK(h.display(pf::P_TBL_1) == "Pulse" && h.get(pf::P_TBL_1) == 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // past the echo window
    h.setN(pf::P_FAV, 0.0f);
    CHECK(h.get(pf::P_FAV) == 0.0f);

    // Random: another table of the browsed category, never the current one.
    h.setN(catTile("ANALOG"), 1.0f);
    for (int i = 0; i < 5; ++i) {
        const std::string before = h.display(pf::P_O1_TABLE);
        h.press(pf::P_RND);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE).rfind("Analog / ", 0) == 0 && h.display(pf::P_O1_TABLE) != before; }));
    }
    // Copy and swap between the oscillators.
    h.press(pf::P_COPY);
    CHECK(h.until([&] { return h.display(pf::P_O2_TABLE) == h.display(pf::P_O1_TABLE); }));
    CHECK(h.load("polyforce 3\no1_table=" + saw + "\no2_table=builtin:Sync\n") == 1);
    CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Analog / Saw"; }));
    h.press(pf::P_SWAP);
    CHECK(h.until([&] {
        return h.display(pf::P_O1_TABLE) == "Built-in / Sync" && h.display(pf::P_O2_TABLE) == "Analog / Saw";
    }));
    // The target switch: the browser now follows OSC 2.
    h.setN(pf::P_BR_TARGET, 0.5f);   // OSC 1, OSC 2, PRESETS
    h.run(2);
    CHECK(h.get(catTile("ANALOG")) == 1.0f && h.get(pf::P_TBL_2) == 1.0f);
    CHECK(h.display(pf::P_BR_NOW).rfind("OSC 2  Analog / Saw", 0) == 0);
    // Momentary buttons spring back to 0 on MPC's side.
    CHECK(h.log.automated.count(pf::P_SWAP) && h.log.automated[pf::P_SWAP] == 0.0f);
    CHECK(h.finite);
}

// The wave view (OSC tab, WAVES): the plugin sets each oscillator's kWaveCols meters to its current frame
// (table, wave and position knob) while the page shows, tells MPC, keeps them when MPC writes to them, and
// saves none of it.
void waveViewTests() {
    auto col = [](Host& h, int o, int c) { return 2.0f * h.get((o ? pf::P_O2_WV01 : pf::P_O1_WV01) + c) - 1.0f; };
    // A sine: up in the first half, down in the second, the peaks on the quarters.
    {
        Host h;
        h.set(pf::P_UI_OSC, 3);   // WAVES
        CHECK(h.display(pf::P_UI_OSC) == "WAVES");
        h.set(pf::P_O1_WAVE, pf::OW_SINE);
        h.run(4);
        CHECK(col(h, 0, 11) > 0.95f && col(h, 0, 12) > 0.95f && col(h, 0, 35) < -0.95f && col(h, 0, 36) < -0.95f);
        bool shape = std::fabs(col(h, 0, 0)) < 0.2f;
        for (int c = 1; c < 23; ++c) shape = shape && col(h, 0, c) > 0.0f;
        for (int c = 25; c < pf::kWaveCols - 1; ++c) shape = shape && col(h, 0, c) < 0.0f;
        CHECK(shape);
        CHECK(h.log.automated.count(pf::P_O1_WV01 + 11) == 1);   // pushed to MPC
        CHECK(h.display(pf::P_O1_WV01).empty());
    }
    // The position knob moves it: a pulse at 50% width is up half the cycle, at 3% only briefly.
    {
        Host h;
        h.set(pf::P_UI_OSC, 3);
        h.set(pf::P_O2_WAVE, pf::OW_PULSE);
        h.set(pf::P_O2_POS, 0.0f);
        h.run(4);
        auto high = [&] {
            int k = 0;
            for (int c = 0; c < pf::kWaveCols; ++c) k += col(h, 1, c) > 0.0f ? 1 : 0;
            return k;
        };
        const int wide = high();
        h.set(pf::P_UI_OSC, 1);   // another page: the view holds still, MPC hears nothing
        const int pushes = h.log.automateCount[pf::P_O2_WV01 + 20];
        h.set(pf::P_O2_POS, 1.0f);
        h.run(4);
        CHECK(high() == wide && h.log.automateCount[pf::P_O2_WV01 + 20] == pushes);
        h.set(pf::P_UI_OSC, 3);   // back: it catches up
        h.run(2);
        CHECK(wide >= 18 && wide <= 30 && high() <= 4);
    }
    // A newly loaded table shows; MPC writing a meter (a touch) doesn't move it; nothing of it is saved or
    // automatable; the page is the fourth of the OSC tab.
    {
        Host h;
        h.set(pf::P_UI_OSC, 3);
        h.run(4);
        std::vector<float> before(pf::kWaveCols);
        for (int c = 0; c < pf::kWaveCols; ++c) before[static_cast<size_t>(c)] = col(h, 0, c);
        CHECK(h.load("polyforce 4\no1_table=builtin:Sync\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Built-in / Sync"; }));
        int moved = 0;
        for (int c = 0; c < pf::kWaveCols; ++c) moved += std::fabs(col(h, 0, c) - before[static_cast<size_t>(c)]) > 0.05f;
        CHECK(moved >= 10);
        const int k = pf::P_O1_WV01 + 20;
        const float v = h.get(k);
        const int pushes = h.log.automateCount[k];
        h.setN(k, v > 0.5f ? 0.0f : 1.0f);
        h.run(2);
        CHECK(h.get(k) == v && h.log.automateCount[k] == pushes + 1 && h.log.automated[k] == v);
        CHECK(h.e->dispatcher(h.e, vst::effCanBeAutomated, k, 0, nullptr, 0.0f) == 0);
        CHECK(h.chunk().find("_wv") == std::string::npos);
    }
}

} // namespace pft
