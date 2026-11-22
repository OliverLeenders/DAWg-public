#include "dawg/AudioPlayer.h"

#include "dawg/ClipOps.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Sampler.h"
#include "dawg/StretchRenderer.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace dawg
{

namespace
{
// Adaptive with no inputs: the compiler sets the width from the clips and the input (playerWidth(), MP15).
constexpr std::array<PortInfo, 1> playerOut { { { PortType::audio, "Audio", adaptiveChannels } } };
const NodeDescriptor playerDescriptor { nodetype::audioPlayer, "Audio player", {}, playerOut, {}, false,
                                        NodeCategory::io };

constexpr double declickSeconds = 0.002;

// Windowed-sinc interpolation (T7): 16 taps, Blackman window, 512 sub-sample phases.
constexpr int halfTaps = 8;
constexpr size_t tapCount = size_t { 2 } * halfTaps;
constexpr int phases = 512;

class SincTable
{
public:
    SincTable()
    {
        for (int p = 0; p <= phases; ++p)
        {
            const auto frac = static_cast<double> (p) / phases;

            for (int t = 0; t < 2 * halfTaps; ++t)
            {
                const auto x = static_cast<double> (t - halfTaps + 1) - frac;
                const auto sinc = std::abs (x) < 1.0e-9 ? 1.0
                                                        : std::sin (juce::MathConstants<double>::pi * x)
                                                              / (juce::MathConstants<double>::pi * x);
                const auto w = (x + halfTaps) / (2.0 * halfTaps);
                const auto window = 0.42 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * w)
                                    + 0.08 * std::cos (4.0 * juce::MathConstants<double>::pi * w);
                m_table[static_cast<size_t> (p)][static_cast<size_t> (t)] = static_cast<float> (sinc * window);
            }
        }
    }

    // Interpolates samples[i] around a fractional position (samples must cover pos - halfTaps + 1 .. pos + halfTaps).
    [[nodiscard]] float at (const float* samples, double position) const noexcept
    {
        const auto base = static_cast<int> (std::floor (position));
        const auto phase = static_cast<size_t> (std::lround ((position - base) * phases));
        const auto& weights = m_table[phase];
        float sum = 0.0f;

        for (int t = 0; t < 2 * halfTaps; ++t)
            sum += samples[base - halfTaps + 1 + t] * weights[static_cast<size_t> (t)];

        return sum;
    }

private:
    std::array<std::array<float, tapCount>, size_t { phases } + 1> m_table {};
};

[[nodiscard]] const SincTable& sincTable()
{
    static const SincTable table;
    return table;
}

class AudioPlayerNode final : public Node
{
public:
    AudioPlayerNode()
        : Node (playerDescriptor)
    {
        std::ignore = sincTable(); // build the table on the message thread
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        // Room for the file frames one block can need: up to 8x the device rate, plus the filter taps.
        const auto frames = static_cast<size_t> (spec.maxBlockSize) * 8 + tapCount * 2 + 4;
        for (auto& channel : m_scratch)
            channel.assign (frames, 0.0f);
    }

    void setOnlyFile (ObjectId fileId) override { m_onlyFile.store (fileId, std::memory_order_relaxed); }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];
        const auto channels = std::clamp (context.channels, 1, 2);

        // Monitored input passes through the track's chain (T9). A mono player has a mono input, if any.
        for (size_t ch = 0; ch < static_cast<size_t> (channels); ++ch)
        {
            juce::FloatVectorOperations::clear (out[ch], context.numSamples);

            if (context.input[ch] != nullptr)
                juce::FloatVectorOperations::add (out[ch], context.input[ch], context.numSamples);
        }

        const auto& transport = *context.transport;

        if (! transport.playing || context.audio == nullptr || transport.endTick <= transport.startTick)
            return;

        const auto only = m_onlyFile.load (std::memory_order_relaxed);

        for (const auto& clip : context.audio->clips)
            if (static_cast<double> (clip.start) < transport.endTick
                && static_cast<double> (clip.end) > transport.startTick && (only == 0 || clip.fileId == only))
            {
                if (clip.timing != nullptr)
                    renderPieces (clip, context);
                else
                    renderClip (clip, context);
            }
    }

