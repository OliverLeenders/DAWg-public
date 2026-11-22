#pragma once

#include "Wav.h"
#include "dawg_sdk/Node.h"

#include <map>
#include <string>
#include <vector>

// Offline rendering through a node, the way DAWg's engine drives it: prepare(), then process() in blocks with the
// parameters set before each block.

namespace harness
{

/**
    How to render.
*/
struct RenderOptions
{
    /**
        Sample rate in Hz.
    */
    double sampleRate = 48000.0;

    /**
        Block sizes, used in turn (one entry for a fixed size). Each must be at most maxBlockSize.
    */
    std::vector<int> blockSizes { 512 };

    /**
        The largest block size the node is prepared for.
    */
    int maxBlockSize = 2048;

    /**
        Extra samples of silence rendered after the input, for the tail.
    */
    int tailSamples = 0;

    /**
        Parameter values by id, set before the first block.
    */
    std::map<std::string, float> params;
};

/**
    Renders audio through a node. The node is prepared first, which clears its state.

    @param node     The node.
    @param input    The input (at options.sampleRate).
    @param options  Rate, block sizes, tail and parameters.
    @return         The output, input.size() + tailSamples long.
*/
[[nodiscard]] Audio render (dawg::Node& node, const Audio& input, const RenderOptions& options);

/**
    A unit impulse at sample 0, then silence.

    @param seconds     Length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] Audio impulse (double seconds, double sampleRate);

/**
    A logarithmic sine sweep from 20 Hz to 20 kHz at -6 dBFS, the same on both channels.

    @param seconds     Length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] Audio sweep (double seconds, double sampleRate);

/**
    Seeded white noise at -12 dBFS RMS, independent per channel.

    @param seconds     Length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] Audio noise (double seconds, double sampleRate);

/**
    Silence.

    @param seconds     Length.
    @param sampleRate  Sample rate.
*/
[[nodiscard]] Audio silence (double seconds, double sampleRate);

} // namespace harness
