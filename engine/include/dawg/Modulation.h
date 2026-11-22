#pragma once

#include "dawg/Model.h"
#include "dawg/Node.h"

#include <array>
#include <cmath>

namespace dawg
{

/**
    The modulation input pool every effect and instrument exposes (docs/design/modulation.md, M2).

    The UI shows only the used slots plus the first free one; the engine and project file keep the whole pool so a
    connection's port index stays stable while slots are added and removed.
*/
inline constexpr size_t maxModulationInputs = 8;

/**
    The ports appended to a node's ordinary inputs.
*/
inline constexpr std::array<PortInfo, maxModulationInputs> modulationInputPorts = []
{
    std::array<PortInfo, maxModulationInputs> ports {};

    for (auto& port : ports)
        port = { PortType::modulation, "Mod" };

    return ports;
}();

/**
    Returns a node's inputs with the modulation pool appended.

    @param base  The node's ordinary inputs.
    @return      The inputs followed by maxModulationInputs modulation ports.
*/
template <size_t N>
[[nodiscard]] constexpr std::array<PortInfo, N + maxModulationInputs>
withModulationInputs (const std::array<PortInfo, N>& base) noexcept
{
    std::array<PortInfo, N + maxModulationInputs> result {};

    for (size_t i = 0; i < N; ++i)
        result[i] = base[i];

    for (size_t i = 0; i < maxModulationInputs; ++i)
        result[N + i] = modulationInputPorts[i];

    return result;
}

/**
    The inputs of a modulation source: note events (optional) and an audio trigger (optional, also the node's key
    input for sidechain sends).
*/
inline constexpr std::array<PortInfo, 2> modulationSourceInputs { {
    { PortType::events, "Notes" },
    { PortType::audio, "Trigger" },
} };

/**
    The LFO's Sine shape (docs/design/modulation.md, M4): four free segments through 0, 1, 0, -1 and 0, each bent
    as close to a quarter sine as one tension point allows (within 0.4 % of full scale). An LFO with no
    shape plays this, and the editor's Sine preset is the same curve, so what is drawn is what plays.
*/
inline constexpr std::array<CurvePoint, 5> lfoSineShape { {
    { 0, 0.0, CurveShape::free, 0.0, 0.6005, 0.984 },
    { modulationShapeTicks / 4, 1.0, CurveShape::free, 0.0, 0.3995, 0.016 },
    { modulationShapeTicks / 2, 0.0, CurveShape::free, 0.0, 0.6005, 0.984 },
    { modulationShapeTicks * 3 / 4, -1.0, CurveShape::free, 0.0, 0.3995, 0.016 },
    { modulationShapeTicks, 0.0 },
} };

/**
    The length in beats (quarter notes) of each tempo-synced LFO cycle, indexed by the LFO's "cycle" choice
    (docs/design/modulation.md, M4): straight, dotted and triplet note values from 1/64T to 4/1, shortest first.
*/
inline constexpr std::array<double, 26> lfoCycleBeats {
    1.0 / 24.0, 1.0 / 16.0, 1.0 / 12.0, 3.0 / 32.0, 1.0 / 8.0, 1.0 / 6.0,  3.0 / 16.0, 1.0 / 4.0, 1.0 / 3.0,
    3.0 / 8.0,  1.0 / 2.0,  2.0 / 3.0,  3.0 / 4.0,  1.0,       4.0 / 3.0,  3.0 / 2.0,  2.0,       8.0 / 3.0,
    3.0,        4.0,        16.0 / 3.0, 6.0,        8.0,       32.0 / 3.0, 12.0,       16.0,
};

/**
    The names of lfoCycleBeats' note lengths, in the same order: "." is dotted and "T" triplet.
*/
inline constexpr std::array<const char*, lfoCycleBeats.size()> lfoCycleChoices {
    "1/64T", "1/64", "1/32T", "1/64.", "1/32", "1/16T", "1/32.", "1/16", "1/8T", "1/16.", "1/8",  "1/4T", "1/8.",
    "1/4",   "1/2T", "1/4.",  "1/2",   "1/1T", "1/2.",  "1/1",   "2/1T", "1/1.", "2/1",   "4/1T", "2/1.", "4/1",
};

/**
    The output of a modulation source.
*/
inline constexpr std::array<PortInfo, 1> modulationOutput { { { PortType::modulation, "Mod" } } };

/**
    The pitch a pitch signal of 0 stands for, in MIDI semitones (C4).
*/
inline constexpr double pitchSignalCentre = 60.0;

/**
    The semitones a pitch signal of 1 stands for. A pitch parameter spanning this many semitones (the oscillator's
    -120..120) follows a pitch signal exactly at 100 % depth (docs/design/phase12-voice-scope.md, V3).
*/
inline constexpr double pitchSignalRange = 240.0;

/**
    Returns the modulation signal for a pitch.

    @param pitch  MIDI semitones.
    @return       (pitch - pitchSignalCentre) / pitchSignalRange.
*/
[[nodiscard]] constexpr double pitchSignal (double pitch) noexcept
{
    return (pitch - pitchSignalCentre) / pitchSignalRange;
}

/**
    How long a node that follows per-sample modulation (ProcessContext::modulatedParams) smooths it: short enough that
    a source's jump stays a sharp attack, long enough not to click (docs/design/modulation.md, M5).
*/
inline constexpr double modulationSmoothingSeconds = 0.001;

/**
    The one-pole coefficient for modulationSmoothingSeconds: each sample, value += (target - value) * coefficient.

    @param sampleRate  The sample rate.
    @return            The coefficient, 0..1.
*/
[[nodiscard]] inline float modulationSmoothingCoefficient (double sampleRate) noexcept
{
    return sampleRate > 0.0 ? static_cast<float> (1.0 - std::exp (-1.0 / (modulationSmoothingSeconds * sampleRate)))
                            : 1.0f;
}

} // namespace dawg
