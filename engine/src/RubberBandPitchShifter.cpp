#include "dawg/PitchShifter.h"

#include <algorithm>
#include <rubberband/RubberBandStretcher.h>

namespace dawg
{

namespace
{
using RB = ::RubberBand::RubberBandStretcher;

constexpr size_t blockFrames = 256; // the ratio is set before each block (section 5.1, step 3)

class RubberBandShifter final : public PitchShifter
{
public:
    explicit RubberBandShifter (RB::Options chosen)
        : options (chosen)
    {
    }

    [[nodiscard]] juce::String identity() const override
    {
        return "rubberband " + juce::String (RUBBERBAND_VERSION) + " shift options "
               + juce::String::toHexString (static_cast<int> (options));
    }

    [[nodiscard]] juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input, double sampleRate,
                                                   const std::vector<float>& ratio, int hop,
                                                   const std::function<bool()>& cancelled) const override
    {
        const auto channels = static_cast<size_t> (input.getNumChannels());
        const auto frames = static_cast<size_t> (input.getNumSamples());
        juce::AudioBuffer<float> output (input.getNumChannels(), input.getNumSamples());
        output.clear();

        if (frames == 0 || channels == 0)
            return output;

        // The ratio at an input frame, interpolated between its samples and held past the ends.
        const auto ratioAt = [&ratio, hop] (double frame)
        {
            if (ratio.empty() || hop <= 0)
                return 1.0;

            const auto at = std::max (0.0, frame / hop);
            const auto i = static_cast<size_t> (at);

            if (i + 1 >= ratio.size())
                return static_cast<double> (ratio.back());

            const auto f = at - static_cast<double> (i);
            return ratio[i] + (ratio[i + 1] - ratio[i]) * f;
        };

        RB shifter (static_cast<size_t> (sampleRate), channels, options);
        shifter.setMaxProcessSize (blockFrames);
        shifter.setPitchScale (ratioAt (0.0));

        // Step 2 and 4: silence in front, and the start delay dropped from the output, so output frame k matches
        // input frame k.
        const auto pad = shifter.getPreferredStartPad();
        auto skip = shifter.getStartDelay();
        size_t written = 0;

        juce::AudioBuffer<float> silence (static_cast<int> (channels), static_cast<int> (blockFrames));
        silence.clear();
        juce::AudioBuffer<float> scratch (static_cast<int> (channels), static_cast<int> (blockFrames));
        std::vector<const float*> in (channels);
        std::vector<float*> out (channels);

        const auto drain = [&]
        {
            for (auto available = shifter.available(); available > 0 && written < frames;
                 available = shifter.available())
            {
                for (size_t c = 0; c < channels; ++c)
                    out[c] = scratch.getWritePointer (static_cast<int> (c));

                auto got = shifter.retrieve (out.data(), std::min (static_cast<size_t> (available), blockFrames));
                size_t from = 0;

                if (skip > 0)
                {
                    from = std::min (skip, got);
                    skip -= from;
                }

                const auto keep = std::min (got - from, frames - written);

                for (size_t c = 0; c < channels; ++c)
                    output.copyFrom (static_cast<int> (c), static_cast<int> (written), scratch, static_cast<int> (c),
                                     static_cast<int> (from), static_cast<int> (keep));

                written += keep;
            }
        };

        const auto feed = [&] (const juce::AudioBuffer<float>& source, size_t at, size_t count, bool last)
        {
            for (size_t c = 0; c < channels; ++c)
                in[c] = source.getReadPointer (static_cast<int> (c), static_cast<int> (at));

            shifter.process (in.data(), count, last);
            drain();
        };

        for (size_t left = pad; left > 0;)
        {
            const auto count = std::min (left, blockFrames);
            feed (silence, 0, count, false);
            left -= count;
        }

        // Step 3: the input in blocks. A ratio set now reaches the audio the shifter is synthesising now, which lags
        // what goes in; so each block sets the ratio of the audio that far back. Measured (2026-11-16, Rubber Band
        // 4.0.0, 44.1 and 96 kHz): the lag is the start pad plus the start delay. Setting the block's own ratio put
        // every change about 90 ms early.
        const auto lag = static_cast<double> (pad + shifter.getStartDelay());

        for (size_t at = 0; at < frames; at += blockFrames)
        {
            const auto count = std::min (blockFrames, frames - at);
            shifter.setPitchScale (ratioAt (static_cast<double> (at) + static_cast<double> (count) / 2.0 - lag));
            feed (input, at, count, false);

            if (cancelled != nullptr && cancelled())
                return {};
        }

        // The tail: silence until the output is complete (bounded, in case the shifter stalls).
        for (int guard = 0; written < frames && guard < 4096; ++guard)
            feed (silence, 0, blockFrames, false);

        return output;
    }

private:
    RB::Options options;
};
} // namespace

std::shared_ptr<const PitchShifter> makeRubberBandPitchShifter (bool finer, bool formants)
{
    return std::make_shared<RubberBandShifter> (
        RB::OptionProcessRealTime | (finer ? RB::OptionEngineFiner : RB::OptionEngineFaster)
        | (formants ? RB::OptionFormantPreserved : RB::OptionFormantShifted) | RB::OptionPitchHighConsistency
        | RB::OptionChannelsTogether | RB::OptionThreadingNever);
}

} // namespace dawg
