declare name "Ping-pong delay";
import("stdfaust.lib");

time = hslider("time[unit:ms][style:knob]", 375, 1, 2000, 1) * ma.SR / 1000;
feedback = hslider("feedback[style:knob]", 0.5, 0, 0.95, 0.01);
mix = hslider("mix[style:knob]", 0.35, 0, 1, 0.01);

maxDelay = 524288;
line = de.sdelay(maxDelay, 1024, time);

// The left line takes the input and the right line's echo; the right line takes the left line's echo.
lines(fromLeft, fromRight, l, r) = ((l + r) * 0.5 + fromRight * feedback : line), (fromLeft * feedback : line);
wet = lines ~ (_, _);

mixer(dry, echo) = dry * (1 - mix) + echo * mix;

process = _, _ <: (_, _), wet : ro.interleave(2, 2) : mixer, mixer;
