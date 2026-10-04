#pragma once
// The CPU guard: keeps one instance from running the audio thread past what MPC can afford.
// After a block that cost too much of its real-time budget (the thread's own CPU time, as the
// meter measures it), the engine fades out the quietest release tails (Synth::shedTails, ~3 ms
// each): notes already let go, so nothing being played stops. Held and pedal-sustained notes
// are never touched. One costly block on its own (a patch rebuild, a burst of note-ons) sheds
// nothing; a sustained overload sheds one tail per block, a heavy one two.

namespace pf {

class CpuGuard {
public:
    static constexpr double kHigh = 0.40;    // two blocks in a row over this: shed one tail
    static constexpr double kHeavy = 0.65;   // a block over this: shed two at once

    // How many tails to shed after a block that took `us` of a `budgetUs` budget.
    int afterBlock(double us, double budgetUs) {
        const double load = budgetUs > 0.0 ? us / budgetUs : 0.0;
        const bool over = load > kHigh;
        const int shed = load > kHeavy ? 2 : (over && wasOver_) ? 1 : 0;
        wasOver_ = over;
        return shed;
    }

private:
    bool wasOver_ = false;
};

} // namespace pf
