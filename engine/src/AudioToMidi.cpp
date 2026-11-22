#include "dawg/AudioToMidi.h"

#include "dawg/MidiInput.h"
#include "dawg/Onsets.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace dawg
{
namespace
{
constexpr double analysisRate = 12000.0; // YIN runs on the audio brought down to about this rate
constexpr double minHz = 50.0;
constexpr double maxHz = 1500.0;
constexpr double hopSeconds = 0.0053;
constexpr double yinThreshold = 0.15;
constexpr double minLevelDb = -50.0;
constexpr double splitSemitones = 0.6; // from the note's key
constexpr double splitSeconds = 0.04;
constexpr double minNoteSeconds = 0.04;
constexpr int maxGapFrames = 2; // unpitched frames inside a note that don't end it
constexpr double bendTolerance = 0.05;
constexpr double levelTolerance = 0.02;
constexpr double drumPeakSeconds = 0.01;
constexpr double drumMaxSeconds = 0.125;
constexpr double centroidSeconds = 0.03;
constexpr double kickBelowHz = 400.0;
constexpr double snareBelowHz = 3000.0;

[[nodiscard]] std::vector<float> monoOf (const juce::AudioBuffer<float>& audio)
{
    std::vector<float> mono (static_cast<size_t> (audio.getNumSamples()), 0.0f);
    const auto gain = 1.0f / static_cast<float> (std::max (1, audio.getNumChannels()));

    for (int c = 0; c < audio.getNumChannels(); ++c)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), audio.getReadPointer (c), gain,
                                                      audio.getNumSamples());

    return mono;
}

[[nodiscard]] double toDb (double gain) noexcept
{
    return gain > 1.0e-6 ? 20.0 * std::log10 (gain) : -120.0;
}

[[nodiscard]] double median (std::vector<double> values)
{
    const auto middle = values.begin() + static_cast<std::ptrdiff_t> (values.size() / 2);
    std::nth_element (values.begin(), middle, values.end());
    return *middle;
}

// The frames from first to last (inclusive) as a note.
[[nodiscard]] DetectedNote noteOf (const std::vector<PitchFrame>& frames, size_t first, size_t last, double half)
{
    DetectedNote note;
    note.start = frames[first].seconds - half;
    note.end = frames[last].seconds + half;

    std::vector<double> pitches;
    auto peakDb = -120.0;

    for (auto i = first; i <= last; ++i)
        if (frames[i].pitch > 0.0)
        {
            pitches.push_back (frames[i].pitch);
            peakDb = std::max (peakDb, frames[i].levelDb);
        }

    note.pitch = std::round (median (pitches));
    note.velocity = velocityOfPeak (peakDb);

    std::vector<std::pair<double, double>> bend;
    std::vector<std::pair<double, double>> level;

    for (auto i = first; i <= last; ++i)
        if (frames[i].pitch > 0.0)
        {
            bend.emplace_back (frames[i].seconds - note.start, frames[i].pitch - note.pitch);
            level.emplace_back (frames[i].seconds - note.start, std::pow (10.0, (frames[i].levelDb - peakDb) / 20.0));
        }

    note.bend = NoteRecorder::simplify (bend, bendTolerance);
    note.level = NoteRecorder::simplify (level, levelTolerance);
    return note;
}

// The spectral centroid in Hz of `count` samples from `start`, by power.
[[nodiscard]] double centroidOf (const std::vector<float>& mono, size_t start, size_t count, double sampleRate)
{
    constexpr int order = 11;
    constexpr int size = 1 << order;
    count = std::min ({ count, static_cast<size_t> (size), mono.size() - std::min (mono.size(), start) });

    if (count < 16)
        return 0.0;

    std::vector<float> data (2 * static_cast<size_t> (size), 0.0f);

    for (size_t i = 0; i < count; ++i)
    {
        const auto window = 0.5
                            - 0.5
                                  * std::cos (juce::MathConstants<double>::twoPi * static_cast<double> (i)
                                              / static_cast<double> (count - 1));
        data[i] = mono[start + i] * static_cast<float> (window);
    }

    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);
    auto weighted = 0.0;
    auto total = 0.0;

    for (int k = 1; k <= size / 2; ++k)
    {
        // Weighted by power, so a tone's peak outweighs a noise floor spread over every bin.
        const auto power = static_cast<double> (data[static_cast<size_t> (k)]) * data[static_cast<size_t> (k)];
        weighted += power * k * sampleRate / size;
        total += power;
    }

    return total > 0.0 ? weighted / total : 0.0;
}
} // namespace

