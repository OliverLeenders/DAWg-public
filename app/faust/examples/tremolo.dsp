declare name "Tremolo";
import("stdfaust.lib");

rate = hslider("rate[unit:Hz][scale:log][style:knob]", 4, 0.1, 20, 0.01);
depth = hslider("depth[style:knob]", 0.5, 0, 1, 0.01);

// A sine between 0 and 1 lowers the gain by up to `depth`.
gain = 1 - depth * (0.5 + 0.5 * os.osc(rate));

process = *(gain), *(gain);
