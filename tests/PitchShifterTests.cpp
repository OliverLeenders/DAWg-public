// Pitch shifting for pitch correction (docs/design/pitch-correction.md, section 5.1), and the shifter trial of
// section 7: hand-made corrections rendered on real takes, to hear Rubber Band's real-time mode before the model and
// the editing are built on it.

#include "dawg/PitchAnalysis.h"
#include "dawg/PitchShifter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <tuple>

using namespace dawg;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double rate = 44100.0;

// A sine whose frequency follows `hzAt` (0: silence), with continuous phase.
template <typename Hz>
[[nodiscard]] juce::AudioBuffer<float> toneOf (double seconds, Hz&& hzAt)
{
    juce::AudioBuffer<float> audio (1, static_cast<int> (seconds * rate));
    auto phase = 0.0;

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto hz = hzAt (i / rate);
        phase += juce::MathConstants<double>::twoPi * hz / rate;
        audio.setSample (0, i, hz > 0.0 ? static_cast<float> (0.5 * std::sin (phase)) : 0.0f);
    }

    return audio;
}

// The frequency of a stretch, from its rising zero crossings.
[[nodiscard]] double frequencyOf (const juce::AudioBuffer<float>& audio, double from, double to)
{
    const auto* x = audio.getReadPointer (0);
    double first = -1.0;
    double last = -1.0;
    auto crossings = 0;

    for (auto i = static_cast<int> (from * rate); i < static_cast<int> (to * rate); ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const auto at = static_cast<float> (i - 1) + x[i - 1] / (x[i - 1] - x[i]);

            if (first < 0.0)
                first = at;
            else
                ++crossings;

            last = at;
        }
    }

    return crossings > 0 ? crossings * rate / (last - first) : 0.0;
}

// The first frame louder than `threshold`.
[[nodiscard]] int firstLoud (const juce::AudioBuffer<float>& audio, float threshold)
{
    for (int i = 0; i < audio.getNumSamples(); ++i)
        if (std::abs (audio.getSample (0, i)) > threshold)
            return i;

    return -1;
}
} // namespace

TEST_CASE ("pitch shifter: a constant ratio keeps the length and shifts the pitch", "[pitch][shifter]")
{
    const auto shifter = makeRubberBandPitchShifter();
    const auto input = toneOf (1.0, [] (double) { return 220.0; });
    const auto ratio = std::pow (2.0, 3.0 / 12.0);
    const auto output =
        shifter->render (input, rate, { static_cast<float> (ratio) }, static_cast<int> (rate / 100), {});

    REQUIRE (output.getNumSamples() == input.getNumSamples());
    CHECK_THAT (frequencyOf (output, 0.3, 0.7), WithinRel (220.0 * ratio, 0.01));
}

TEST_CASE ("pitch shifter: output frames line up with input frames", "[pitch][shifter]")
{
    const auto shifter = makeRubberBandPitchShifter();
    const auto input = toneOf (1.0, [] (double t) { return t < 0.5 ? 0.0 : 220.0; });

    for (const auto ratio : { 1.0f, 1.2f, 0.85f })
    {
        const auto output = shifter->render (input, rate, { ratio }, static_cast<int> (rate / 100), {});
        INFO ("ratio " << ratio);
        // The tone starts at 0.5 s; within 10 ms either way.
        CHECK (std::abs (firstLoud (output, 0.1f) - static_cast<int> (0.5 * rate)) < static_cast<int> (0.01 * rate));
    }
}

