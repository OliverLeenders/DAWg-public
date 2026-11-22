#include "dawg/Stretcher.h"

#include <algorithm>
#include <map>
#include <rubberband/RubberBandStretcher.h>

namespace dawg
{

namespace
{
constexpr int blockFrames = 4096;

using RB = ::RubberBand::RubberBandStretcher;

class RubberBandR3 final : public Stretcher
{
public:
    [[nodiscard]] juce::String identity() const override
    {
        return "rubberband " + juce::String (RUBBERBAND_VERSION) + " options " + juce::String::toHexString (options);
    }

    [[nodiscard]] juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input, const StretchSpec& spec,
                                                   const std::function<bool()>& cancelled) const override
    {
        const auto channels = static_cast<size_t> (input.getNumChannels());
        const auto inputFrames = static_cast<size_t> (input.getNumSamples());
        juce::AudioBuffer<float> output (input.getNumChannels(), static_cast<int> (spec.outputFrames));
        output.clear();

        if (inputFrames == 0 || spec.outputFrames <= 0)
            return output;

        const auto chosen =
            spec.formants ? options : (options & ~RB::OptionFormantPreserved) | RB::OptionFormantShifted;
        RB stretcher (static_cast<size_t> (spec.sampleRate), channels, chosen);
        stretcher.setTimeRatio (static_cast<double> (spec.outputFrames) / static_cast<double> (inputFrames));
        stretcher.setPitchScale (spec.pitchScale);
        stretcher.setExpectedInputDuration (inputFrames);

        std::map<size_t, size_t> keyFrames;

        for (const auto& [in, out] : spec.keyFrames)
            if (in > 0 && static_cast<size_t> (in) < inputFrames && out > 0 && out < spec.outputFrames)
                keyFrames[static_cast<size_t> (in)] = static_cast<size_t> (out);

        if (! keyFrames.empty())
            stretcher.setKeyFrameMap (keyFrames);

        int written = 0;
        std::vector<const float*> in (channels);
        const auto feed = [&] (bool study)
        {
            for (size_t at = 0; at < inputFrames; at += blockFrames)
            {
                const auto count = std::min<size_t> (blockFrames, inputFrames - at);

                for (size_t c = 0; c < channels; ++c)
                    in[c] = input.getReadPointer (static_cast<int> (c), static_cast<int> (at));

                const auto last = at + count >= inputFrames;

                if (study)
                    stretcher.study (in.data(), count, last);
                else
                    stretcher.process (in.data(), count, last);

                if (! study && ! drain (stretcher, output, written))
                    break; // the output is full

                if (cancelled != nullptr && cancelled())
                    return false;
            }

            return true;
        };

        if (! feed (true) || ! feed (false))
            return {};

        while (stretcher.available() > 0 && drain (stretcher, output, written))
        {
        }

        return output;
    }

private:
    static constexpr RB::Options options = RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionFormantPreserved
                                           | RB::OptionPitchHighQuality | RB::OptionChannelsTogether
                                           | RB::OptionThreadingNever;

    // Appends what the stretcher has ready at `written`; returns false once the output is full.
    static bool drain (RB& stretcher, juce::AudioBuffer<float>& output, int& written)
    {
        for (auto available = stretcher.available(); available > 0; available = stretcher.available())
        {
            const auto room = static_cast<size_t> (output.getNumSamples() - written);
            const auto count = std::min (static_cast<size_t> (available), static_cast<size_t> (blockFrames));

            std::vector<float*> out (static_cast<size_t> (output.getNumChannels()));
            juce::AudioBuffer<float> scratch (output.getNumChannels(), static_cast<int> (count));

            for (size_t c = 0; c < out.size(); ++c)
                out[c] = scratch.getWritePointer (static_cast<int> (c));

            const auto got = stretcher.retrieve (out.data(), count);
            const auto keep = std::min (got, room);

            for (int c = 0; c < output.getNumChannels(); ++c)
                output.copyFrom (c, written, scratch, c, 0, static_cast<int> (keep));

            written += static_cast<int> (keep);

            if (written >= output.getNumSamples())
                return false;
        }

        return true;
    }
};
} // namespace

std::shared_ptr<const Stretcher> makeRubberBandStretcher()
{
    return std::make_shared<RubberBandR3>();
}

} // namespace dawg
