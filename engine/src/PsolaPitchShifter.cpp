#include "dawg/PitchShifter.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
constexpr double unvoicedPeriodSeconds = 0.005; // marks every 5 ms where there's no pitch, copied unchanged
constexpr double lowPassHz = 1000.0;            // the signal marks are placed on: the voice's lower partials
constexpr double searchShare = 0.25;            // a mark is looked for within a quarter period of its prediction

// TD-PSOLA (docs/design/pitch-correction.md, section 5.1): pitch marks one period apart on the sung pitch, then each
// two-period grain, Hann-windowed around its mark, is laid down again at the shifted period. Grains are never
// resampled, so the formants stay where they were.
class PsolaShifter final : public PitchShifter
{
public:
    explicit PsolaShifter (PitchTrack track)
        : m_track (std::move (track))
    {
    }

    [[nodiscard]] juce::String identity() const override { return "psola 1"; }

    [[nodiscard]] juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input, double sampleRate,
                                                   const std::vector<float>& ratio, int hop,
                                                   const std::function<bool()>& cancelled) const override
    {
        const auto frames = input.getNumSamples();
        const auto channels = input.getNumChannels();
        juce::AudioBuffer<float> output (channels, frames);
        output.clear();

        if (frames == 0 || channels == 0)
            return output;

        const auto marks = pitchMarks (input, sampleRate);
        const auto ratioAt = [&ratio, hop] (double frame)
        {
            if (ratio.empty() || hop <= 0)
                return 1.0;

            const auto at = std::max (0.0, frame / hop);
            const auto i = static_cast<size_t> (at);

            if (i + 1 >= ratio.size())
                return static_cast<double> (ratio.back());

            return ratio[i] + (ratio[i + 1] - ratio[i]) * (at - static_cast<double> (i));
        };

        std::vector<double> weight (static_cast<size_t> (frames), 0.0);
        size_t nearest = 0;

        for (auto at = static_cast<double> (marks.front().frame); at < frames;)
        {
            // The analysis mark nearest this synthesis mark.
            while (nearest + 1 < marks.size()
                   && std::abs (marks[nearest + 1].frame - at) <= std::abs (marks[nearest].frame - at))
                ++nearest;

            const auto& mark = marks[nearest];
            // The step to the next mark as placed, not the nominal period: at a ratio of 1 every grain then goes back
            // exactly where it came from, and the audio comes out unchanged. A grain reaches at least to its
            // neighbours, so the windows always cover every frame between them.
            const auto period = nearest + 1 < marks.size()
                                    ? std::max (1.0, static_cast<double> (marks[nearest + 1].frame - mark.frame))
                                    : mark.period;
            const auto r = mark.voiced ? ratioAt (at) : 1.0;

            // Where nothing is shifted, synthesis goes back onto the analysis marks: a shift moves the synthesis
            // marks by fractions of a period, which would otherwise stay as a phase offset after it.
            if (std::abs (r - 1.0) < 1.0e-6)
                at = static_cast<double> (mark.frame);

            const auto centre = static_cast<int> (std::lround (at));
            const auto before = nearest > 0 ? static_cast<double> (mark.frame - marks[nearest - 1].frame) : 0.0;
            const auto half = static_cast<int> (std::lround (std::max ({ mark.period, period, before })));

            for (auto k = -half; k <= half; ++k)
            {
                const auto from = mark.frame + k;
                const auto to = centre + k;

                if (from < 0 || from >= frames || to < 0 || to >= frames)
                    continue;

                const auto w = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * k / half);

                for (int c = 0; c < channels; ++c)
                    output.addSample (c, to, static_cast<float> (w * input.getSample (c, from)));

                weight[static_cast<size_t> (to)] += w;
            }

            at += period / r;

            if (cancelled != nullptr && (nearest % 512) == 0 && cancelled())
                return {};
        }

        // Grains overlap more when the pitch goes up; dividing by the windows' sum keeps the level.
        for (int i = 0; i < frames; ++i)
            if (weight[static_cast<size_t> (i)] > 0.05)
                for (int c = 0; c < channels; ++c)
                    output.setSample (c, i,
                                      static_cast<float> (output.getSample (c, i) / weight[static_cast<size_t> (i)]));

        return output;
    }