private:
    // Where a block lies in a clip's content ticks: content ticks run at a constant rate in real time, and real
    // time is linear in samples, so a block maps to a straight line local0 + perSample * s whatever the tempo map
    // does inside the clip (docs/design/tempo-map.md, TM8).
    struct Local
    {
        double local0 = 0.0;
        double perSample = 0.0;

        [[nodiscard]] double at (int s) const noexcept { return local0 + perSample * s; }
    };

    // One-shot clips: content ticks run at a constant rate in real time. Musical clips: content ticks are timeline
    // ticks, which are linear in samples within a block (the engine cuts blocks at tempo points).
    [[nodiscard]] Local localOf (const CompiledAudioClip& clip, const ProcessContext& context) const noexcept
    {
        const auto& transport = *context.transport;

        if (clip.musical)
            return { transport.startTick - static_cast<double> (clip.start),
                     (transport.endTick - transport.startTick) / context.numSamples };

        const auto blockSeconds = transport.timeMap.tempoMap().secondsAt (transport.startTick);
        return { (blockSeconds - clip.startSeconds) / clip.secondsPerTick, 1.0 / (m_sampleRate * clip.secondsPerTick) };
    }

    // Renders the passes of a (possibly looped) clip that overlap the block, like forEachAudibleNote(), in the
    // clip's content ticks from its start (0) to its localLength.
    void renderClip (const CompiledAudioClip& clip, const ProcessContext& context) noexcept
    {
        const auto local = localOf (clip, context);
        const auto blockEnd = local.at (context.numSamples);

        const auto loopEnd = static_cast<double> (clip.loopStart + clip.loopLength);
        const auto looped = clip.loopLength > 0;
        auto contentFrom = ! looped || static_cast<double> (clip.offset) < loopEnd
                               ? static_cast<double> (clip.offset)
                               : static_cast<double> (clip.loopStart);
        auto passStart = 0.0;

        while (passStart < clip.localLength && passStart < blockEnd)
        {
            const auto passEnd =
                looped ? std::min (clip.localLength, passStart + (loopEnd - contentFrom)) : clip.localLength;

            if (passEnd > local.local0)
                renderPass (clip, context, local, passStart, passEnd, contentFrom);

            passStart = passEnd;
            contentFrom = static_cast<double> (clip.loopStart);

            if (! looped)
                break;
        }
    }

    void renderPass (const CompiledAudioClip& clip, const ProcessContext& context, const Local& local, double passStart,
                     double passEnd, double contentFrom) noexcept
    {
        const auto n = context.numSamples;
        const auto firstSample =
            std::max (0, static_cast<int> (std::ceil ((passStart - local.local0) / local.perSample)));
        const auto endSample = std::min (n, static_cast<int> (std::ceil ((passEnd - local.local0) / local.perSample)));

        if (firstSample >= endSample || clip.source == nullptr)
            return;

        const auto fileRate = clip.source->sampleRate();
        const auto frameAt = [&] (int s)
        { return (contentFrom + local.at (s) - passStart) * clip.secondsPerTick * fileRate; };

        renderSpan (clip, context, *clip.source, local, firstSample, endSample, frameAt, [] (int) { return 1.0f; });
    }

    // Musical clips (docs/design/time-stretching.md, TS7): each piece plays the file, a render or a repitch preview
    // over its stretch of timeline seconds, with a linear crossfade at the joins between pieces.
    void renderPieces (const CompiledAudioClip& clip, const ProcessContext& context) noexcept
    {
        const auto& transport = *context.transport;
        const auto blockSeconds = transport.timeMap.tempoMap().secondsAt (transport.startTick);
        const auto n = context.numSamples;
        const auto blockEnd = blockSeconds + n / m_sampleRate;
        const auto local = localOf (clip, context);

        for (const auto& piece : clip.pieces)
        {
            const auto from = piece.startSeconds - (piece.joinIn ? joinSeconds : 0.0);
            const auto to = piece.endSeconds + (piece.joinOut ? joinSeconds : 0.0);

            if (from >= blockEnd)
                break;

            if (to <= blockSeconds || piece.source == nullptr)
                continue;

            const auto first = std::max (0, static_cast<int> (std::ceil ((from - blockSeconds) * m_sampleRate)));
            const auto end = std::min (n, static_cast<int> (std::ceil ((to - blockSeconds) * m_sampleRate)));

            if (first >= end)
                continue;

            const auto rate = piece.source->sampleRate();
            size_t hint = 0;
            const auto frameAt = [&] (int s)
            {
                if (piece.kind == CompiledAudioPiece::Kind::repitch)
                    return clip.timing->frameAt (clip.musical ? transport.startTick + local.perSample * s - piece.delta
                                                              : blockSeconds + s / m_sampleRate - piece.delta,
                                                 hint);

                return (blockSeconds + s / m_sampleRate + piece.shift - piece.anchorSeconds) * rate;
            };
            const auto joinGain = [&] (int s)
            {
                if (! piece.joinIn && ! piece.joinOut)
                    return 1.0f;

                const auto seconds = blockSeconds + s / m_sampleRate;
                auto gain = 1.0;

                if (piece.joinIn)
                    gain *= std::clamp ((seconds - from) / (2.0 * joinSeconds), 0.0, 1.0);

                if (piece.joinOut)
                    gain *= std::clamp ((to - seconds) / (2.0 * joinSeconds), 0.0, 1.0);

                return static_cast<float> (gain);
            };

            renderSpan (clip, context, *piece.source, local, first, end, frameAt, joinGain);
        }
    }

    // Plays samples [first, end) of the block from a source: frameAt (s) is the source frame for sample s, read with
    // the sinc interpolator, and the clip's gain and envelope times extraGain (s) shape it.
    template <typename FrameAt, typename ExtraGain>
    void renderSpan (const CompiledAudioClip& clip, const ProcessContext& context, const AudioSource& source,
                     const Local& local, int first, int end, const FrameAt& frameAt,
                     const ExtraGain& extraGain) noexcept
    {
        // Read the source frames this span needs (plus the filter's reach) into scratch.
        const auto firstFrame = static_cast<juce::int64> (std::floor (frameAt (first))) - halfTaps;
        const auto lastFrame = static_cast<juce::int64> (std::ceil (frameAt (end))) + halfTaps + 1;
        const auto count = static_cast<int> (
            std::min<juce::int64> (lastFrame - firstFrame, static_cast<juce::int64> (m_scratch[0].size())));
        const std::array<float*, 2> scratch { m_scratch[0].data(), m_scratch[1].data() };
        std::ignore = source.read (scratch, firstFrame, count); // a dropout plays as silence

        const auto& sinc = sincTable();
        const auto& out = context.audioOutputs[0];

        for (int s = first; s < end; ++s)
        {
            const auto position = frameAt (s) - static_cast<double> (firstFrame);

            if (position < halfTaps - 1 || position >= count - halfTaps - 1)
                continue; // beyond what fitted into scratch (extreme ratios): leave silent

            const auto gain = clip.gain * envelope (clip, local.at (s)) * extraGain (s);
            out[0][s] += sinc.at (scratch[0], position) * gain;

            if (context.channels != 1)
                out[1][s] += sinc.at (scratch[1], position) * gain;
        }
    }

    // Clip fades, and short declick fades at edges without a fade, at a content tick from the clip start.
    [[nodiscard]] static float envelope (const CompiledAudioClip& clip, double fromStart) noexcept
    {
        const auto declickIn = declickSeconds / clip.secondsPerTick;
        const auto declickOut = declickSeconds / clip.endSecondsPerTick;
        const auto toEnd = clip.localLength - fromStart;
        auto gain = 1.0f;

        if (clip.fadeIn > 0 && fromStart < static_cast<double> (clip.fadeIn))
            gain *= fadeGain (clip.fadeInShape, fromStart / static_cast<double> (clip.fadeIn));
        else if (clip.fadeIn == 0 && fromStart < declickIn)
            gain *= static_cast<float> (fromStart / declickIn);

        if (clip.fadeOut > 0 && toEnd < static_cast<double> (clip.fadeOut))
            gain *= fadeGain (clip.fadeOutShape, toEnd / static_cast<double> (clip.fadeOut));
        else if (clip.fadeOut == 0 && toEnd < declickOut)
            gain *= static_cast<float> (toEnd / declickOut);

        return gain;
    }

    double m_sampleRate = 48000.0;
    std::array<std::vector<float>, 2> m_scratch;
    std::atomic<ObjectId> m_onlyFile { 0 }; // set by the compiler while the audio thread may play
};
} // namespace