TEST_CASE ("pitch shifter: the ratio follows its curve", "[pitch][shifter]")
{
    // R2 ("faster") is left out: in this mode it shifts too far (about 2.5 semitones for 2).
    const auto formants = GENERATE (true, false);
    INFO ("formants " << formants);
    const auto shifter = makeRubberBandPitchShifter (true, formants);
    const auto input = toneOf (2.0, [] (double) { return 220.0; });
    const auto up = static_cast<float> (std::pow (2.0, 2.0 / 12.0));
    std::vector<float> ratio (200, 1.0f); // every 10 ms

    for (size_t i = 100; i < ratio.size(); ++i)
        ratio[i] = up;

    const auto output = shifter->render (input, rate, ratio, static_cast<int> (rate / 100), {});
    CHECK_THAT (frequencyOf (output, 0.3, 0.8), WithinRel (220.0, 0.01));
    CHECK_THAT (frequencyOf (output, 1.2, 1.8), WithinRel (220.0 * up, 0.01));

    // The change lands where the curve has it, at 1 s: where a 20 ms window's pitch passes halfway.
    auto switched = 0.0;

    for (auto t = 0.8; t < 1.2 && switched == 0.0; t += 0.002)
        if (frequencyOf (output, t - 0.01, t + 0.01) > 220.0 * std::sqrt (static_cast<double> (up)))
            switched = t;

    CHECK_THAT (switched, Catch::Matchers::WithinAbs (1.0, 0.01));
}

namespace
{
// A track of a steady pitch, as the analysis would give for toneOf at that pitch, voiced where `voicedAt` says.
template <typename Voiced>
[[nodiscard]] PitchTrack steadyTrack (double seconds, double pitch, Voiced&& voicedAt)
{
    PitchTrack track;
    track.sampleRate = rate;
    track.startSeconds = 0.005;

    for (auto t = track.startSeconds; t < seconds; t += track.hopSeconds)
    {
        const auto voiced = voicedAt (t);
        track.pitch.push_back (voiced ? static_cast<float> (pitch) : std::numeric_limits<float>::quiet_NaN());
        track.voicedProb.push_back (voiced ? 0.9f : 0.0f);
        track.levelDb.push_back (voiced ? -10.0f : -90.0f);
    }

    return track;
}
} // namespace

TEST_CASE ("PSOLA shifter: shifts the pitch, follows the curve, keeps the timing", "[pitch][shifter]")
{
    // 220 Hz is MIDI 57.
    const auto input = toneOf (2.0, [] (double t) { return t < 0.5 ? 0.0 : 220.0; });
    const auto shifter = makePsolaPitchShifter (steadyTrack (2.0, 57.0, [] (double t) { return t >= 0.5; }));
    const auto up = static_cast<float> (std::pow (2.0, 2.0 / 12.0));
    std::vector<float> ratio (200, 1.0f);

    for (size_t i = 125; i < ratio.size(); ++i)
        ratio[i] = up;

    const auto output = shifter->render (input, rate, ratio, static_cast<int> (rate / 100), {});
    REQUIRE (output.getNumSamples() == input.getNumSamples());
    CHECK (std::abs (firstLoud (output, 0.1f) - static_cast<int> (0.5 * rate)) < static_cast<int> (0.01 * rate));
    CHECK_THAT (frequencyOf (output, 0.6, 1.2), WithinRel (220.0, 0.01));
    CHECK_THAT (frequencyOf (output, 1.3, 1.9), WithinRel (220.0 * up, 0.01));

    auto switched = 0.0;

    for (auto t = 1.05; t < 1.45 && switched == 0.0; t += 0.002)
        if (frequencyOf (output, t - 0.01, t + 0.01) > 220.0 * std::sqrt (static_cast<double> (up)))
            switched = t;

    CHECK_THAT (switched, Catch::Matchers::WithinAbs (1.25, 0.01));

    // At a ratio of 1 every grain goes back where it came from: the audio comes out as it went in.
    const auto same = shifter->render (input, rate, { 1.0f }, static_cast<int> (rate / 100), {});
    auto worst = 0.0f;

    for (int i = 0; i < input.getNumSamples(); ++i)
        worst = std::max (worst, std::abs (same.getSample (0, i) - input.getSample (0, i)));

    CHECK (worst < 1.0e-4f);
}

