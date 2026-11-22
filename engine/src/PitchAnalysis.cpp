#include "dawg/PitchAnalysis.h"

#include "dawg/MidiInput.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace dawg
{
namespace
{
constexpr float nan = std::numeric_limits<float>::quiet_NaN();

// Preparation (section 3.1).
constexpr double highPassHz = 40.0;
constexpr double levelWindowSeconds = 0.025;

// Viterbi (section 3.3). A voiced frame's pitch may move up to 12 bins (240 cents for CREPE) to the next frame, with
// a triangular weight, which follows a fast scoop; a larger jump has to pass through the unvoiced state.
constexpr int maxStepBins = 12;
constexpr double switchProbability = 0.01;
constexpr double silentDb = -60.0;
constexpr int weightedBins = 4;     // the pitch is the salience-weighted mean of the path's bin and 4 on each side
constexpr double minShare = 1.0e-3; // a voiced bin's observation never drops below this share of the peak's, so a
                                    // single frame's octave error costs less than leaving and re-entering voicing

// Segmentation (section 3.4).
constexpr double bridgeSeconds = 0.015;
constexpr double stepSemitones = 0.5;
constexpr double stepSeconds = 0.04;
constexpr double stepAverageSeconds = 0.15; // steps are found on the pitch averaged over about one vibrato cycle
constexpr double minNoteSeconds = 0.04;
constexpr double glidePieceSeconds = 0.15;  // a piece this short that moves this far is part of a glide, not a note:
constexpr double glidePieceSemitones = 1.0; // it becomes the next note's scoop (or the previous one's fall)
constexpr double onsetMarginSeconds = 0.06;
constexpr double onsetDipDb = 6.0;
constexpr double onsetDipSeconds = 0.05;
constexpr double leadMinDb = -50.0;
constexpr double leadMaxSeconds = 0.3;
constexpr double leadMinSeconds = 0.02;
constexpr double centreTrim = 0.15; // the centre leaves out 15 % of the voiced frames at each end

// Vibrato: 4-8 Hz, at least 15 cents deep, on notes of at least 300 ms.
constexpr double vibratoMinHz = 4.0;
constexpr double vibratoMaxHz = 8.0;
constexpr double vibratoMinCents = 15.0;
constexpr double vibratoMinSeconds = 0.3;
constexpr double vibratoMinCorrelation = 0.3;

// Shape fit (section 3.5). Tuned on the owner's free singing (2026-11-16): at 15 cents and 6 nodes per second held
// notes got 7-14 nodes that followed every wobble; the shape is meant to carry only the scoop, drift and fall.
constexpr double shapeTolerance = 0.25;
constexpr double maxNodesPerSecond = 4.0;
constexpr size_t minNodeBudget = 4;         // a short note still gets nodes for its turning points (a smooth run
                                            // never passes its nodes, so a dip needs a node of its own)
constexpr double maxShapeTolerance = 0.5;   // the budget never loosens the fit beyond this
constexpr double shapeMedianSeconds = 0.09; // a median keeps a monotonic scoop as it is and removes the wobble

[[nodiscard]] float toDb (double rms) noexcept
{
    return static_cast<float> (rms > 1.0e-6 ? 20.0 * std::log10 (rms) : -120.0);
}

[[nodiscard]] double median (std::vector<double> values)
{
    const auto middle = values.begin() + static_cast<std::ptrdiff_t> (values.size() / 2);
    std::nth_element (values.begin(), middle, values.end());
    return *middle;
}

// A centred moving average, the window shrinking at the ends.
[[nodiscard]] std::vector<double> movingAverage (const std::vector<double>& values, int radius)
{
    const auto count = static_cast<int> (values.size());
    std::vector<double> sums (values.size() + 1, 0.0);

    for (int i = 0; i < count; ++i)
        sums[static_cast<size_t> (i + 1)] = sums[static_cast<size_t> (i)] + values[static_cast<size_t> (i)];

    std::vector<double> result (values.size());

    for (int i = 0; i < count; ++i)
    {
        const auto from = std::max (0, i - radius);
        const auto to = std::min (count, i + radius + 1);
        result[static_cast<size_t> (i)] =
            (sums[static_cast<size_t> (to)] - sums[static_cast<size_t> (from)]) / (to - from);
    }

    return result;
}

// A centred running median over 2 * radius + 1 values, the window shrinking symmetrically at the ends, so the end
// values stay put.
[[nodiscard]] std::vector<double> runningMedian (const std::vector<double>& values, int radius)
{
    const auto count = static_cast<int> (values.size());
    std::vector<double> result (values.size());

    for (int i = 0; i < count; ++i)
    {
        const auto r = std::min ({ radius, i, count - 1 - i });
        const auto from = i - r;
        const auto to = i + r + 1;
        result[static_cast<size_t> (i)] = median ({ values.begin() + from, values.begin() + to });
    }

    return result;
}

// The track's pitch with short unvoiced gaps bridged by straight lines; frames that stay unvoiced are NaN.
[[nodiscard]] std::vector<double> bridgedPitch (const PitchTrack& track)
{
    const auto count = static_cast<int> (track.size());
    const auto maxGap = static_cast<int> (std::floor (bridgeSeconds / track.hopSeconds + 1.0e-9));
    std::vector<double> pitch (track.size());

    for (int i = 0; i < count; ++i)
        pitch[static_cast<size_t> (i)] = track.pitch[static_cast<size_t> (i)];

    for (int i = 0; i < count;)
    {
        if (! std::isnan (pitch[static_cast<size_t> (i)]))
        {
            ++i;
            continue;
        }

        auto end = i;
        while (end < count && std::isnan (pitch[static_cast<size_t> (end)]))
            ++end;

        if (i > 0 && end < count && end - i <= maxGap)
        {
            const auto a = pitch[static_cast<size_t> (i - 1)];
            const auto b = pitch[static_cast<size_t> (end)];

            for (auto k = i; k < end; ++k)
                pitch[static_cast<size_t> (k)] = a + (b - a) * (k - i + 1) / (end - i + 1);
        }

        i = end;
    }

    return pitch;
}

// Frames first..last (inclusive) of the track whose middles lie in [start, end) audio frames.
[[nodiscard]] std::pair<int, int> framesOf (const PitchTrack& track, juce::int64 start, juce::int64 end)
{
    const auto count = static_cast<int> (track.size());
    const auto first = std::max (0, static_cast<int> (std::ceil (track.indexOf (start) - 1.0e-9)));
    auto last = std::min (count - 1, static_cast<int> (std::floor (track.indexOf (end) - 1.0e-9)));

    while (last >= first && track.audioFrameOf (last) >= end)
        --last;

    return { first, last };
}

// The level-weighted mean pitch over the middle 70 % of the voiced frames first..last.
[[nodiscard]] double centreOf (const std::vector<double>& pitch, const PitchTrack& track, int first, int last)
{
    std::vector<int> voiced;

    for (auto i = first; i <= last; ++i)
        if (! std::isnan (pitch[static_cast<size_t> (i)]))
            voiced.push_back (i);

    if (voiced.empty())
        return std::numeric_limits<double>::quiet_NaN();

    const auto trim = voiced.size() >= 5 ? static_cast<size_t> (centreTrim * static_cast<double> (voiced.size())) : 0;
    auto weighted = 0.0;
    auto total = 0.0;

    for (auto k = trim; k < voiced.size() - trim; ++k)
    {
        const auto i = static_cast<size_t> (voiced[k]);
        const auto weight = std::pow (10.0, track.levelDb[i] / 20.0) + 1.0e-9;
        weighted += weight * pitch[i];
        total += weight;
    }

    return weighted / total;
}

struct Vibrato
{
    double rate = 0.0;
    double depth = 0.0;
};

// The deviation from the centre of frames first..last, with unvoiced frames bridged by straight lines (ends held).
[[nodiscard]] std::vector<double> deviationOf (const std::vector<double>& pitch, int first, int last, double centre)
{
    std::vector<double> dev;

    for (auto i = first; i <= last; ++i)
        dev.push_back (pitch[static_cast<size_t> (i)] - centre);

    const auto count = static_cast<int> (dev.size());
    auto previous = -1;

    for (int i = 0; i <= count; ++i)
    {
        if (i < count && std::isnan (dev[static_cast<size_t> (i)]))
            continue;

        for (auto k = previous + 1; k < i; ++k)
        {
            const auto a =
                previous >= 0 ? dev[static_cast<size_t> (previous)] : (i < count ? dev[static_cast<size_t> (i)] : 0.0);
            const auto b = i < count ? dev[static_cast<size_t> (i)] : a;
            dev[static_cast<size_t> (k)] = previous >= 0 ? a + (b - a) * (k - previous) / (i - previous) : b;
        }

        previous = i;
    }

    return dev;
}

// The vibrato of a deviation (section 3.4, step 6): band-passed to about 4-8 Hz, its autocorrelation gives the rate
// and its RMS the depth.
[[nodiscard]] Vibrato vibratoOf (const std::vector<double>& dev, double hopSeconds)
{
    if (static_cast<double> (dev.size()) * hopSeconds < vibratoMinSeconds)
        return {};

    // High-pass: minus a 0.5 s moving average, whose response is close to zero from 4 to 8 Hz. Low-pass: a 30 ms one.
    const auto slow = movingAverage (dev, static_cast<int> (std::lround (0.25 / hopSeconds)));
    std::vector<double> band (dev.size());

    for (size_t i = 0; i < dev.size(); ++i)
        band[i] = dev[i] - slow[i];

    band = movingAverage (band, 1);
    const auto count = static_cast<int> (band.size());

    auto correlation = [&] (int lag)
    {
        auto sum = 0.0;
        for (auto i = 0; i + lag < count; ++i)
            sum += band[static_cast<size_t> (i)] * band[static_cast<size_t> (i + lag)];
        return sum / (count - lag);
    };

    const auto energy = correlation (0);

    if (energy <= 0.0)
        return {};

    const auto minLag = std::max (2, static_cast<int> (std::floor (1.0 / (vibratoMaxHz * hopSeconds))));
    const auto maxLag = std::min (count - 2, static_cast<int> (std::ceil (1.0 / (vibratoMinHz * hopSeconds))));
    auto bestLag = -1;
    auto best = vibratoMinCorrelation;

    for (auto lag = minLag; lag <= maxLag; ++lag)
    {
        const auto r = correlation (lag) / energy;

        if (r > best)
        {
            best = r;
            bestLag = lag;
        }
    }

    if (bestLag < 0)
        return {};

    // Parabolic interpolation of the peak, for a rate between whole lags.
    const auto a = correlation (bestLag - 1);
    const auto b = correlation (bestLag);
    const auto c = correlation (bestLag + 1);
    const auto curvature = a - 2.0 * b + c;
    const auto offset = curvature < 0.0 ? std::clamp (0.5 * (a - c) / curvature, -0.5, 0.5) : 0.0;

    Vibrato vibrato;
    vibrato.depth = std::sqrt (2.0 * energy) * 100.0;
    vibrato.rate = 1.0 / ((bestLag + offset) * hopSeconds);

    if (vibrato.depth < vibratoMinCents)
        return {};

    return vibrato;
}

// The slow movement of a deviation: with vibrato the midline between the curves through its peaks and through its
// troughs, without it a 90 ms median.
[[nodiscard]] std::vector<double> slowMovementOf (const std::vector<double>& dev, bool vibrato, double hopSeconds)
{
    if (! vibrato)
        return runningMedian (dev,
                              std::max (1, static_cast<int> (std::lround (0.5 * shapeMedianSeconds / hopSeconds))));

    const auto smooth = runningMedian (dev, 1);

    if (smooth.size() < 3)
        return smooth;

    const auto count = static_cast<int> (smooth.size());
    std::vector<int> peaks { 0 };
    std::vector<int> troughs { 0 };

    for (int i = 1; i + 1 < count; ++i)
    {
        const auto here = smooth[static_cast<size_t> (i)];

        if (here > smooth[static_cast<size_t> (i - 1)] && here >= smooth[static_cast<size_t> (i + 1)])
            peaks.push_back (i);
        else if (here < smooth[static_cast<size_t> (i - 1)] && here <= smooth[static_cast<size_t> (i + 1)])
            troughs.push_back (i);
    }

    peaks.push_back (count - 1);
    troughs.push_back (count - 1);

    auto envelope = [&] (const std::vector<int>& anchors)
    {
        std::vector<double> result (smooth.size());

        for (size_t k = 0; k + 1 < anchors.size(); ++k)
        {
            const auto from = anchors[k];
            const auto to = anchors[k + 1];
            const auto a = smooth[static_cast<size_t> (from)];
            const auto b = smooth[static_cast<size_t> (to)];

            for (auto i = from; i <= to; ++i)
                result[static_cast<size_t> (i)] = to > from ? a + (b - a) * (i - from) / (to - from) : a;
        }

        return result;
    };

    const auto upper = envelope (peaks);
    const auto lower = envelope (troughs);
    std::vector<double> mid (smooth.size());

    for (size_t i = 0; i < mid.size(); ++i)
        mid[i] = 0.5 * (upper[i] + lower[i]);

    return mid;
}

struct Sample
{
    Ticks time;
    double value;
};

// A curve of smooth segments through the given samples' times with the given values.
[[nodiscard]] Curve smoothCurveOf (const std::vector<Sample>& samples, const std::vector<size_t>& nodes,
                                   const std::vector<double>& values)
{
    Curve curve;

    for (size_t k = 0; k < nodes.size(); ++k)
        curve.push_back (
            { samples[nodes[k]].time, values[k], k + 1 < nodes.size() ? CurveShape::smooth : CurveShape::linear });

    return curve;
}

struct SmoothFit
{
    Curve curve;
    double maxError = 0.0;
};

// The squared and the largest error of a curve against the samples.
[[nodiscard]] std::pair<double, double> errorOf (const Curve& curve, const std::vector<Sample>& samples)
{
    auto squared = 0.0;
    auto largest = 0.0;

    for (const auto& sample : samples)
    {
        const auto error = evaluateCurve (curve, static_cast<double> (sample.time), 0.0) - sample.value;
        squared += error * error;
        largest = std::max (largest, std::abs (error));
    }

    return { squared, largest };
}

// The node values that fit the samples best (least squares), by coordinate descent: each node in turn moves to the
// bottom of the parabola through the error at its value and 5 cents either side, at most 30 cents a step. The curve
// needn't pass through noisy samples, so fewer nodes reach the tolerance.
[[nodiscard]] SmoothFit refineValues (const std::vector<Sample>& samples, const std::vector<size_t>& nodes)
{
    std::vector<double> values;

    for (const auto node : nodes)
        values.push_back (samples[node].value);

    auto squaredError = [&] { return errorOf (smoothCurveOf (samples, nodes, values), samples).first; };

    for (int round = 0; round < 4; ++round)
        for (auto& value : values)
        {
            constexpr auto step = 0.05;
            const auto original = value;
            const auto here = squaredError();
            value = original + step;
            const auto up = squaredError();
            value = original - step;
            const auto down = squaredError();
            const auto curvature = up + down - 2.0 * here;
            value =
                curvature > 0.0 ? original + std::clamp (step * (down - up) / (2.0 * curvature), -0.3, 0.3) : original;

            if (squaredError() > here)
                value = original;
        }

    SmoothFit fit;
    fit.curve = smoothCurveOf (samples, nodes, values);
    fit.maxError = errorOf (fit.curve, samples).second;
    return fit;
}

// Fits smooth segments to samples (section 3.5): Ramer-Douglas-Peucker nodes, least-squares values, then thinning
// while the fit stays within the tolerance.
[[nodiscard]] SmoothFit fitSmooth (const std::vector<Sample>& samples, double tolerance)
{
    std::vector<std::pair<double, double>> points;

    for (const auto& s : samples)
        points.emplace_back (static_cast<double> (s.time), s.value);

    std::vector<size_t> nodes;
    size_t next = 0;

    for (const auto& [time, value] : NoteRecorder::simplify (points, tolerance))
    {
        while (next < samples.size() && static_cast<double> (samples[next].time) < time)
            ++next;

        if (next < samples.size() && (nodes.empty() || nodes.back() != next))
            nodes.push_back (next);
    }

    auto best = refineValues (samples, nodes);

    for (auto removed = true; removed && nodes.size() > 2;)
    {
        removed = false;

        for (size_t k = 1; k + 1 < nodes.size(); ++k)
        {
            auto fewer = nodes;
            fewer.erase (fewer.begin() + static_cast<std::ptrdiff_t> (k));
            auto fit = refineValues (samples, fewer);

            if (fit.maxError <= tolerance)
            {
                nodes = std::move (fewer);
                best = std::move (fit);
                removed = true;
                break;
            }
        }
    }

    return best;
}

// The shape of frames first..last (section 3.5).
[[nodiscard]] Curve shapeOf (const std::vector<double>& pitch, const PitchTrack& track, int first, int last,
                             double centre, juce::int64 start)
{
    if (first > last || std::isnan (centre))
        return {};

    const auto dev = deviationOf (pitch, first, last, centre);
    const auto slow = slowMovementOf (dev, vibratoOf (dev, track.hopSeconds).depth > 0.0, track.hopSeconds);
    std::vector<Sample> samples;

    for (auto i = first; i <= last; ++i)
    {
        const auto time = static_cast<Ticks> (track.audioFrameOf (i) - start);

        if (samples.empty() || samples.back().time < time)
            samples.push_back ({ time, slow[static_cast<size_t> (i - first)] });
    }

    if (samples.size() < 2)
        return { CurvePoint { samples.front().time, samples.front().value } };

    const auto seconds = static_cast<double> (last - first + 1) * track.hopSeconds;
    const auto maxNodes = std::max (minNodeBudget, static_cast<size_t> (std::ceil (maxNodesPerSecond * seconds)));
    auto tolerance = shapeTolerance;
    auto fit = fitSmooth (samples, tolerance);

    while (fit.curve.size() > maxNodes && tolerance < maxShapeTolerance)
    {
        tolerance = std::min (maxShapeTolerance, tolerance * 1.5);
        fit = fitSmooth (samples, tolerance);
    }

    return std::move (fit.curve);
}

struct Span
{
    int first;
    int last;           // inclusive
    bool onset = false; // split from the note before at an onset (a repeated note)
};
} // namespace

juce::int64 PitchTrack::audioFrameOf (double index) const noexcept
{
    return static_cast<juce::int64> (std::llround ((startSeconds + index * hopSeconds) * sampleRate));
}

double PitchTrack::indexOf (juce::int64 audioFrame) const noexcept
{
    return (static_cast<double> (audioFrame) / sampleRate - startSeconds) / hopSeconds;
}

std::vector<float> preparePitchAudio (const juce::AudioBuffer<float>& audio, double sampleRate, double modelRate)
{
    const auto length = audio.getNumSamples();
    const auto channels = std::min (2, audio.getNumChannels());
    std::vector<float> mono (static_cast<size_t> (length) + 8, 0.0f);

    for (int c = 0; c < channels; ++c)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), audio.getReadPointer (c),
                                                      1.0f / static_cast<float> (channels), length);

    juce::IIRFilter highPass;
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, highPassHz));
    highPass.processSamples (mono.data(), length);

    if (std::abs (sampleRate - modelRate) < 1.0e-6)
    {
        mono.resize (static_cast<size_t> (length));
        return mono;
    }

    if (sampleRate > modelRate)
    {
        // An 8th-order Butterworth low-pass at 90 % of the new Nyquist frequency against aliasing.
        for (const auto q : { 0.5098, 0.6013, 0.9000, 2.5629 })
        {
            juce::IIRFilter lowPass;
            lowPass.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, 0.45 * modelRate, q));
            lowPass.processSamples (mono.data(), length);
        }
    }

    const auto ratio = sampleRate / modelRate;
    const auto count = static_cast<int> (std::floor (static_cast<double> (length) / ratio));
    std::vector<float> result (static_cast<size_t> (std::max (0, count)), 0.0f);
    juce::LagrangeInterpolator interpolator;
    [[maybe_unused]] const auto used = interpolator.process (ratio, mono.data(), result.data(), count);
    return result;
}

