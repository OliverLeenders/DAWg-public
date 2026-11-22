declare name "Voice synth";
declare description "A synth voice for a Voice Instrument's voice graph: patch Note Expression's Pitch to pitch and its Gate to gate, both at 100 %.";
import("stdfaust.lib");

// Semitones from C4, over the same -120..120 range as the Oscillator's pitch, so Note Expression's Pitch output
// tracks the note exactly at 100 % depth (docs/design/phase12-voice-scope.md, V3).
pitch = hslider("pitch[unit:st]", 0, -120, 120, 0.01);
// 1 while the note is held: Note Expression's Gate output.
gate = hslider("gate", 0, 0, 1, 0.01) > 0.5;

detune = hslider("detune[unit:ct][style:knob]", 8, 0, 50, 0.1);
cutoff = hslider("cutoff[unit:Hz][scale:log][style:knob]", 2400, 60, 16000, 1);
resonance = hslider("resonance[style:knob]", 2, 0.7, 10, 0.01);
attack = hslider("attack[unit:s][scale:log][style:knob]", 0.005, 0.001, 2, 0.001);
release = hslider("release[unit:s][scale:log][style:knob]", 0.3, 0.005, 5, 0.001);

freq = 261.6256 * 2 ^ (pitch / 12);
cents(c) = 2 ^ (c / 1200);

// Two saws detuned against each other, one per side, through a resonant lowpass that opens with the envelope.
env = en.asr(attack, 1, release, gate);
saw(f) = os.sawtooth(f) : fi.resonlp(cutoff * (0.25 + 0.75 * env), resonance, 1);
process = saw(freq * cents(detune)) * env * 0.12, saw(freq / cents(detune)) * env * 0.12;
