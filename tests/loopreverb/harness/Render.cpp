#include "Render.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

namespace harness
{

Audio render (dawg::Node& node, const Audio& input, const RenderOptions& options)
{
    for (const auto& [id, value] : options.params)
        if (const auto index = node.descriptor().indexOfParam (id); index >= 0)
            node.params().set (static_cast<size_t> (index), value);

    node.prepare ({ options.sampleRate, options.maxBlockSize });

    const auto total = input.left.size() + static_cast<size_t> (std::max (0, options.tailSamples));
    Audio in;
    in.sampleRate = options.sampleRate;
    in.left = input.left;
    in.right = input.right;
    in.left.resize (total, 0.0f);
    in.right.resize (total, 0.0f);

    Audio out;
    out.sampleRate = options.sampleRate;
    out.left.assign (total, 0.0f);
    out.right.assign (total, 0.0f);

    dawg::Transport transport;
    size_t position = 0;
    size_t block = 0;

    while (position < total)
    {
        const auto size =
            std::min (static_cast<size_t> (options.blockSizes[block++ % options.blockSizes.size()]), total - position);
        const std::array<dawg::AudioInput, 1> inputs { { { in.left.data() + position, in.right.data() + position } } };
        const std::array<dawg::AudioOutput, 1> outputs { { { out.left.data() + position,
                                                             out.right.data() + position } } };
        dawg::ProcessContext context;
        context.numSamples = static_cast<int> (size);
        context.audioInputs = inputs;
        context.audioOutputs = outputs;
        context.transport = &transport;
        node.process (context);
        position += size;
    }

    return out;
}

Audio impulse (double seconds, double sampleRate)
{
    auto audio = silence (seconds, sampleRate);

    if (! audio.left.empty())
        audio.left[0] = audio.right[0] = 1.0f;

    return audio;
}

Audio sweep (double seconds, double sampleRate)
{
    auto audio = silence (seconds, sampleRate);
    const auto frames = audio.left.size();
    const auto ratio = std::log (20000.0 / 20.0);

    for (size_t i = 0; i < frames; ++i)
    {
        const auto t = static_cast<double> (i) / sampleRate;
        const auto phase = 2.0 * std::numbers::pi * 20.0 * seconds / ratio * (std::exp (t / seconds * ratio) - 1.0);
        audio.left[i] = audio.right[i] = static_cast<float> (0.5 * std::sin (phase));
    }

    return audio;
}

Audio noise (double seconds, double sampleRate)
{
    auto audio = silence (seconds, sampleRate);
    std::mt19937 random (20261026);
    std::normal_distribution<float> gaussian (0.0f, 0.25f);

    for (size_t i = 0; i < audio.left.size(); ++i)
    {
        audio.left[i] = gaussian (random);
        audio.right[i] = gaussian (random);
    }

    return audio;
}

Audio silence (double seconds, double sampleRate)
{
    Audio audio;
    audio.sampleRate = sampleRate;
    const auto frames = static_cast<size_t> (std::max (0.0, std::round (seconds * sampleRate)));
    audio.left.assign (frames, 0.0f);
    audio.right.assign (frames, 0.0f);
    return audio;
}

} // namespace harness