PitchTrack decodePitch (const PitchSalience& salience, float threshold, const std::vector<float>& levelDb,
                        double sampleRate)
{
    PitchTrack track;
    track.sampleRate = sampleRate;
    track.startSeconds = salience.startSeconds;
    track.hopSeconds = salience.hopSeconds;

    const auto frames = salience.frames();
    const auto bins = static_cast<int> (salience.binPitches.size());

    if (frames == 0 || bins == 0)
        return track;

    track.pitch.assign (frames, nan);
    track.voicedProb.assign (frames, 0.0f);
    track.levelDb.assign (frames, -120.0f);

    for (size_t f = 0; f < frames && f < levelDb.size(); ++f)
        track.levelDb[f] = levelDb[f];

    // How likely a frame is voiced: its peak salience p, raised to the power that maps the model's threshold to 0.5.
    const auto exponent = std::log (0.5) / std::log (std::clamp (static_cast<double> (threshold), 0.01, 0.99));

    for (size_t f = 0; f < frames; ++f)
    {
        const auto* values = salience.frame (f);
        const auto peak = static_cast<double> (*std::max_element (values, values + bins));
        auto voiced = std::pow (std::clamp (peak, 1.0e-4, 1.0), exponent);

        if (! levelDb.empty() && f < levelDb.size() && levelDb[f] < silentDb)
            voiced = 0.01;

        track.voicedProb[f] = static_cast<float> (voiced);
    }

    // Viterbi over the bins (voiced) and one unvoiced state (index `bins`), in the log domain. Scores aren't
    // normalised probabilities: a steady voiced frame and a steady unvoiced one cost about the same, so the
    // observations decide.
    const auto unvoiced = bins;
    const auto states = static_cast<size_t> (bins + 1);
    const auto logStay = std::log (1.0 - switchProbability);
    const auto logSwitch = std::log (switchProbability);
    std::array<double, maxStepBins + 1> logStep {};

    for (int d = 0; d <= maxStepBins; ++d)
        logStep[static_cast<size_t> (d)] = logStay + std::log (1.0 - static_cast<double> (d) / (maxStepBins + 1));

    std::vector<double> score (states);
    std::vector<double> next (states);
    std::vector<std::int16_t> from (frames * states);

    // One frame's log observations, for every state.
    std::vector<double> observed (states);

    auto observe = [&] (size_t f)
    {
        const auto voiced = static_cast<double> (track.voicedProb[f]);
        const auto* values = salience.frame (f);
        const auto peak = static_cast<double> (*std::max_element (values, values + bins));

        for (int s = 0; s < bins; ++s)
        {
            const auto share = peak > 0.0 ? std::max (0.0, static_cast<double> (values[s])) / peak : 0.0;
            observed[static_cast<size_t> (s)] = std::log (std::max (1.0e-9, voiced * std::max (minShare, share)));
        }

        observed[static_cast<size_t> (unvoiced)] = std::log (std::max (1.0e-9, 1.0 - voiced));
    };

    observe (0);
    score = observed;

    for (size_t f = 1; f < frames; ++f)
    {
        observe (f);

        // The best voiced state of the previous frame, for entering voicing anywhere.
        auto bestVoiced = 0;
        for (int s = 1; s < bins; ++s)
            if (score[static_cast<size_t> (s)] > score[static_cast<size_t> (bestVoiced)])
                bestVoiced = s;

        for (int s = 0; s < bins; ++s)
        {
            auto best = score[static_cast<size_t> (unvoiced)] + logSwitch;
            auto origin = unvoiced;

            for (auto p = std::max (0, s - maxStepBins); p <= std::min (bins - 1, s + maxStepBins); ++p)
            {
                const auto candidate = score[static_cast<size_t> (p)] + logStep[static_cast<size_t> (std::abs (s - p))];

                if (candidate > best)
                {
                    best = candidate;
                    origin = p;
                }
            }

            next[static_cast<size_t> (s)] = best + observed[static_cast<size_t> (s)];
            from[f * states + static_cast<size_t> (s)] = static_cast<std::int16_t> (origin);
        }

        const auto stay = score[static_cast<size_t> (unvoiced)] + logStay;
        const auto leave = score[static_cast<size_t> (bestVoiced)] + logSwitch;
        next[static_cast<size_t> (unvoiced)] = std::max (stay, leave) + observed[static_cast<size_t> (unvoiced)];
        from[f * states + static_cast<size_t> (unvoiced)] =
            static_cast<std::int16_t> (stay >= leave ? unvoiced : bestVoiced);
        std::swap (score, next);

        // Keep the scores near zero, so long files don't lose precision.
        const auto top = *std::max_element (score.begin(), score.end());
        for (auto& value : score)
            value -= top;
    }

    auto state = static_cast<int> (std::max_element (score.begin(), score.end()) - score.begin());

    for (auto f = frames; f-- > 0;)
    {
        if (state != unvoiced)
        {
            const auto* values = salience.frame (f);
            auto weighted = 0.0;
            auto total = 0.0;

            for (auto k = std::max (0, state - weightedBins); k <= std::min (bins - 1, state + weightedBins); ++k)
            {
                const auto weight = std::max (0.0, static_cast<double> (values[k]));
                weighted += weight * salience.binPitches[static_cast<size_t> (k)];
                total += weight;
            }

            track.pitch[f] =
                static_cast<float> (total > 0.0 ? weighted / total : salience.binPitches[static_cast<size_t> (state)]);
        }

        if (f > 0)
            state = from[f * states + static_cast<size_t> (state)];
    }

    return track;
}

