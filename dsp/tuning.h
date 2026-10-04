#pragma once
// Microtuning: every MIDI note's pitch, in (fractional) semitones on the MIDI scale (69 =
// 440 Hz), from AnaMark .tun or Scala .scl files. Load time only.
//
//   .tun   [Tuning] / [Exact Tuning] "note N = cents" from MIDI note 0: at 8.1757989 Hz in
//          [Tuning], at BaseFreq (default the same) in [Exact Tuning], which wins where both are given.
//   .scl   "!" comments, a description line, the number of degrees, then each degree in cents
//          (has a ".") or as a ratio ("3/2", "2"). The last degree is the period (usually 2/1).
//          Degree 0 sits on MIDI note 60 at its 12-TET pitch.
#include <string>

namespace pf {

struct Tuning {
    std::string name;
    float pitch[128];   // semitones; equal temperament: pitch[n] = n
};

const Tuning& equalTemperament();   // the built-in 12-TET
bool parseTun(const std::string& text, Tuning& out, std::string* err = nullptr);
bool parseScl(const std::string& text, Tuning& out, std::string* err = nullptr);
bool loadTuning(const std::string& path, Tuning& out, std::string* err = nullptr);   // by extension

} // namespace pf
