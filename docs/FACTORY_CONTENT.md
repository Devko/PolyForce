# Factory content

PolyForce ships with 30 built-in wavetables and 205 factory presets. Everything is computed or
written for PolyForce: no sample data, nothing that depends on files on the device.

- [Built-in wavetables](#built-in-wavetables)
- [Factory presets](#factory-presets)

---

## Built-in wavetables

All 30 are computed from formulas and band-limited like imported tables. They list in the
browser's *Built-in* category in this order.

| Table | Frames | Position sweeps | Tip |
|---|---|---|---|
| Classic | 16 | sine → triangle → saw → square | |
| PWM | 16 | pulse width 50% → 4% | |
| Sync | 16 | a saw hard-synced to the fundamental, ratio 1 → 8 | |
| Formant | 16 | a resonant peak, harmonic 1 → 48 | |
| Square Sync | 32 | a square hard-synced, ratio 1 → 8 | |
| Reso Saw | 32 | a saw through a resonant 24 dB low-pass, cutoff harmonic 1.5 → 96 | an envelope to position is a filter sweep with no filter |
| Reso Square | 32 | the same on a square | |
| Harmonics | 32 | additive, 1 → 32 harmonics | |
| Comb Saw | 32 | a saw plus a delayed copy: comb notches sweep down (flanger) | a slow LFO to position |
| Fold | 32 | a sine through a wavefolder, gain 1 → 8 | |
| Phase Dist | 32 | Casio CZ phase distortion, sine → sharp saw | |
| CZ Reso | 32 | Casio CZ resonance, peak 1 → 16 × the fundamental | |
| FM Ratio 1, 2, 3 | 32 | two-operator FM, modulator at 1×, 2×, 3×, index 0 → 6, 5, 4 | |
| FM Tine | 32 | 1:1 and 14:1 modulators rising: an FM electric piano's tine | position 0 is a pure sine, so an envelope decays the tine |
| Digital | 32 | eight seeded random spectra in turn | |
| Bitcrush | 32 | a sine with 64 → 2 levels and 256 → 16 steps a cycle | |
| Chip | 8 | pulse 12.5 / 25 / 50%, NES triangle, 4-bit saw, 4-bit sine, octave pulse, a Game Boy wave | position k/7 is shape k |
| Vowels | 32 | a tenor singing A → E → I → O → U | truest near C3 |
| Choir | 32 | an alto, oo → oh → ah → eh | truest near A3 |
| Growl | 32 | saturated bass vowels U → O → A → E → I | truest near C2 |
| Organ | 16 | tonewheel drawbars, 8' alone → all nine | harmonic 1 is the 16': play it at Octave −1 |
| E-Piano | 16 | mellow → barking | velocity to position |
| Strings | 16 | a bowed string in a violin body, dark → bright | |
| Brass | 16 | soft → blaring | velocity or an envelope to position |
| Reed | 16 | clarinet → oboe → saxophone | |
| Pluck | 32 | a plucked string, bright → dull, one gain for the table | position 1 with E2 > Pos −1: it darkens as it rings |
| Mallet | 16 | soft → marimba → vibraphone → xylophone → glockenspiel → kalimba | key frames at 0, 0.2 … 1 |
| Bell | 32 | a church bell, soft → bright, then → a metal plate | the prime is harmonic 16: play it at Octave −3, Semi −12 |

**When they're built:** Classic is built when the plugin loads, because it is every oscillator's
fallback. The others are built on the loader thread the first time a patch uses one, then cached
like a file (evicted when unused, rebuilt on demand): 6 to 73 ms each on x86.

---

## Factory presets

205 presets in 14 categories, one folder per category under `presets/Factory/`. These are the
preset browser's categories, in this order. A user folder named like a factory category lists next
to it as "Bass (files)" and so on.

| Category | Presets | For example |
|---|---|---|
| Templates | 9 | Init, Init Mono, Init Pad, Init Pluck, Init Wavetable, Init Arp, Init Seq, Init Drum |
| Bass | 24 | Moog Bass, Reese Bass, 808 Bass, FM Bass, Growl Bass, Talk Bass, Neuro Bass, Chip Bass |
| Lead | 20 | Saw Lead, Prophet Lead, Screamer, Fold Lead, Chip Lead, Vowel Lead, Whistle, Theremin |
| Pad | 20 | Warm Pad, Juno Pad, Glass Pad, Shimmer Pad, Flanger Pad, Air Pad, Drone Pad, Motion Pad |
| Keys | 13 | E-Piano, Suitcase EP, FM E-Piano, Wurli, Clav, Harpsichord, Jazz Organ, Church Organ |
| Pluck | 14 | Harp, Nylon Guitar, Koto, Sitar, Banjo, Future Pluck, Glass Pluck, Dub Pluck |
| Bell | 14 | Church Bell, Tubular Bell, Gong, Marimba, Vibraphone, Glockenspiel, Kalimba, Music Box |
| Brass & Wind | 12 | Brass Section, Trumpet, French Horn, Synth Brass, Flute, Clarinet, Oboe, Sax, Pan Flute |
| Strings | 10 | String Ensemble, Solo Violin, Cello, Pizzicato, Tremolo Strings, Solina, Slow Strings |
| Vocal | 10 | Choir Aah, Choir Ooh, Talk Box, Robot Voice, Wah Vox, Vowel Morph, Monk Choir |
| Synth | 16 | House Stab, Prophet Poly, Supersaw Chords, CZ Poly, Lo-Fi Keys, Chip Poly, Reso Motion |
| Arp & Seq | 16 | Up Down Arp, Chord Pulse, Berlin School, Acid Seq, Rhythm Gate, Shape Wobble, Bell Arp |
| Drum & Perc | 13 | Kick, 808 Kick, Snare, Clap, Closed Hat, Open Hat, Tom, Cowbell, Conga, Shaker |
| FX | 14 | Riser, Downlifter, Laser, Siren, Robot Bleeps, Wind, Ocean, Impact, Glitch, Star Field |

- **Same sound everywhere:** presets use only built-in tables, so they sound the same on every
  device.
- **Level-matched:** every preset was rendered through the plugin on a phrase that suits it and its
  volume set for the loudest 3 s at −19 LUFS (one-shots: loudest 400 ms at −17 LUFS), peaks at most
  −3 dBFS, so stepping through them doesn't jump in level.
- **Mod wheel:** apart from the drums and most of the original 21 presets, every preset answers the
  mod wheel (a brighter filter, more vibrato or a wavetable move); with the wheel down nothing
  changes.

### Adding factory presets

Files are named `NN_Category/NN_Name.pfp`: `NN` sets the order and `_` shows as a space. The build
embeds them in the plugin, and `surface/surface.py` checks every preset first: values in range,
names unique and short enough for a browser tile, and every table it names a built-in.
