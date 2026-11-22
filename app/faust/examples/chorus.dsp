declare name "Chorus";
import("stdfaust.lib");

rate = hslider("rate[unit:Hz][style:knob]", 0.6, 0.05, 5, 0.01);
depth = hslider("depth[unit:ms][style:knob]", 3, 0, 10, 0.01);
mix = hslider("mix[style:knob]", 0.5, 0, 1, 0.01);

base = 12; // ms

// One modulated delay per side, the right side's LFO half a cycle behind.
voice(phase) = de.fdelay(8192, (base + depth * (0.5 + 0.5 * os.oscp(rate, phase))) * ma.SR / 1000);
side(phase, x) = x * (1 - mix) + (x : voice(phase)) * mix;

process = side(0), side(ma.PI);