std::vector<DetectedPitchNote> findPitchNotes (const PitchTrack& track, const std::vector<Onset>& onsets)
{
    const auto count = static_cast<int> (track.size());
    const auto pitch = bridgedPitch (track);
    const auto hop = track.hopSeconds;
    const auto stepFrames = std::max (1, static_cast<int> (std::lround (stepSeconds / hop)));
    const auto minFrames = std::max (1, static_cast<int> (std::lround (minNoteSeconds / hop)));
    std::vector<Span> spans;

    // 1. Voiced regions; 2. split at held pitch steps, at their steepest point.
    for (int i = 0; i < count;)
    {
        if (std::isnan (pitch[static_cast<size_t> (i)]))
        {
            ++i;
            continue;
        }

        auto end = i;
        while (end < count && ! std::isnan (pitch[static_cast<size_t> (end)]))
            ++end;

        // A step is a held move of the pitch averaged over about one vibrato cycle (so vibrato isn't one) away from
        // the median of the note so far (its last 0.5 s, so a slow drift isn't one either).
        const std::vector<double> region (pitch.begin() + i, pitch.begin() + end);
        const auto smooth = runningMedian (region, 1);
        // The average still holds some of the previous note just after a split, so the new note's reference starts
        // half an averaging window later.
        const auto settle = std::max (1, static_cast<int> (std::lround (0.5 * stepAverageSeconds / hop)));
        const auto slow = movingAverage (region, settle);
        const auto history = static_cast<int> (std::lround (0.5 / hop));
        auto noteStart = 0;
        auto runStart = -1;

        for (int k = 0; k < static_cast<int> (smooth.size()); ++k)
        {
            const auto settled = noteStart == 0 ? 0 : noteStart + settle;

            if (k < settled)
                continue;

            const auto from = std::max (settled, k - history);
            const auto reference =
                k > from ? median ({ slow.begin() + from, slow.begin() + k }) : slow[static_cast<size_t> (k)];

            if (std::abs (slow[static_cast<size_t> (k)] - reference) <= stepSemitones)
            {
                runStart = -1;
                continue;
            }

            if (runStart < 0)
                runStart = k;

            if (k - runStart + 1 >= stepFrames)
            {
                auto split = runStart;
                auto steepest = 0.0;

                // The average moves up to half a window before the pitch does, so the search looks as far ahead.
                const auto searchEnd = std::min (static_cast<int> (smooth.size()) - 1, k + settle);

                for (auto j = std::max (noteStart + 1, runStart - 2 * stepFrames); j <= searchEnd; ++j)
                {
                    const auto slope = std::abs (smooth[static_cast<size_t> (j)] - smooth[static_cast<size_t> (j - 1)]);

                    if (slope > steepest)
                    {
                        steepest = slope;
                        split = j;
                    }
                }

                spans.push_back ({ i + noteStart, i + split - 1 });
                noteStart = split;
                runStart = -1;
            }
        }

        spans.push_back ({ i + noteStart, end - 1 });
        i = end;
    }

    // 2b. Split at onsets with a level dip before them: repeated notes.
    const auto margin = static_cast<int> (std::lround (onsetMarginSeconds / hop));
    const auto dip = std::max (1, static_cast<int> (std::lround (onsetDipSeconds / hop)));

    for (const auto& onset : onsets)
    {
        const auto at = static_cast<int> (std::lround (track.indexOf (onset.frame)));

        for (size_t s = 0; s < spans.size(); ++s)
        {
            const auto span = spans[s];

            if (at - span.first < margin || span.last - at < margin)
                continue;

            auto before = std::numeric_limits<float>::max();
            auto after = std::numeric_limits<float>::lowest();

            for (auto k = std::max (0, at - dip); k <= at; ++k)
                before = std::min (before, track.levelDb[static_cast<size_t> (k)]);

            for (auto k = at; k <= std::min (count - 1, at + dip); ++k)
                after = std::max (after, track.levelDb[static_cast<size_t> (k)]);

            if (after - before >= onsetDipDb)
            {
                spans[s].last = at - 1;
                spans.insert (spans.begin() + static_cast<std::ptrdiff_t> (s + 1), Span { at, span.last, true });
            }

            break;
        }
    }

    // Short pieces join the note they touch (a scoop belongs to its note), or go if they touch none.
    for (size_t s = 0; s < spans.size();)
    {
        if (spans[s].last - spans[s].first + 1 >= minFrames)
        {
            ++s;
            continue;
        }

        if (s > 0 && spans[s - 1].last + 1 == spans[s].first)
            spans[s - 1].last = spans[s].last;
        else if (s + 1 < spans.size() && spans[s].last + 1 == spans[s + 1].first)
            spans[s + 1].first = spans[s].first;

        spans.erase (spans.begin() + static_cast<std::ptrdiff_t> (s));
    }

    // Glide pieces: the bottom of a scoop or a dip between two notes, split off by the step rule.
    const auto glideFrames = static_cast<int> (std::lround (glidePieceSeconds / hop));

    for (size_t s = 0; s < spans.size();)
    {
        const auto& span = spans[s];
        auto low = std::numeric_limits<double>::max();
        auto high = std::numeric_limits<double>::lowest();

        for (auto k = span.first; k <= span.last; ++k)
        {
            low = std::min (low, pitch[static_cast<size_t> (k)]);
            high = std::max (high, pitch[static_cast<size_t> (k)]);
        }

        const auto touchesNext = s + 1 < spans.size() && span.last + 1 == spans[s + 1].first && ! spans[s + 1].onset;
        const auto touchesPrevious = s > 0 && spans[s - 1].last + 1 == span.first && ! span.onset;

        if (span.last - span.first + 1 > glideFrames || high - low < glidePieceSemitones
            || ! (touchesNext || touchesPrevious))
        {
            ++s;
            continue;
        }

        if (touchesNext)
            spans[s + 1].first = span.first;
        else
            spans[s - 1].last = span.last;

        spans.erase (spans.begin() + static_cast<std::ptrdiff_t> (s));
    }

    // Touching notes at the same pitch are one note, unless an onset split them: a pitch step found in a glide's
    // overshoot can land on the pitch it left.
    for (size_t s = 0; s + 1 < spans.size();)
    {
        const auto& a = spans[s];
        const auto& b = spans[s + 1];

        if (a.last + 1 == b.first && ! b.onset
            && std::abs (centreOf (pitch, track, a.first, a.last) - centreOf (pitch, track, b.first, b.last))
                   < stepSemitones)
        {
            spans[s].last = b.last;
            spans.erase (spans.begin() + static_cast<std::ptrdiff_t> (s + 1));
        }
        else
            ++s;
    }

    std::vector<DetectedPitchNote> notes;
    const auto leadMax = static_cast<int> (std::lround (leadMaxSeconds / hop));
    const auto leadMin = std::max (1, static_cast<int> (std::lround (leadMinSeconds / hop)));

    for (size_t s = 0; s < spans.size(); ++s)
    {
        const auto first = spans[s].first;
        const auto last = spans[s].last;
        DetectedPitchNote note;
        note.start = std::max<juce::int64> (0, track.audioFrameOf (first - 0.5));
        note.end = track.audioFrameOf (last + 0.5);
        note.leadStart = note.start;

        // 4. An unvoiced lead-in above -50 dBFS, up to 300 ms, after the previous note.
        const auto floor = s > 0 ? spans[s - 1].last + 1 : 0;
        auto lead = first;

        while (lead > floor && first - lead < leadMax && std::isnan (pitch[static_cast<size_t> (lead - 1)])
               && track.levelDb[static_cast<size_t> (lead - 1)] > leadMinDb)
            --lead;

        if (first - lead >= leadMin)
            note.leadStart = std::max<juce::int64> (0, track.audioFrameOf (lead - 0.5));

        // 5. The centre; 6. vibrato; the shape.
        note.centre = centreOf (pitch, track, first, last);
        const auto vibrato = vibratoOf (deviationOf (pitch, first, last, note.centre), hop);
        note.vibratoRate = vibrato.rate;
        note.vibratoDepth = vibrato.depth;
        note.shape = shapeOf (pitch, track, first, last, note.centre, note.start);
        notes.push_back (std::move (note));
    }

    return notes;
}

