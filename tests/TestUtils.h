#pragma once

#include "dawg/AudioEngine.h"
#include "dawg/Commands.h"
#include "dawg/GraphCompiler.h"
#include "dawg/VarUtils.h"

#include <array>
#include <vector>

namespace dawg::test
{

/** A tempo map of 120 BPM in 4/4 that outlives every TimeMap made from it. */
[[nodiscard]] inline const TempoMap& constantTempo()
{
    static const TempoMap map;
    return map;
}

inline constexpr double sampleRate = 48000.0;
inline constexpr int blockSize = 512;
inline constexpr ProcessSpec spec { sampleRate, blockSize };

/**
    Two channels of float samples for offline rendering.
*/
class StereoBuffer final
{
public:
    explicit StereoBuffer (int numSamples)
        : m_left (static_cast<size_t> (numSamples)),
          m_right (static_cast<size_t> (numSamples)),
          m_channels { m_left.data(), m_right.data() }
    {
    }

    [[nodiscard]] float* const* channels() noexcept { return m_channels.data(); }
    [[nodiscard]] const std::vector<float>& left() const noexcept { return m_left; }
    [[nodiscard]] const std::vector<float>& right() const noexcept { return m_right; }
    [[nodiscard]] int size() const noexcept { return static_cast<int> (m_left.size()); }

private:
    std::vector<float> m_left;
    std::vector<float> m_right;
    std::array<float*, 2> m_channels;
};

/**
    Applies a command and returns the result (throws CommandError on failure).
*/
[[nodiscard]] inline Project apply (const Project& project, const char* command,
                                    std::initializer_list<std::pair<const char*, juce::var>> args)
{
    return applyCommand (project, command, makeObject (args));
}

/**
    A project with one default track and one 16-beat clip on it.
*/
struct ProjectWithClip
{
    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
};

[[nodiscard]] inline ProjectWithClip projectWithClip()
{
    ProjectWithClip result;
    result.project = apply ({}, "addTrack", {});
    result.trackId = result.project.tracks[0]->id;
    result.project = apply (result.project, "addClip",
                            { { "trackId", result.trackId }, { "start", 0 }, { "length", 16 * ticksPerBeat } });
    result.clipId = result.project.tracks[0]->clips[0]->id;
    return result;
}

/**
    A note argument object for addNotes.
*/
[[nodiscard]] inline juce::var noteArgs (Ticks start, Ticks length, double pitch)
{
    return makeObject ({ { "start", start }, { "length", length }, { "pitch", pitch } });
}

/**
    Renders numSamples into a fresh buffer.
*/
[[nodiscard]] inline StereoBuffer render (AudioEngine& engine, int numSamples)
{
    StereoBuffer buffer (numSamples);
    engine.renderOffline (buffer.channels(), 2, numSamples, sampleRate);
    return buffer;
}

/**
    Counts sign changes (zero crossings) in [from, to).
*/
[[nodiscard]] inline int zeroCrossings (const std::vector<float>& samples, size_t from, size_t to)
{
    int count = 0;

    for (auto i = from + 1; i < to; ++i)
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f))
            ++count;

    return count;
}

/**
    Index of the first sample whose magnitude exceeds a threshold, or -1.
*/
[[nodiscard]] inline int firstAudible (const std::vector<float>& samples, float threshold = 1.0e-6f)
{
    for (size_t i = 0; i < samples.size(); ++i)
        if (std::abs (samples[i]) > threshold)
            return static_cast<int> (i);

    return -1;
}

} // namespace dawg::test
