declare name "Simple reverb";
import("stdfaust.lib");

size = hslider("size[style:knob]", 0.7, 0, 1, 0.01);
damping = hslider("damping[style:knob]", 0.5, 0, 1, 0.01);
mix = hslider("mix[style:knob]", 0.3, 0, 1, 0.01);

// Freeverb: comb feedback from the size, fixed all-pass feedback and stereo spread.
wet = re.stereo_freeverb(0.7 + 0.28 * size, 0.5, damping, 23);

mixer(dry, tail) = dry * (1 - mix) + tail * mix;

process = _, _ <: (_, _), wet : ro.interleave(2, 2) : mixer, mixer;