float fadeGain (FadeShape shape, double x) noexcept
{
    x = std::clamp (x, 0.0, 1.0);

    switch (shape)
    {
        case FadeShape::equalPower:
            return static_cast<float> (std::sin (x * juce::MathConstants<double>::halfPi));
        case FadeShape::exponential:
            return static_cast<float> (x * x);
        case FadeShape::sCurve:
            return static_cast<float> (0.5 - 0.5 * std::cos (x * juce::MathConstants<double>::pi));
        case FadeShape::linear:
            break;
    }

    return static_cast<float> (x);
}

// ---------------------------------------------------------------------------------------------------------
// Musical files (docs/design/time-stretching.md, TS7, TS9)

namespace
{
// Half a second of context on each side of a render, rendered and thrown away so the joins are seamless (TS9).
constexpr double contextSeconds = 0.5;
// Key frames along tempo ramps, so the render follows the curve.
constexpr double rampKeyFrameTicks = static_cast<double> (ticksPerBeat) / 8.0;
// The longest render: longer segments are cut into chunks rendered in parallel (offline R3 runs about 5x real time
// on one core).
constexpr double maxRenderSeconds = 8.0;
// Ratio 1 within floating-point error (TS7).
constexpr double unityTolerance = 1.0e-9;
// Lining up joins (CompiledAudioPiece::shift): the window compared, the largest offset, and the correlation below
// which audio is left as it is.
constexpr double alignWindowSeconds = 0.02;
constexpr double maxAlignSeconds = 0.01;
constexpr double minAlignCorrelation = 0.5;

// The curve from timeline seconds to file frames of one pass. A musical file's content tick c plays at timeline tick
// c + delta; a one-shot file's content second c at timeline second c + delta.
struct PassCurve
{
    const TempoMap& map;
    const TimingMap& timing;
    double delta = 0.0;
    bool musical = true;