double velocityOfPeak (double peakDb) noexcept
{
    const auto key = 1.0 + (std::clamp (peakDb, -40.0, 0.0) + 40.0) / 40.0 * 126.0;
    return std::round (key) / 127.0;
}

std::vector<PitchFrame> detectPitch (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto mono = monoOf (audio);
    const auto factor = std::max (1, static_cast<int> (std::lround (sampleRate / analysisRate)));
    const auto rate = sampleRate / factor;

    // Brought down by averaging, which is enough of a low-pass for the pitch range.
    std::vector<double> x (mono.size() / static_cast<size_t> (factor));

    for (size_t i = 0; i < x.size(); ++i)
    {
        auto sum = 0.0;
        for (int k = 0; k < factor; ++k)
            sum += mono[i * static_cast<size_t> (factor) + static_cast<size_t> (k)];
        x[i] = sum / factor;
    }

    const auto tauMin = std::max (2, static_cast<int> (std::floor (rate / maxHz)));
    const auto tauMax = static_cast<int> (std::ceil (rate / minHz));
    const auto window = tauMax;
    const auto hop = std::max (1, static_cast<int> (std::lround (rate * hopSeconds)));
    std::vector<double> difference (static_cast<size_t> (tauMax + 2), 0.0);
    std::vector<double> normalised (static_cast<size_t> (tauMax + 2), 1.0);
    std::vector<PitchFrame> frames;

    for (size_t p = 0; p + static_cast<size_t> (window + tauMax + 1) < x.size(); p += static_cast<size_t> (hop))
    {
        PitchFrame frame;
        frame.seconds = (static_cast<double> (p) + window / 2.0) / rate;

        // The level, at the full rate.
        auto energy = 0.0;
        const auto from = p * static_cast<size_t> (factor);
        const auto count = static_cast<size_t> (window) * static_cast<size_t> (factor);

        for (auto i = from; i < from + count; ++i)
            energy += static_cast<double> (mono[i]) * mono[i];

        frame.levelDb = toDb (std::sqrt (energy / static_cast<double> (count)));

        if (frame.levelDb < minLevelDb)
        {
            frames.push_back (frame);
            continue;
        }

        // The difference function and its cumulative mean normalisation.
        auto running = 0.0;

        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            auto sum = 0.0;

            for (int j = 0; j < window; ++j)
            {
                const auto d = x[p + static_cast<size_t> (j)] - x[p + static_cast<size_t> (j + tau)];
                sum += d * d;
            }

            difference[static_cast<size_t> (tau)] = sum;
            running += sum;
            normalised[static_cast<size_t> (tau)] = running > 0.0 ? sum * tau / running : 1.0;
        }

        // The first dip below the threshold, down to its bottom.
        auto tau = tauMin;

        while (tau <= tauMax && normalised[static_cast<size_t> (tau)] >= yinThreshold)
            ++tau;

        if (tau > tauMax)
        {
            frames.push_back (frame);
            continue;
        }

        while (tau < tauMax && normalised[static_cast<size_t> (tau) + 1] < normalised[static_cast<size_t> (tau)])
            ++tau;

        // Parabolic interpolation of the dip.
        const auto a = difference[static_cast<size_t> (tau - 1)];
        const auto b = difference[static_cast<size_t> (tau)];
        const auto c = difference[static_cast<size_t> (tau) + 1];
        const auto curvature = a - 2.0 * b + c;
        const auto exact =
            static_cast<double> (tau) + (std::abs (curvature) > 1.0e-12 ? 0.5 * (a - c) / curvature : 0.0);
        frame.pitch = 69.0 + 12.0 * std::log2 (rate / exact / 440.0);
        frames.push_back (frame);
    }

    return frames;
}