PitchAnalysis analysePitch (const juce::AudioBuffer<float>& audio, double sampleRate, const PitchModel& model,
                            const std::vector<Onset>* onsets, const std::function<bool()>& cancelled)
{
    PitchAnalysis analysis;
    analysis.analyser = juce::String (pitchAnalysisVersion) + "/" + model.identity();
    analysis.track.sampleRate = sampleRate;

    const auto prepared = preparePitchAudio (audio, sampleRate, model.sampleRate());
    const auto salience = model.salience (prepared, cancelled);

    if (salience.frames() == 0)
        return analysis;

    // The level of each frame, over 25 ms around its middle.
    const auto rate = model.sampleRate();
    const auto half = static_cast<std::int64_t> (levelWindowSeconds * rate / 2.0);
    const auto length = static_cast<std::int64_t> (prepared.size());
    std::vector<float> levels (salience.frames());

    for (size_t f = 0; f < levels.size(); ++f)
    {
        const auto centre = static_cast<std::int64_t> (
            std::llround ((salience.startSeconds + static_cast<double> (f) * salience.hopSeconds) * rate));
        auto sum = 0.0;
        auto n = 0;

        for (auto i = std::max<std::int64_t> (0, centre - half); i < std::min (length, centre + half); ++i)
        {
            sum += static_cast<double> (prepared[static_cast<size_t> (i)]) * prepared[static_cast<size_t> (i)];
            ++n;
        }

        levels[f] = toDb (n > 0 ? std::sqrt (sum / n) : 0.0);
    }

    if (cancelled != nullptr && cancelled())
        return analysis;

    analysis.track = decodePitch (salience, model.voicingThreshold(), levels, sampleRate);
    analysis.notes = findPitchNotes (analysis.track, onsets != nullptr ? *onsets : detectOnsets (audio, sampleRate));
    return analysis;
}