    [[nodiscard]] double secondsAt (double content) const
    {
        return musical ? map.secondsAt (content + delta) : content + delta;
    }
};

// The render of one marker segment [a, b) of a file in one pass (TS9): the segment plus its context.
[[nodiscard]] StretchJob jobFor (const AudioFile& file, const PassCurve& curve, double a, double b, double pitchScale,
                                 double& outputStartSeconds)
{
    // The context runs on at the segment's own average rate, so a render depends on its own segment only: moving a
    // marker re-renders just the two segments beside it.
    const auto rate = file.sampleRate;
    const auto secondsA = curve.secondsAt (a);
    const auto secondsB = curve.secondsAt (b);
    const auto frameA = curve.timing.frameAt (a);
    const auto frameB = curve.timing.frameAt (b);
    const auto framesPerSecond = (frameB - frameA) / (secondsB - secondsA);
    const auto out0 = secondsA - contextSeconds;
    // Nudged so float noise doesn't flip a whole frame either way.
    const auto in0 = std::floor (frameA - contextSeconds * framesPerSecond + 1.0e-6);
    const auto in1 = std::ceil (frameB + contextSeconds * framesPerSecond - 1.0e-6);
    outputStartSeconds = out0;

    StretchJob job;
    job.file = file;
    job.inputStart = static_cast<juce::int64> (in0);
    job.inputFrames = std::max (juce::int64 { 1 }, static_cast<juce::int64> (in1 - in0));
    job.spec.sampleRate = rate;
    job.spec.outputFrames =
        std::max (juce::int64 { 1 },
                  static_cast<juce::int64> (std::llround ((secondsB - secondsA + 2.0 * contextSeconds) * rate)));
    job.spec.pitchScale = pitchScale;
    job.seconds = secondsA;

    // Key frames: the segment's ends (its markers), and for musical files the tempo points inside it and samples
    // along ramps (a one-shot file's content runs in real time).
    std::vector<double> ticks { a, b };
    const auto& allPoints = curve.map.points();
    const auto points = curve.musical ? std::span (allPoints) : std::span (allPoints).first (0);

    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto t = static_cast<double> (points[i].tick) - curve.delta;

        if (t > a && t < b)
            ticks.push_back (t);

        if (points[i].ramp && i + 1 < points.size())
        {
            const auto end = std::min (b, static_cast<double> (points[i + 1].tick) - curve.delta);

            const auto first = std::max (a, t);

            for (int k = 1; first + k * rampKeyFrameTicks < end; ++k)
                ticks.push_back (first + k * rampKeyFrameTicks);
        }
    }

    std::ranges::sort (ticks);
    juce::int64 lastIn = 0;
    juce::int64 lastOut = 0;

    for (const auto t : ticks)
    {
        const auto in = static_cast<juce::int64> (std::llround (curve.timing.frameAt (t) - in0));
        const auto out =
            static_cast<juce::int64> (std::llround ((curve.secondsAt (t) - secondsA + contextSeconds) * rate));

        if (in > lastIn && out > lastOut && in < job.inputFrames && out < job.spec.outputFrames)
        {
            job.spec.keyFrames.emplace_back (in, out);
            lastIn = in;
            lastOut = out;
        }
    }