std::vector<DetectedNote> pitchNotes (const std::vector<PitchFrame>& frames)
{
    std::vector<DetectedNote> notes;

    if (frames.empty())
        return notes;

    const auto half = frames.size() > 1 ? (frames[1].seconds - frames[0].seconds) / 2.0 : hopSeconds / 2.0;
    std::vector<double> pitches; // of the note so far
    size_t first = 0;
    size_t last = 0;
    auto open = false;
    auto gap = 0;
    size_t deviation = 0;
    auto deviating = false;

    const auto close = [&]
    {
        if (open && frames[last].seconds - frames[first].seconds + 2.0 * half >= minNoteSeconds)
            notes.push_back (noteOf (frames, first, last, half));

        open = false;
        deviating = false;
        pitches.clear();
    };

    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto& frame = frames[i];

        if (frame.pitch <= 0.0)
        {
            if (open && ++gap > maxGapFrames)
                close();

            continue;
        }

        gap = 0;

        if (! open)
        {
            open = true;
            first = last = i;
            pitches = { frame.pitch };
            continue;
        }

        // A pitch that stays away from the note's median for long enough starts a new note where it left.
        // Against the note's key (its median, rounded), so vibrato around it never splits it.
        if (std::abs (frame.pitch - std::round (median (pitches))) > splitSemitones)
        {
            if (! deviating)
            {
                deviating = true;
                deviation = i;
            }

            if (frame.seconds - frames[deviation].seconds >= splitSeconds)
            {
                const auto restart = deviation;
                last = restart - 1;
                close();
                open = true;
                first = restart;
                last = i;

                for (auto k = restart; k <= i; ++k)
                    if (frames[k].pitch > 0.0)
                        pitches.push_back (frames[k].pitch);

                continue;
            }
        }
        else
        {
            deviating = false;
            pitches.push_back (frame.pitch);
        }

        last = i;
    }

    close();
    return notes;
}

std::vector<DetectedNote> drumNotes (const juce::AudioBuffer<float>& audio, double sampleRate, double sensitivity,
                                     bool split)
{
    const auto mono = monoOf (audio);
    const auto threshold = 1.0 - std::clamp (sensitivity, 0.0, 1.0);
    std::vector<juce::int64> hits;

    for (const auto& onset : detectOnsets (audio, sampleRate))
        if (static_cast<double> (onset.strength) >= threshold - 1.0e-9)
            hits.push_back (onset.frame);

    std::vector<DetectedNote> notes;
    const auto total = static_cast<juce::int64> (mono.size());

    for (size_t i = 0; i < hits.size(); ++i)
    {
        const auto frame = hits[i];
        const auto next = i + 1 < hits.size() ? hits[i + 1] : total;
        DetectedNote note;
        note.start = static_cast<double> (frame) / sampleRate;
        note.end = std::min (static_cast<double> (next) / sampleRate, note.start + drumMaxSeconds);

        auto peak = 0.0f;
        const auto peakEnd = std::min (total, frame + static_cast<juce::int64> (drumPeakSeconds * sampleRate));

        for (auto s = frame; s < peakEnd; ++s)
            peak = std::max (peak, std::abs (mono[static_cast<size_t> (s)]));

        note.velocity = velocityOfPeak (toDb (peak));
        note.pitch = 36.0;

        if (split)
        {
            const auto length = std::min (next - frame, static_cast<juce::int64> (centroidSeconds * sampleRate));
            const auto centroid =
                centroidOf (mono, static_cast<size_t> (frame), static_cast<size_t> (length), sampleRate);
            note.pitch = 42.0; // hats

            if (centroid < kickBelowHz)
                note.pitch = 36.0;
            else if (centroid < snareBelowHz)
                note.pitch = 38.0;
        }

        notes.push_back (note);
    }

    return notes;
}
} // namespace dawg
