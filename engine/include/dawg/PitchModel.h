#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

namespace dawg
{

/**
    What a pitch model gives for a stretch of audio (docs/design/pitch-correction.md, section 3.2): per frame, a
    salience over pitch bins.
*/
struct PitchSalience
{
    /**
        Seconds between frames.
    */
    double hopSeconds = 0.01;

    /**
        The middle of the first frame, in seconds from the audio's start.
    */
    double startSeconds = 0.0;

    /**
        The pitch of each bin, in MIDI semitones, rising.
    */
    std::vector<float> binPitches;

    /**
        Frame by frame, binPitches.size() values each, 0..1.
    */
    std::vector<float> values;

    /**
        The number of frames.
    */
    [[nodiscard]] size_t frames() const noexcept { return binPitches.empty() ? 0 : values.size() / binPitches.size(); }

    /**
        One frame's values.
    */
    [[nodiscard]] const float* frame (size_t index) const noexcept { return values.data() + index * binPitches.size(); }
};

/**
    A neural pitch model (section 3.2, PC2), behind an interface so the model can be swapped.
*/
class PitchModel
{
public:
    virtual ~PitchModel() = default;

    /**
        The model's name and a hash of its weights: part of the analyser's version (PC1), so another model
        invalidates cached analyses.
    */
    [[nodiscard]] virtual juce::String identity() const = 0;

    /**
        The sample rate the model reads: 16 kHz for FCPE and CREPE.
    */
    [[nodiscard]] virtual double sampleRate() const noexcept = 0;

    /**
        The peak salience at which a frame counts as voiced as often as not (section 3.3).
    */
    [[nodiscard]] virtual float voicingThreshold() const noexcept = 0;

    /**
        Runs the model.

        @param audio      Mono audio at sampleRate().
        @param cancelled  Asked between batches; true stops early with an empty result. May be empty.
        @return           The salience, frame by frame, from the audio's start to its end.
    */
    [[nodiscard]] virtual PitchSalience salience (const std::vector<float>& audio,
                                                  const std::function<bool()>& cancelled) const = 0;
};

/**
    Loads a pitch model from an ONNX file and runs it through ONNX Runtime on the CPU. The kind is told from the
    graph: an input named "frames" is CREPE (1024 samples per frame, every 10 ms), one named "audio" is FCPE (the
    whole signal, its mel front end inside).

    @param file     The model file.
    @param error    Receives why it couldn't be loaded.
    @param threads  The threads one run may use: 1 where analyses run in parallel, more for the one file the user
                    waits for.
    @return         The model, or null with `error` set.
*/
[[nodiscard]] std::unique_ptr<PitchModel> loadPitchModel (const juce::File& file, juce::String& error, int threads = 1);

/**
    Where the pitch models are: next to the app (models/pitch), or the build's download folder in tests and builds
    run from the build tree.

    @return  The folder; may not exist.
*/
[[nodiscard]] juce::File pitchModelsFolder();

} // namespace dawg