    return job;
}
} // namespace

namespace
{
// Calls fn (curve, a, b, from, to, joinIn, joinOut) for each marker segment [a, b) of the file's content that a pass
// of the clip plays, over the part [from, to) it plays. Joins are the segment ends inside a pass.
template <typename Fn>
void forEachSegment (const NoteClip& clip, const AudioFile& file, const TempoMap& map, const TimingMap& timing, Fn&& fn)
{
    // Musical files work in timeline ticks. One-shot files work in seconds: their content positions are ticks at the
    // tempo where the clip starts, and the clip lasts its real time.
    const auto musical = file.timing.musical();
    const auto clipStart = static_cast<double> (clip.start);
    const auto scale = musical ? 1.0 : 60.0 / (map.bpmAt (clipStart) * static_cast<double> (ticksPerBeat));
    const auto origin = musical ? clipStart : map.secondsAt (clipStart);
    const auto length = musical ? static_cast<double> (clip.length)
                                : map.secondsAt (clipStart + static_cast<double> (clip.length)) - origin;
    const auto offset = static_cast<double> (clip.offset) * scale;
    const auto loopStart = static_cast<double> (clip.loopStart) * scale;
    const auto loopEnd = static_cast<double> (clip.loopStart + clip.loopLength) * scale;
    const auto looped = clip.loopLength > 0;
    auto contentFrom = ! looped || offset < loopEnd ? offset : loopStart;
    const auto fileEnd = timing.tickAt (static_cast<double> (file.frames));
    const auto& points = timing.points();
    auto passStart = 0.0;

    while (passStart < length)
    {
        const auto passEnd = looped ? std::min (length, passStart + (loopEnd - contentFrom)) : length;

        if (passEnd <= passStart)
            break;

        const PassCurve curve { map, timing, origin + passStart - contentFrom, musical };
        const auto c0 = contentFrom;
        const auto c1 = std::min (contentFrom + passEnd - passStart, fileEnd);

        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto start = points[i].tick;
            const auto end = i + 1 < points.size() ? points[i + 1].tick : fileEnd;

            if (end <= start || end <= c0 || start >= c1)
                continue;

            // Long segments render in chunks, so several workers share them.
            const auto seconds = curve.secondsAt (end) - curve.secondsAt (start);
            const auto chunks = std::max (1, static_cast<int> (std::ceil (seconds / maxRenderSeconds)));

            for (int k = 0; k < chunks; ++k)
            {
                const auto a = start + (end - start) * k / chunks;
                const auto b = k + 1 == chunks ? end : start + (end - start) * (k + 1) / chunks;
                const auto from = std::max (a, c0);
                const auto to = std::min (b, c1);

                if (to > from)
                    fn (curve, a, b, from, to, juce::exactlyEqual (from, a) && a > c0,
                        juce::exactlyEqual (to, b) && b < c1);
            }
        }

        passStart = passEnd;
        contentFrom = loopStart;

        if (! looped)
            break;
    }
}

// Whether a segment plays at the file's own speed and pitch, within floating-point error (TS7).
[[nodiscard]] bool isUnity (const PassCurve& curve, double a, double b, double rate, double pitchScale)
{
    const auto span = curve.secondsAt (b) - curve.secondsAt (a);
    const auto frames = curve.timing.frameAt (b) - curve.timing.frameAt (a);
    return juce::exactlyEqual (pitchScale, 1.0) && std::abs (frames / rate - span) <= unityTolerance * span;
}

[[nodiscard]] double pitchScaleOf (const NoteClip& clip, const AudioFile& file)
{
    return file.timing.style == MusicalStyle::stretch ? std::pow (2.0, clip.transpose / 12.0) : 1.0;
}

