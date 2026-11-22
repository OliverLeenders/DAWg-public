#pragma once

#include "dawg/Curve.h"
#include "dawg/Node.h"

#include <array>
#include <map>
#include <memory>

namespace dawg
{

class NodeRegistry;

/**
    Creates the internal delay used for latency compensation. Not in the registry; the graph compiler inserts
    it where paths of different latency meet.

    @param delaySamples  The delay in samples. At least 1.
    @return              The node (one stereo input, one stereo output).
*/
[[nodiscard]] std::unique_ptr<Node> makeDelayCompensationNode (int delaySamples);

/**
    Registers DAWg's built-in node types (sequencer, MPE synth, gain, track output).

    @param registry  The registry to add them to.
*/
void registerBuiltinNodes (NodeRegistry& registry);

/**
    Registers the built-in effects: EQ, filter, compressor, gate, expander, utility and mix (docs/design/phase8-node-graph.md, G1).
    Called by registerBuiltinNodes().

    @param registry  The registry to add them to.
*/
void registerEffectNodes (NodeRegistry& registry);

/**
    Registers the split nodes: the Band Split, and the Stereo Split and Merge (docs/design/split-nodes.md). Called by
    registerBuiltinNodes().

    @param registry  The registry to add them to.
*/
void registerSplitNodes (NodeRegistry& registry);

/**
    Registers the modulation sources: the LFO, ADSR (docs/design/modulation.md) and Note Expression
    (docs/design/phase12-voice-scope.md, V2). Called by registerBuiltinNodes().

    @param registry  The registry to add them to.
*/
void registerModulationNodes (NodeRegistry& registry);

/**
    Makes a Macros device's instance in a voice slot follow the track's (docs/design/macros-and-modulation-column.md,
    MC8): it reads the knob values from `leader`, so their automation and MIDI reach every voice, and ramps them itself.
    Does nothing if either isn't a Macros device.

    @param follower  The voice slot's instance.
    @param leader    The track's instance. Kept alive by the follower.
    @note  Message thread.
*/
void followMacros (Node& follower, std::shared_ptr<Node> leader);

/**
    Registers the oscillator (docs/design/phase12-voice-scope.md, V14). Called by registerBuiltinNodes().

    @param registry  The registry to add it to.
*/
void registerOscillator (NodeRegistry& registry);

/**
    Registers the voice scope's nodes: the Voice Instrument, Voice Input and Voice Output
    (docs/design/phase12-voice-scope.md, V1). Called by registerBuiltinNodes().

    @param registry  The registry to add them to.
*/
void registerVoiceNodes (NodeRegistry& registry);

/**
    Registers the Sampler (docs/design/sampler-midi-delay-limiter.md, section 4). Called by registerBuiltinNodes().

    @param registry  The registry to add to.
*/
void registerSampler (NodeRegistry& registry);

/**
    Registers the Loop Reverb (docs/design/loop-reverb.md). Called by registerEffectNodes().

    @param registry  The registry to add it to.
*/
void registerLoopReverb (NodeRegistry& registry);

/**
    Waits until every Convolution Reverb's impulse response has been built (docs/design/convolution-reverb.md, I15): for
    an export, and for tests.
*/
void waitForConvolutionBuilds();

#if DAWG_PRIVATE_NODES
/**
    Registers the nodes kept outside the repository (DAWG_PRIVATE_NODES_DIR, cmake/DawgEngine.cmake). Called by
    registerBuiltinNodes() in builds that have them; defined by the private folder's sources.

    @param registry  The registry to add them to.
*/
void registerPrivateNodes (NodeRegistry& registry);
#endif

/**
    The compressor's static curve, exposed for tests: the gain change for an input level, with a 6 dB soft knee.

    @param levelDb      Input level in dBFS.
    @param thresholdDb  Threshold in dBFS.
    @param ratio        Compression ratio (1 or more).
    @return             The gain change in dB, 0 or negative.
*/
[[nodiscard]] float compressorGainChangeDb (float levelDb, float thresholdDb, float ratio) noexcept;

/**
    The expander's static curve, exposed for tests (docs/design/gate-expander.md, N10): the gain change for a detector
    level, 1:ratio below the threshold with a 6 dB soft knee, limited by the range.

    @param levelDb      Detector level in dBFS.
    @param thresholdDb  Threshold in dBFS.
    @param ratio        Expansion ratio (1 or more): each dB below the threshold costs ratio - 1 dB of gain.
    @param rangeDb      The most gain change, 0 or negative.
    @return             The gain change in dB, between rangeDb and 0.
*/
[[nodiscard]] float expanderGainChangeDb (float levelDb, float thresholdDb, float ratio, float rangeDb) noexcept;

/**
    Number of points of an EQ or filter response curve (docs/design/phase11-faust-panels.md, P1).
*/
inline constexpr int responsePoints = 256;

/**
    Computes what a device panel draws for a built-in effect at the given parameter values, from the coefficients
    and curves the node itself uses (docs/design/phase11-faust-panels.md, P1).

    @param type        The node type: nodetype::eq, nodetype::filter, nodetype::compressor, nodetype::saturator,
                       nodetype::clipper or nodetype::waveshaper.
    @param params      Parameter values by id; missing ones use their defaults.
    @param sampleRate  The sample rate the node runs at.
    @return            EQ and filter: { x: frequencies (Hz, responsePoints log-spaced from 20 Hz to 20 kHz), y: gain
                       (dB) }, and for the EQ also bands: [[dB...] per band]. Compressor: { x: input level (dB), y:
                       output level (dB, with makeup) }. Saturator: { x: input (-1..1), y: output, the static curve
                       with mix and output gain }. Clipper: the same, with the final clamp at
                       the ceiling. Waveshaper: the same, with the drawn curve as the identity. A void var for other types.
*/
[[nodiscard]] juce::var effectResponse (const juce::String& type, const std::map<juce::String, double>& params,
                                        double sampleRate);

/**
    The Waveshaper's curves and their antiderivatives (docs/design/waveshaper.md, S3, S4, S6), for the node and its
    tests.
*/
namespace waveshaper
{
/**
    The curves, in the order of the node's Shape parameter.
*/
enum class Shape
{
    soft,        ///< u / (1 + |u|)
    tube,        ///< 1 - e^-u above 0, 2 (e^(u/2) - 1) below: asymmetric
    fold,        ///< a triangle wavefolder, the line up to +-1, folding back every 2 units
    sineFold,    ///< sin u
    halfRectify, ///< u / (1 + u) above 0, 0 below
    fullRectify, ///< |u| / (1 + |u|)
    drawn,       ///< a curve drawn on the panel, as a DrawnTable
};

/**
    Below this difference between two inputs, the ADAA quotient is ill-conditioned and the curve at the midpoint is
    used.
*/
inline constexpr double illConditioned = 1.0e-5;

/**
    A drawn curve sampled into linear segments over u from -1 to 1, with its running integral, so that its
    antiderivative is exact and piecewise quadratic (S6). Beyond +-1 the curve holds its end values.
*/
struct DrawnTable
{
    /**
        Number of linear segments.
    */
    static constexpr size_t segments = 256;

