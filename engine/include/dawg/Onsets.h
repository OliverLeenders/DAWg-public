#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace dawg
{

/**
    A detected note onset (docs/design/time-stretching.md, TS11): where timing markers snap.
*/
struct Onset
{
    /**
        The frame where the onset begins.
    */
    juce::int64 frame = 0;

    /**
        How strong it is, 0..1 (1 for the file's strongest).
    */
    float strength = 0.0f;
};

/**
    Finds the onsets in audio: spectral flux of the mono sum, peaks above an adaptive threshold, each refined to the
    attack in the waveform.

    @param audio       The audio (any number of channels).
    @param sampleRate  Its sample rate.
    @return            The onsets, by frame.
*/
[[nodiscard]] std::vector<Onset> detectOnsets (const juce::AudioBuffer<float>& audio, double sampleRate);

} // namespace dawg
