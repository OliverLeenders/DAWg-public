#pragma once

#include "dawg/PitchAnalysis.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace dawg
{

/**
    Shifts the pitch of audio by a ratio that changes over time, keeping its length: output frame k is input frame k
    shifted (docs/design/pitch-correction.md, section 5.1, PC11).
*/
class PitchShifter
{
public:
    virtual ~PitchShifter() = default;

    /**
        What the renders depend on: the library, its version and options. Part of a rendered phrase's key.
    */
    [[nodiscard]] virtual juce::String identity() const = 0;

    /**
        Renders the shifted audio.

        @param input       The audio.
        @param sampleRate  Its sample rate.
        @param ratio       The pitch ratio (2 is an octave up), sampled every `hop` input frames from frame 0 and
                           interpolated between; held past its end. Empty means 1.
        @param hop         Input frames between ratio values.
        @param cancelled   Asked now and then; true stops early with an empty buffer. May be empty.
        @return            As long as the input, or empty if cancelled.
    */
    [[nodiscard]] virtual juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input, double sampleRate,
                                                           const std::vector<float>& ratio, int hop,
                                                           const std::function<bool()>& cancelled) const = 0;
};

/**
    Creates the Rubber Band shifter: real-time mode driven offline, with high-consistency pitch changes and the
    channels processed together.

    @param finer     The R3 ("finer") engine; false: R2 ("faster").
    @param formants  Formants preserved; false: shifted with the pitch.
*/
[[nodiscard]] std::shared_ptr<const PitchShifter> makeRubberBandPitchShifter (bool finer = true, bool formants = true);

/**
    Creates a TD-PSOLA shifter (a prototype for the shifter trial, section 5.1): pitch marks on the sung pitch, and
    two-period grains laid down again at the shifted period, which keeps the formants.

    @param track  The pitch of the audio it will render.
*/
[[nodiscard]] std::shared_ptr<const PitchShifter> makePsolaPitchShifter (PitchTrack track);

} // namespace dawg