double measureCentre (const PitchTrack& track, juce::int64 start, juce::int64 end)
{
    const auto [first, last] = framesOf (track, start, end);

    if (first > last)
        return std::numeric_limits<double>::quiet_NaN();

    return centreOf (bridgedPitch (track), track, first, last);
}

Curve fitShape (const PitchTrack& track, juce::int64 start, juce::int64 end, double centre)
{
    const auto [first, last] = framesOf (track, start, end);

    if (first > last)
        return {};

    const auto pitch = bridgedPitch (track);
    const auto anyVoiced =
        std::any_of (pitch.begin() + first, pitch.begin() + last + 1, [] (double p) { return ! std::isnan (p); });
    return anyVoiced ? shapeOf (pitch, track, first, last, centre, start) : Curve {};
}

juce::var pitchAnalysisToVar (const PitchAnalysis& analysis)
{
    const auto& track = analysis.track;
    const auto rounded = [] (double value, double scale) { return std::round (value * scale) / scale; };
    juce::Array<juce::var> pitch;

    for (size_t i = 0; i < track.size(); i += 2)
        pitch.add (track.voiced (i) ? juce::var (rounded (track.pitch[i], 100.0)) : juce::var());

    juce::Array<juce::var> notes;

    for (const auto& note : analysis.notes)
    {
        juce::Array<juce::var> shape;

        for (const auto& point : note.shape)
            shape.add (
                juce::Array<juce::var> { point.time, rounded (point.value, 1000.0), static_cast<int> (point.shape) });

        notes.add (makeObject ({ { "leadStart", note.leadStart },
                                 { "start", note.start },
                                 { "end", note.end },
                                 { "centre", rounded (note.centre, 1000.0) },
                                 { "vibratoRate", rounded (note.vibratoRate, 100.0) },
                                 { "vibratoDepth", rounded (note.vibratoDepth, 10.0) },
                                 { "shape", shape } }));
    }

    return makeObject ({ { "analyser", analysis.analyser },
                         { "sampleRate", track.sampleRate },
                         { "start", track.startSeconds },
                         { "hop", track.hopSeconds * 2.0 },
                         { "pitch", pitch },
                         { "notes", notes } });
}

} // namespace dawg
