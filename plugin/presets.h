#pragma once
// Presets and tunings as files, next to the wavetable library (plugin/library.h).
//
//   Presets   *.pfp under <plugin dir>/Presets and /media/AkaiForce/PolyForce Presets
//             (PF_PRESET_ROOTS), plus the factory set built in (presets/Factory, embedded at
//             build time: category "Factory", keys "builtin:<name>"). A preset file is the
//             plugin's own state text ("polyforce 4" + key=value lines), so a preset and a
//             project restore the same way. Saving writes <first root>/User/User NNN.pfp:
//             there is no text entry on the device, so names are numbered.
//   Tunings   *.tun and *.scl under <plugin dir>/Tunings and /media/AkaiForce/Tunings
//             (PF_TUNING_ROOTS), plus the built-in "12-TET". Loaded by the loader thread.
#include "library.h"
#include "loader.h"

#include <string>

namespace pf {

bool presetText(const std::string& key, std::string& out);   // factory or file
// Claims the next user preset file (created empty: highest number + 1) and its key. "" if
// there is no preset root. The caller writes it, or removes it on failure.
std::string nextUserPreset(std::string* key);

Loader::SlotType tuningSlotType();   // fallback builtin:12-TET; the object is a pf::Tuning

} // namespace pf