namespace
{
// One trial correction: the change in semitones at each track frame, NaN where the audio is left alone.
using Trial = std::function<double (const DetectedPitchNote&, size_t noteIndex, double sung, double shapeAt)>;

// The ratio every `hop` audio frames for a trial (section 4.3): the change inside each note's voiced frames, 0
// elsewhere, ramped over a few frames at voicing edges, and, if `smoothSteps`, steps between notes eased too.
[[nodiscard]] std::vector<float> ratioOf (const PitchAnalysis& analysis, const Trial& trial, bool smoothSteps,
                                          juce::int64 frames, int hop)
{
    const auto& track = analysis.track;
    std::vector<double> delta (track.size(), 0.0);
    std::vector<double> weight (track.size(), 0.0);

    for (size_t n = 0; n < analysis.notes.size(); ++n)
    {
        const auto& note = analysis.notes[n];
        const auto first = static_cast<size_t> (std::max (0.0, std::ceil (track.indexOf (note.start))));
        const auto last = std::min (track.size(), static_cast<size_t> (std::ceil (track.indexOf (note.end))));

        for (auto i = first; i < last; ++i)
        {
            if (! track.voiced (i))
                continue;

            const auto frame = static_cast<double> (track.audioFrameOf (static_cast<double> (i)));
            const auto shape = evaluateCurve (note.shape, frame - static_cast<double> (note.start), 0.0);
            delta[i] = trial (note, n, track.pitch[i], shape);
            weight[i] = 1.0;
        }
    }

    // Unvoiced frames hold the nearest voiced change, so easing never pulls towards 0 inside a note.
    for (size_t i = 1; i < delta.size(); ++i)
        if (weight[i] == 0.0 && weight[i - 1] > 0.0)
            delta[i] = delta[i - 1];

    const auto average = [] (const std::vector<double>& v, int half)
    {
        std::vector<double> out (v.size());

        for (size_t i = 0; i < v.size(); ++i)
        {
            auto sum = 0.0;
            auto n = 0;

            for (auto k = -half; k <= half; ++k)
                if (const auto j = static_cast<std::ptrdiff_t> (i) + k;
                    j >= 0 && j < static_cast<std::ptrdiff_t> (v.size()))
                {
                    sum += v[static_cast<size_t> (j)];
                    ++n;
                }

            out[i] = sum / n;
        }

        return out;
    };

    if (smoothSteps)
        delta = average (delta, 2); // about 50 ms
    weight = average (weight, 1);   // about 30 ms ramps at voicing edges

    std::vector<float> ratio;

    for (juce::int64 frame = 0; frame < frames; frame += hop)
    {
        const auto at = std::clamp (track.indexOf (frame), 0.0, static_cast<double> (track.size() - 1));
        const auto i = static_cast<size_t> (at);
        const auto j = std::min (i + 1, track.size() - 1);
        const auto f = at - static_cast<double> (i);
        const auto d = (delta[i] + (delta[j] - delta[i]) * f) * (weight[i] + (weight[j] - weight[i]) * f);
        ratio.push_back (static_cast<float> (std::pow (2.0, d / 12.0)));
    }

    return ratio;
}
} // namespace

