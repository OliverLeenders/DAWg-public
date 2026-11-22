#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace dawg
{

/**
    What one offline stretch produces (docs/design/time-stretching.md, TS1, TS7).
*/
struct StretchSpec
{
    /**
        The audio's sample rate, kept by the stretch.
    */
    double sampleRate = 48000.0;

    /**
        The output's length in frames (the input's length is the input buffer's).
    */
    juce::int64 outputFrames = 0;

    /**
        Key frames inside the material: input frame -> output frame, both increasing. The ends map by the overall
        ratio.
    */
    std::vector<std::pair<juce::int64, juce::int64>> keyFrames;

    /**
        The pitch scale (2 is an octave up).
    */
    double pitchScale = 1.0;

    /**
        Whether a pitch change keeps the formants (an audio clip's transpose) or moves them with the pitch, as tape does
        (a Sampler's Keep length, docs/design/sampler-mapping.md, PM2).
    */
    bool formants = true;
};

/**
    Changes the speed of audio without changing its pitch, offline (TS1). The renderer calls the library through this
    interface only, so another library could replace it without touching the cache or the player.

    @note  Thread-safe: render() may run on several threads at once.
*/
class Stretcher
{
public:
    virtual ~Stretcher() = default;

    /**
        Returns what identifies the library, its version and its options. Renders depend on it, so it is part of
        their cache key (TS8).
    */
    [[nodiscard]] virtual juce::String identity() const = 0;

    /**
        Renders a stretch.

        @param input      The source audio (one or two channels).
        @param spec       The output length, key frames and pitch.
        @param cancelled  Polled now and then; returning true abandons the render.
        @return           The output, with spec.outputFrames frames and the input's channels, or an empty buffer if
                          cancelled.
    */
    [[nodiscard]] virtual juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input,
                                                           const StretchSpec& spec,
                                                           const std::function<bool()>& cancelled) const = 0;
};

/**
    Creates the Rubber Band stretcher: the R3 ("finer") engine in offline mode, with formants preserved and the
    channels processed together.
*/
[[nodiscard]] std::shared_ptr<const Stretcher> makeRubberBandStretcher();

} // namespace dawg