private:
    struct Mark
    {
        int frame = 0;
        double period = 0.0;
        bool voiced = false;
    };

    // The pitch in Hz at an audio frame, or 0 where the track is unvoiced there.
    [[nodiscard]] double hzAt (double frame) const
    {
        const auto at = m_track.indexOf (static_cast<juce::int64> (frame));
        const auto i = static_cast<std::ptrdiff_t> (std::floor (at));

        if (i < 0 || static_cast<size_t> (i + 1) >= m_track.size() || ! m_track.voiced (static_cast<size_t> (i))
            || ! m_track.voiced (static_cast<size_t> (i + 1)))
            return 0.0;

        const auto f = at - static_cast<double> (i);
        const auto pitch =
            m_track.pitch[static_cast<size_t> (i)] * (1.0 - f) + m_track.pitch[static_cast<size_t> (i + 1)] * f;
        return 440.0 * std::pow (2.0, (pitch - 69.0) / 12.0);
    }

    // Marks one period apart on the low-passed mono signal's peaks where voiced, every 5 ms elsewhere.
    [[nodiscard]] std::vector<Mark> pitchMarks (const juce::AudioBuffer<float>& input, double sampleRate) const
    {
        const auto frames = input.getNumSamples();
        std::vector<float> mono (static_cast<size_t> (frames), 0.0f);

        for (int c = 0; c < input.getNumChannels(); ++c)
            for (int i = 0; i < frames; ++i)
                mono[static_cast<size_t> (i)] += input.getSample (c, i);

        // Two passes of a one-pole low-pass, forwards then backwards, so peaks don't move.
        const auto a = std::exp (-juce::MathConstants<double>::twoPi * lowPassHz / sampleRate);
        auto state = 0.0;

        for (auto& v : mono)
            v = static_cast<float> (state = (1.0 - a) * v + a * state);

        state = 0.0;

        for (auto it = mono.rbegin(); it != mono.rend(); ++it)
            *it = static_cast<float> (state = (1.0 - a) * *it + a * state);

        std::vector<Mark> marks;
        const auto unvoiced = unvoicedPeriodSeconds * sampleRate;
        double at = 0.0;
        auto wasVoiced = false;

        while (at < frames)
        {
            const auto hz = hzAt (at);

            if (hz <= 0.0)
            {
                marks.push_back ({ static_cast<int> (at), unvoiced, false });
                at += unvoiced;
                wasVoiced = false;
                continue;
            }

            const auto period = sampleRate / hz;
            // The first mark of a voiced stretch takes the highest peak of its first period; later ones the highest
            // near where one period on predicts.
            const auto from = static_cast<int> (wasVoiced ? at - searchShare * period : at);
            const auto to = static_cast<int> (wasVoiced ? at + searchShare * period : at + period);
            auto best = std::clamp (static_cast<int> (at), 0, frames - 1);

            for (auto i = std::max (0, from); i <= std::min (frames - 1, to); ++i)
                if (mono[static_cast<size_t> (i)] > mono[static_cast<size_t> (best)])
                    best = i;

            if (! marks.empty() && best <= marks.back().frame)
                best = marks.back().frame + 1;

            marks.push_back ({ best, period, true });
            at = best + period;
            wasVoiced = true;
        }

        // A mark on the first frame, so the frames before the first period's peak are covered too.
        if (marks.empty() || marks.front().frame > 0)
            marks.insert (marks.begin(), { 0, unvoiced, false });

        return marks;
    }

    PitchTrack m_track;
};
} // namespace

std::shared_ptr<const PitchShifter> makePsolaPitchShifter (PitchTrack track)
{
    return std::make_shared<PsolaShifter> (std::move (track));
}

} // namespace dawg