// The shifter trial (section 7): renders hand-made corrections of DAWG_SHIFT_INPUT (a file or a folder) into
// DAWG_SHIFT_OUTPUT as <name>.<trial>.wav, and prints how much faster than real time each render was.
// Run with: dawg_tests "[pitch-shift-tool]".
TEST_CASE ("pitch shifter trial", "[.][pitch-shift-tool]")
{
    const juce::File root (juce::SystemStats::getEnvironmentVariable ("DAWG_SHIFT_INPUT", {}));
    const juce::File outFolder (juce::SystemStats::getEnvironmentVariable ("DAWG_SHIFT_OUTPUT", {}));
    REQUIRE (root.exists());
    REQUIRE (outFolder.createDirectory());

    juce::String error;
    const auto model = loadPitchModel (pitchModelsFolder().getChildFile ("fcpe.onnx"), error, 4);
    REQUIRE (model != nullptr);
    // DAWG_SHIFT_VARIANTS: engines to compare, of r3-formants (the default), r3, r2-formants, r2 (R2 shifts too far here) and psola; with more than
    // one, each file is named <name>.<trial>.<variant>.wav. DAWG_SHIFT_TRIALS: only these trials.
    const auto variants = juce::StringArray::fromTokens (
        juce::SystemStats::getEnvironmentVariable ("DAWG_SHIFT_VARIANTS", "r3-formants"), ",", "");
    const auto wanted =
        juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("DAWG_SHIFT_TRIALS", ""), ",", "");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::Array<juce::File> files;

    if (root.isDirectory())
        files = root.findChildFiles (juce::File::findFiles, false, formats.getWildcardForAllFormats());
    else
        files.add (root);

    for (const auto& file : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            continue;

        juce::AudioBuffer<float> audio (static_cast<int> (std::min (reader->numChannels, 2u)),
                                        static_cast<int> (reader->lengthInSamples));
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, audio.getNumChannels() > 1);
        const auto sampleRate = reader->sampleRate;
        const auto analysis = analysePitch (audio, sampleRate, *model);

        // The two longest notes, for the moved-notes trial.
        std::vector<size_t> byLength (analysis.notes.size());
        std::iota (byLength.begin(), byLength.end(), size_t { 0 });
        std::sort (byLength.begin(), byLength.end(),
                   [&] (size_t a, size_t b)
                   {
                       return analysis.notes[a].end - analysis.notes[a].start
                              > analysis.notes[b].end - analysis.notes[b].start;
                   });
        const auto up = byLength.empty() ? SIZE_MAX : byLength[0];
        const auto down = byLength.size() < 2 ? SIZE_MAX : byLength[1];

        const std::vector<std::tuple<const char*, Trial, bool>> trials {
            // Rubber Band's own colouring, with nothing changed.
            { "unchanged", [] (const DetectedPitchNote&, size_t, double, double) { return 0.0; }, true },
            // Each note's centre snapped to the nearest semitone; shape, vibrato and detail kept.
            { "snapped", [] (const DetectedPitchNote& n, size_t, double, double)
              { return std::round (n.centre) - n.centre; }, true },
            // The longest note 3 semitones up, the next longest 3 down.
            { "moved", [up, down] (const DetectedPitchNote&, size_t i, double, double)
              { return i == up     ? 3.0
                       : i == down ? -3.0
                                   : 0.0; }, true },
            // Vibrato and jitter removed: each note follows its fitted shape.
            { "shape-only", [] (const DetectedPitchNote& n, size_t, double sung, double shape)
              { return n.centre + shape - sung; }, false },
            // Hard-tuned: each note flat on its snapped centre.
            { "flat", [] (const DetectedPitchNote& n, size_t, double sung, double)
              { return std::round (n.centre) - sung; }, false },
        };

        const auto hop = static_cast<int> (std::lround (0.01 * sampleRate));
        std::cout << file.getFileName() << ": " << analysis.notes.size() << " notes, "
                  << audio.getNumSamples() / sampleRate << " s" << std::endl;

        for (const auto& [name, trial, smoothSteps] : trials)
            for (const auto& variant : variants)
            {
                if (! wanted.isEmpty() && ! wanted.contains (name))
                    continue;

                const auto engine = variant == "psola" ? makePsolaPitchShifter (analysis.track)
                                                       : makeRubberBandPitchShifter (! variant.startsWith ("r2"),
                                                                                     variant.endsWith ("formants"));
                const auto ratio = ratioOf (analysis, trial, smoothSteps, audio.getNumSamples(), hop);
                const auto began = std::chrono::steady_clock::now();
                const auto shifted = engine->render (audio, sampleRate, ratio, hop, {});
                const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - began).count();
                std::cout << "  " << name << " " << variant << ": " << (audio.getNumSamples() / sampleRate) / seconds
                          << "x real time" << std::endl;

                const auto suffix = variants.size() > 1 ? "." + variant : juce::String();
                const auto target =
                    outFolder.getChildFile (file.getFileNameWithoutExtension() + "." + name + suffix + ".wav");
                std::ignore = target.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());
                REQUIRE (stream != nullptr);
                const auto options = juce::AudioFormatWriterOptions {}
                                         .withSampleRate (sampleRate)
                                         .withNumChannels (shifted.getNumChannels())
                                         .withBitsPerSample (24);
                auto writer = wav.createWriterFor (stream, options);
                REQUIRE (writer != nullptr);
                REQUIRE (writer->writeFromAudioSampleBuffer (shifted, 0, shifted.getNumSamples()));
            }
    }
}