// The offset for piece b (see CompiledAudioPiece::shift) that lines it up with piece a, itself offset by a.shift,
// over alignWindowSeconds around their join: the offset within ±maxAlignSeconds at which the two correlate best. Of
// offsets that correlate nearly as well (periodic audio), the one nearest 0 wins, so offsets never add up along a
// clip; weakly correlated audio isn't offset.
[[nodiscard]] double alignmentShift (const CompiledAudioPiece& a, const CompiledAudioPiece& b, double join)
{
    const auto rate = a.source->sampleRate();
    const auto window = static_cast<int> (std::lround (alignWindowSeconds * rate));
    const auto maxLag = static_cast<int> (std::lround (maxAlignSeconds * rate));
    const auto start = join - alignWindowSeconds / 2.0;

    const auto readMono = [] (const AudioSource& source, juce::int64 from, int count)
    {
        juce::AudioBuffer<float> buffer (2, count);
        std::ignore = source.readBlocking ({ buffer.getWritePointer (0), buffer.getWritePointer (1) }, from, count);
        std::vector<double> mono (static_cast<size_t> (count));

        for (int i = 0; i < count; ++i)
            mono[static_cast<size_t> (i)] = 0.5 * (buffer.getSample (0, i) + buffer.getSample (1, i));

        return mono;
    };

    const auto aSamples = readMono (*a.source, std::llround ((start + a.shift - a.anchorSeconds) * rate), window);
    const auto bSamples =
        readMono (*b.source, std::llround ((start - b.anchorSeconds) * rate) - maxLag, window + 2 * maxLag);
    auto aEnergy = 0.0;

    for (const auto x : aSamples)
        aEnergy += x * x;

    if (aEnergy < 1.0e-9)
        return 0.0;

    std::vector<double> correlation (static_cast<size_t> (2 * maxLag + 1));
    auto best = -1.0;

    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        auto product = 0.0;
        auto bEnergy = 0.0;

        for (int n = 0; n < window; ++n)
        {
            const auto index = n + maxLag + lag;
            const auto x = bSamples[static_cast<size_t> (index)];
            product += x * aSamples[static_cast<size_t> (n)];
            bEnergy += x * x;
        }

        const auto c = bEnergy > 1.0e-9 ? product / std::sqrt (aEnergy * bEnergy) : 0.0;
        const auto slot = lag + maxLag;
        correlation[static_cast<size_t> (slot)] = c;
        best = std::max (best, c);
    }

    if (best < minAlignCorrelation)
        return 0.0;

    auto chosen = maxLag + 1;

    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        const auto slot = lag + maxLag;

        if (correlation[static_cast<size_t> (slot)] >= 0.98 * best && std::abs (lag) < std::abs (chosen))
            chosen = lag;
    }

    return chosen / rate;
}

// Lines up the joins that involve a render (see CompiledAudioPiece::shift), piece by piece along the clip. Each
// offset is cached by what the two pieces play and the first one's offset, so recompiling finds it again.
void alignJoins (std::vector<CompiledAudioPiece>& pieces, const std::vector<juce::String>& keys,
                 StretchRenderer& renderer)
{
    using Kind = CompiledAudioPiece::Kind;

    for (size_t i = 0; i + 1 < pieces.size(); ++i)
    {
        const auto& a = pieces[i];
        auto& b = pieces[i + 1];

        if (! a.joinOut || ! b.joinIn || a.kind == Kind::repitch || b.kind == Kind::repitch
            || (a.kind != Kind::render && b.kind != Kind::render) || a.source == nullptr || b.source == nullptr
            || ! juce::exactlyEqual (a.source->sampleRate(), b.source->sampleRate()))
            continue;

        const auto join = a.endSeconds;
        const auto key = keys[i] + "|" + keys[i + 1] + "|" + juce::String (join - a.anchorSeconds, 7) + "|"
                         + juce::String (join - b.anchorSeconds, 7) + "|" + juce::String (a.shift, 7);
        b.shift = renderer.cached (key, [&] { return alignmentShift (a, b, join); });
    }
}

// Appends a piece, merging it into the previous one where they play the same source the same way. Returns whether
// it was merged.
bool addPiece (std::vector<CompiledAudioPiece>& pieces, CompiledAudioPiece piece)
{
    if (! pieces.empty())
    {
        auto& last = pieces.back();
        const auto same = last.kind == piece.kind && last.source == piece.source && last.joinOut && piece.joinIn
                          && (piece.kind == CompiledAudioPiece::Kind::repitch
                                  ? std::abs (last.delta - piece.delta) <= 1.0e-6
                                  : piece.kind == CompiledAudioPiece::Kind::file
                                        && std::abs (last.anchorSeconds - piece.anchorSeconds) <= 1.0e-9);

        if (same)
        {
            last.endSeconds = piece.endSeconds;
            last.joinOut = piece.joinOut;
            return true;
        }
    }

    pieces.push_back (std::move (piece));
    return false;
}

