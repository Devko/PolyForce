#pragma once
// The CPU guard: keeps one instance from running the audio thread past what MPC can afford.
// The plugin reports the engine's own CPU time per call (thread clock, host callbacks not
// included); calls are added up to at least one 128-frame block, so process() sub-blocks and
// small host blocks are judged like MPC's. When that window cost too much of its real-time
// budget, the engine fades out the quietest release tails (Synth::shedTails, ~3 ms each): notes
// already let go, so nothing being played stops. Held and pedal-sustained notes are never
// touched. A single window over kHigh (a patch rebuild, a burst of note-ons) sheds nothing; two
// in a row shed one tail, and any window over kHeavy sheds two.

namespace pf {

class CpuGuard {
public:
    static constexpr double kHigh = 0.40;
    static constexpr double kHeavy = 0.65;
    static constexpr double kWindowUs = 2900.0;   // one 128-frame block at 44.1 kHz (2902 us)

    // After a call that took `us` of a `budgetUs` budget: how many tails to shed now.
    int afterBlock(double us, double budgetUs) {
        us_ += us;
        budget_ += budgetUs;
        if (budget_ < kWindowUs) return 0;
        const double load = us_ / budget_;
        us_ = budget_ = 0.0;
        const bool over = load > kHigh;
        const int shed = load > kHeavy ? 2 : (over && wasOver_) ? 1 : 0;
        wasOver_ = over;
        return shed;
    }

private:
    double us_ = 0.0, budget_ = 0.0;
    bool   wasOver_ = false;
};

} // namespace pf
