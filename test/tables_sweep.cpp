// Every wavetable WAV under a folder (default ../wavetables): loads it, checks the result,
// plays a chord through the engine sweeping the whole table, and reports load cost and
// memory. Optimised, no sanitizers (a few hundred tables x 256 frames of FFTs):
//   make test-tables [WAVETABLES=<dir>]
#include "../dsp/synth.h"
#include "../dsp/wavetable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : "../wavetables";
    if (!fs::is_directory(root)) {
        std::printf("no folder %s: nothing to do\n", root.string().c_str());
        return 0;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (e.is_regular_file() && ext == ".wav") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    int ok = 0, failed = 0, frames = 0;
    double totalMs = 0.0, worstMs = 0.0, perFrameUs = 0.0;
    size_t worstBytes = 0;
    std::string worstName;
    for (const auto& path : files) {
        pf::Wavetable t;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool loaded = pf::loadWavetable(path.string(), t, &err);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const std::string rel = fs::relative(path, root).string();
        if (!loaded) {
            std::printf("FAIL load  %s: %s\n", rel.c_str(), err.c_str());
            ++failed;
            continue;
        }

        // the table itself: peak 1, finite, guard samples in place
        float peak = 0.0f;
        bool sane = true;
        for (int f = 0; f < t.frames; ++f)
            for (int k = 0; k < pf::kMipLevels; ++k) {
                const float* m = t.get(f, k);
                sane = sane && m[pf::kTableSize] == m[0];
                for (int i = 0; i < pf::kTableSize; ++i) {
                    sane = sane && std::isfinite(m[i]);
                    if (k == 0) peak = std::max(peak, std::fabs(m[i]));
                }
            }
        sane = sane && peak > 0.999f && peak < 1.001f;

        // ...and through the engine: env 2 sweeps the position across every frame
        pf::Synth s;
        pf::Patch p;
        p.osc[0].table = &t;
        p.osc[0].pos = 0.0f;
        p.osc[0].unison = 4;
        p.osc[1].level = 0.0f;
        p.env2Pos = 1.0f;
        p.env[1] = {0.5f, 0.5f, 0.5f, 0.3f};
        s.setPatch(p);
        for (int n : {36, 60, 84, 108}) s.noteOn(n, 100);
        float L[128], R[128], outPeak = 0.0f;
        for (int b = 0; b < 344; ++b) {
            s.render(L, R, 128);
            for (int i = 0; i < 128; ++i) {
                sane = sane && std::isfinite(L[i]) && std::isfinite(R[i]);
                outPeak = std::max(outPeak, std::max(std::fabs(L[i]), std::fabs(R[i])));
            }
        }
        sane = sane && outPeak > 1e-3f && outPeak < 8.0f;

        if (!sane) {
            std::printf("FAIL check %s (table peak %.4f, output peak %.4f)\n", rel.c_str(), peak, outPeak);
            ++failed;
            continue;
        }
        ++ok;
        frames += t.frames;
        totalMs += ms;
        perFrameUs = std::max(perFrameUs, 1000.0 * ms / t.frames);
        if (ms > worstMs) {
            worstMs = ms;
            worstName = rel;
        }
        worstBytes = std::max(worstBytes, t.data.size() * sizeof(float));
    }

    std::printf("%d tables ok, %d failed, %d frames\n", ok, failed, frames);
    if (ok) {
        std::printf("load: %.1f ms total, %.2f ms per table avg, worst %.1f ms (%s), up to %.0f us per frame\n",
                    totalMs, totalMs / ok, worstMs, worstName.c_str(), perFrameUs);
        std::printf("memory: up to %.1f MB per loaded table (%d mip levels x %d frames max)\n",
                    static_cast<double>(worstBytes) / (1024.0 * 1024.0), pf::kMipLevels, pf::kMaxFrames);
    }
    return failed ? 1 : 0;
}