// Plans a musical clip's pieces (TS7, TS9): the file where a segment's ratio is 1, renders where it is stretched (or
// the repitch preview while they are made), and repitch everywhere in the repitch style.
void planPieces (CompiledAudioClip& compiled, const NoteClip& clip, const AudioFile& file, const TempoMap& map,
                 StretchRenderer* renderer)
{
    const auto& timing = *compiled.timing;
    const auto stretch = file.timing.style == MusicalStyle::stretch;
    const auto pitch = pitchScaleOf (clip, file);
    const auto rate = file.sampleRate;
    std::vector<juce::String> keys; // what each piece plays, for alignJoins()

    forEachSegment (clip, file, map, timing,
                    [&] (const PassCurve& curve, double a, double b, double from, double to, bool joinIn, bool joinOut)
                    {
                        CompiledAudioPiece piece;
                        juce::String key;
                        piece.startSeconds = curve.secondsAt (from);
                        piece.endSeconds = curve.secondsAt (to);
                        piece.joinIn = joinIn;
                        piece.joinOut = joinOut;
                        piece.source = compiled.source;

                        if (isUnity (curve, a, b, rate, pitch))
                        {
                            piece.kind = CompiledAudioPiece::Kind::file;
                            piece.anchorSeconds = curve.secondsAt (a) - timing.frameAt (a) / rate;
                            key = "file|" + file.path;
                        }
                        else if (stretch && renderer != nullptr)
                        {
                            auto outputStart = 0.0;
                            const auto job = jobFor (file, curve, a, b, pitch, outputStart);

                            if (auto render = renderer->find (job, clip.id, &key))
                            {
                                piece.kind = CompiledAudioPiece::Kind::render;
                                piece.source = std::move (render);
                                piece.anchorSeconds = outputStart;
                            }
                            else
                            {
                                piece.kind = CompiledAudioPiece::Kind::repitch;
                                piece.delta = curve.delta;
                                ++compiled.pendingRenders;
                            }
                        }
                        else
                        {
                            piece.kind = CompiledAudioPiece::Kind::repitch;
                            piece.delta = curve.delta;
                        }

                        if (! addPiece (compiled.pieces, std::move (piece)))
                            keys.push_back (key);
                    });

    if (renderer != nullptr)
        alignJoins (compiled.pieces, keys, *renderer);
}
} // namespace

namespace
{
// The renders the Samplers in a graph play in Keep length (docs/design/sampler-mapping.md, PM2), found by compiling their
// zones with a renderer that only notes what it's asked for.
void samplerJobs (const TrackGraph& graph, const Project& project, std::vector<StretchJob>& jobs)
{
    const SampleRenderer note = [&jobs] (const AudioFile& file, juce::int64 start, juce::int64 frames, double scale)
    {
        jobs.push_back (samplerRenderJob (file, start, frames, scale));
        return std::shared_ptr<const AudioSource>();
    };

    for (const auto& node : graph.nodes)
    {
        if (nodetype::playsSamples (node.type))
            std::ignore = compileSampleMap (node, project, AudioSourceResolver(), note);

        if (node.component != nullptr)
            samplerJobs (node.component->graph, project, jobs);

        if (node.voiceGraph != nullptr)
            samplerJobs (*node.voiceGraph, project, jobs);
    }
}
} // namespace

std::vector<StretchJob> stretchJobs (const Project& project)
{
    std::vector<StretchJob> jobs;

    for (const auto& track : project.tracks)
        samplerJobs (track->graph, project, jobs);

    if (project.master != nullptr)
        samplerJobs (project.master->graph, project, jobs);

    for (const auto& track : project.tracks)
    {
        if (track->kind != TrackKind::audio)
            continue;

        for (const auto& clip : crossfadedParts (track->clips, project))
        {
            const auto* file = clip->fileId != 0 ? project.findAudioFile (clip->fileId) : nullptr;

            if (file == nullptr || ! file->timing.retimed() || file->timing.style != MusicalStyle::stretch)
                continue;

            const TimingMap timing (file->timing, file->sampleRate);
            const auto pitch = pitchScaleOf (*clip, *file);

            forEachSegment (*clip, *file, project.tempoMap, timing,
                            [&] (const PassCurve& curve, double a, double b, double, double, bool, bool)
                            {
                                if (isUnity (curve, a, b, file->sampleRate, pitch))
                                    return;

                                auto outputStart = 0.0;
                                jobs.push_back (jobFor (*file, curve, a, b, pitch, outputStart));
                            });
        }
    }

    return jobs;
}