    /**
        Segments per unit of u.
    */
    static constexpr double segmentsPerUnit = 128.0;

    /**
        The curve at u = -1 + i / segmentsPerUnit, clamped to -1..1.
    */
    std::array<double, segments + 1> y {};

    /**
        The curve's integral from -1 to each of those points.
    */
    std::array<double, segments + 1> integral {};

    /**
        Samples a curve whose times 0..modulationShapeTicks map to u from -1 to 1; an empty curve is the identity.

        @param points     The curve.
        @param symmetric  Whether to use only the curve's positive half (times from modulationShapeTicks / 2), mirrored
                          for negative inputs: f(-u) = -f(u) (docs/design/waveshaper.md, S6).
    */
    void fill (const Curve& points, bool symmetric = false);
};

/**
    A drawn curve as the node publishes it to the audio thread: sampled both ways, so switching between symmetric and
    asymmetric needs no work on the audio thread.
*/
struct DrawnTables
{
    /**
        The whole curve.
    */
    DrawnTable full;

    /**
        Its positive half, mirrored.
    */
    DrawnTable mirrored;
};

/**
    A curve's value.

    @param shape  The curve.
    @param u      The shaper's input.
    @param table  The drawn curve, for Shape::drawn; null means the identity, clamped to +-1.
    @return       f(u).
*/
[[nodiscard]] double curve (Shape shape, double u, const DrawnTable* table) noexcept;

/**
    A curve's antiderivative, in closed form (S4).

    @param shape  The curve.
    @param u      The shaper's input.
    @param table  The drawn curve, for Shape::drawn; null means the identity, clamped to +-1.
    @return       F(u), with F(0) = 0 for the built-in curves.
*/
[[nodiscard]] double antiderivative (Shape shape, double u, const DrawnTable* table) noexcept;

/**
    First-order ADAA: the curve's mean over the segment from the previous input to this one.

    @param shape     The curve.
    @param u         This input.
    @param previous  The previous input.
    @param table     The drawn curve, for Shape::drawn.
    @return          (F(u) - F(previous)) / (u - previous), or the curve at the midpoint when they are too close.
*/
[[nodiscard]] double antialiased (Shape shape, double u, double previous, const DrawnTable* table) noexcept;
} // namespace waveshaper

/**
    The Bit Crusher's quantiser and rate (docs/design/bit-crusher.md, B3–B5, B14), for the node and its tests.
*/
namespace bitcrusher
{
/**
    At this depth (the top of the Bits range) the quantiser is off and passes its input unchanged.
*/
inline constexpr float maxBits = 24.0f;

/**
    The frequency of the Note rate's 0 semitones: C4.
*/
inline constexpr double noteRateC4 = 261.6255653005986;

/**
    Quantises one value.

    @param x         The input, after drive, clip and bias.
    @param bits      The depth, 1..maxBits; fractional depths have steps between their neighbours'. At maxBits or
                     above, x is returned unchanged.
    @param midRise   False for mid-tread (levels at k / M, 0 among them, from -1 to 1), true for mid-rise (levels at
                     (k + 1/2) / M, within -1..1), with M = 2^(bits - 1).
    @return          The level.
*/
[[nodiscard]] double quantise (double x, double bits, bool midRise) noexcept;

/**
    The rate of the Note mode for a pitch.

    @param semitones  Semitones from C4.
    @return           The rate in Hz.
*/
[[nodiscard]] double noteRate (double semitones) noexcept;
} // namespace bitcrusher

} // namespace dawg