AudioSequence compileAudio (const Track& track, const Project& project, const AudioSourceResolver& resolve,
                            StretchRenderer* stretch)
{
    AudioSequence sequence;

    // Stacked takes play top first (docs/design/takes-lanes.md, T8), and covered clips play on under the fades of the
    // clips above them (docs/design/audio-features.md, AU6). A frozen track's renders overlap but each plays through
    // a player of its own (frozenView, AU15), so they are taken as they are.
    const auto clips = track.freeze.active() ? track.clips : crossfadedParts (track.clips, project);

    for (const auto& clip : clips)
    {
        const auto* file = clip->fileId != 0 ? project.findAudioFile (clip->fileId) : nullptr;

        if (file == nullptr)
            continue;

        CompiledAudioClip compiled;
        compiled.id = clip->id;
        compiled.fileId = file->id;
        compiled.start = clip->start;
        compiled.end = clip->start + clip->length;
        compiled.offset = clip->offset;
        compiled.loopStart = clip->loopStart;
        compiled.loopLength = clip->loopLength;
        compiled.gain = juce::Decibels::decibelsToGain (static_cast<float> (clip->gainDb), -96.0f);
        compiled.fadeIn = clip->fadeIn;
        compiled.fadeOut = clip->fadeOut;
        compiled.fadeInShape = clip->fadeInShape;
        compiled.fadeOutShape = clip->fadeOutShape;
        compiled.source = resolve != nullptr ? resolve (*file) : nullptr;

        // A pitch-corrected file plays its phrase renders over the file (docs/design/pitch-correction.md, 5.2);
        // phrases still rendering play as recorded meanwhile.
        auto pitchPending = 0;

        if (stretch != nullptr && compiled.source != nullptr && file->pitch.edited())
            if (auto overlays = stretch->pitchOverlays (*file, clip->id, pitchPending); ! overlays.empty())
                compiled.source = AudioSource::withOverlays (compiled.source, std::move (overlays));
        // Content ticks run at the tempo where the clip starts (docs/design/tempo-map.md, TM8).
        const auto& map = project.tempoMap;
        compiled.secondsPerTick = 60.0 / (map.bpmAt (static_cast<double> (clip->start)) * ticksPerBeat);
        compiled.endSecondsPerTick = compiled.secondsPerTick;
        compiled.startSeconds = map.secondsAt (static_cast<double> (clip->start));
        compiled.localLength =
            (map.secondsAt (static_cast<double> (compiled.end)) - compiled.startSeconds) / compiled.secondsPerTick;

        // Musical files, and one-shot files with markers, play through their timing map
        // (docs/design/time-stretching.md, TS7).
        if (file->timing.musical())
        {
            compiled.musical = true;
            compiled.localLength = static_cast<double> (clip->length);
            // Musical content ticks are timeline ticks, so the declick at the end lasts its ticks at the end's tempo.
            compiled.endSecondsPerTick = 60.0 / (map.bpmAt (static_cast<double> (compiled.end)) * ticksPerBeat);
        }

        if (file->timing.retimed())
        {
            compiled.timing = std::make_shared<const TimingMap> (file->timing, file->sampleRate);
            planPieces (compiled, *clip, *file, map, stretch);
        }

        compiled.pendingRenders += pitchPending;
        sequence.clips.push_back (std::move (compiled));
    }

    std::ranges::sort (sequence.clips, {}, &CompiledAudioClip::start);
    return sequence;
}

int playerWidth (const Track& track, const Project& project, ObjectId onlyFile) noexcept
{
    auto any = false;

    for (const auto& clip : track.clips)
    {
        if (clip->fileId == 0 || (onlyFile != 0 && clip->fileId != onlyFile))
            continue;

        if (const auto* file = project.findAudioFile (clip->fileId); file != nullptr)
        {
            if (file->channels != 1)
                return 2;

            any = true;
        }
    }

    if (track.kind == TrackKind::audio && track.inputChannel >= 0)
    {
        if (track.inputStereo)
            return 2;

        any = true;
    }

    return any ? 1 : 2;
}

const NodeDescriptor& audioPlayerDescriptor() noexcept
{
    return playerDescriptor;
}

std::unique_ptr<Node> makeAudioPlayerNode()
{
    return std::make_unique<AudioPlayerNode>();
}

} // namespace dawg
