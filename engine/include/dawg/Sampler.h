#pragma once

#include "dawg/AudioSource.h"
#include "dawg/Model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace dawg
{

/**
    The highest choke group a zone can be in; 0 is none.
*/
inline constexpr int maxChokeGroup = 8;

/**
    The most layers a Sampler's zones can be in (docs/design/sampler-mapping.md, TL1).
*/
inline constexpr int maxSampleLayers = 8;

/**
    The most outputs a Sampler has: Main and Out 2 to Out 16 (docs/design/multi-output-instruments.md, MO1).
*/
inline constexpr int maxSampleOutputs = 16;

/**
    A zone's sample rendered at another pitch, at its own length (docs/design/sampler-mapping.md, PM2).
*/
struct PitchedRender
{
    /**
        Semitones from the zone's root key.
    */
    int semitones = 0;

    /**
        The render: the zone's frames from its start, pitched, as long as they were.
    */
    std::shared_ptr<const AudioSource> source;
};

/**
    Returns a render of a pool file's frames at a pitch scale (2 is an octave up), the same length; or null while it
    is being rendered.
*/
using SampleRenderer = std::function<std::shared_ptr<const AudioSource> (const AudioFile& file, juce::int64 start,
                                                                         juce::int64 frames, double pitchScale)>;

/**
    How far apart a zone's renders are, by how many keys it covers (PM2): every key up to 25, every 3 semitones beyond.
*/
[[nodiscard]] inline int renderStep (int keys) noexcept
{
    return keys <= 25 ? 1 : 3;
}

/**
    A sample zone made ready for playback: its file opened and its positions checked against it.
*/
struct CompiledSampleZone
{
    /**
        The zone's id, by which a sounding voice finds it again after a recompile.
    */
    ObjectId id = 0;

    /**
        The file's audio, or null if it is missing (the zone then plays silence).
    */
    std::shared_ptr<const AudioSource> source;

    /**
        The zone's key and velocity range and root key.
    */
    SampleZone zone;

    /**
        First frame played, and the frame after the last.
    */
    juce::int64 start = 0;

    /**
        The frame after the last played.
    */
    juce::int64 end = 0;

    /**
        The looped frames, when the zone loops; loopEnd is 0 when it doesn't.
    */
    juce::int64 loopStart = 0;

    /**
        The frame the loop jumps back from.
    */
    juce::int64 loopEnd = 0;

    /**
        Frames before loopEnd that crossfade into those before loopStart.
    */
    juce::int64 crossfade = 0;

    /**
        Linear gains of the zone's gain and pan.
    */
    std::array<float, 2> gains { 1.0f, 1.0f };

    /**
        Its renders at other pitches, by semitones, when the Sampler keeps notes' lengths (PM2); complete only when
        every one it needs is ready, else empty, and notes keep their length with grains meanwhile (PM3).
    */
    std::vector<PitchedRender> pitched;
};

/**
    What a Sampler's zones compile to, shared by that node's instances in every voice slot (docs/design/
    sampler-midi-delay-limiter.md, section 4). Immutable but for the choke counts, which belong to the audio thread: a
    voice starting in a choke group counts up, and the group's other voices see the count move.
*/
struct SampleMap
{
    /**
        The zones, in model order. In each layer, the first that holds a note plays it, or one of its takes.
    */
    std::vector<CompiledSampleZone> zones;

    /**
        For each zone that is the first of its takes, which take played last, plus 1 (0 before any), so the next can
        be another (docs/design/sampler-mapping.md, TL2). The audio thread's, as the choke counts are.
    */
    mutable std::vector<juce::uint32> takes;

    /**
        The state of the random numbers that pick takes at random.
    */
    mutable juce::uint32 random = 0x9e3779b9u;

    /**
        How many notes have started in each choke group. Slots of a Voice Instrument run one after the other, so this
        needs no synchronisation.
    */
    mutable std::array<juce::uint32, static_cast<size_t> (maxChokeGroup) + 1> chokes {};
};

/**
    Compiles a Sampler's zones.

    @param node     The Sampler node.
    @param project  The project, for its audio pool.
    @param resolve  Opens a pool file.
    @param render   Renders a zone at another pitch, when the node keeps notes' lengths (PM2); none leaves them to the
                    grains.
    @return         The compiled zones.
*/
template <typename Resolve>
[[nodiscard]] std::shared_ptr<const SampleMap> compileSampleMap (const GraphNode& node, const Project& project,
                                                                 const Resolve& resolve,
                                                                 const SampleRenderer& render = {})
{
    const auto pitchMode = node.params.find ("pitchMode");
    const auto keepLength = render && pitchMode != node.params.end() && pitchMode->second >= 0.5;

    auto map = std::make_shared<SampleMap>();

    for (const auto& zone : node.sampleZones)
    {
        const auto* file = project.findAudioFile (zone.fileId);

        if (file == nullptr)
            continue;

        CompiledSampleZone compiled;
        compiled.id = zone.id;
        compiled.zone = zone;

        if (resolve)
            compiled.source = resolve (*file);

        compiled.end = zone.end > 0 ? std::min (zone.end, file->frames) : file->frames;
        compiled.start = std::clamp<juce::int64> (zone.start, 0, std::max<juce::int64> (0, compiled.end - 1));

        if (zone.mode == SampleMode::loop)
        {
            const auto loopEnd = zone.loopEnd > 0 ? std::min (zone.loopEnd, compiled.end) : compiled.end;
            const auto loopStart = std::clamp (zone.loopStart, compiled.start, loopEnd);

            if (loopEnd - loopStart >= 2)
            {
                compiled.loopStart = loopStart;
                compiled.loopEnd = loopEnd;
                compiled.crossfade = std::clamp<juce::int64> (
                    zone.crossfade, 0, std::min (loopEnd - loopStart, loopStart - compiled.start));
            }
        }

        const auto gain = std::pow (10.0, zone.gainDb / 20.0);
        const auto angle = (std::clamp (zone.pan, -1.0, 1.0) + 1.0) * juce::MathConstants<double>::pi / 4.0;
        compiled.gains = { static_cast<float> (gain * std::cos (angle) * juce::MathConstants<double>::sqrt2),
                           static_cast<float> (gain * std::sin (angle) * juce::MathConstants<double>::sqrt2) };
        compiled.zone.layer = std::clamp (zone.layer, 1, maxSampleLayers);

        // The pitches its keys need, all ready or none (PM2, PM3).
        if (keepLength && compiled.end > compiled.start)
        {
            const auto step = renderStep (zone.highKey - zone.lowKey + 1);
            const auto lowest =
                static_cast<int> (std::floor ((zone.lowKey - zone.rootKey) / static_cast<double> (step))) * step;
            auto complete = true;

            for (auto semitones = lowest; semitones <= zone.highKey - zone.rootKey + step - 1; semitones += step)
            {
                if (semitones == 0 || std::abs (semitones) > 48)
                    continue;

                auto source =
                    render (*file, compiled.start, compiled.end - compiled.start, std::exp2 (semitones / 12.0));
                complete = complete && source != nullptr;
                compiled.pitched.push_back ({ semitones, std::move (source) });
            }

            if (! complete)
                compiled.pitched.clear();
        }

        map->zones.push_back (std::move (compiled));
    }

    map->takes.assign (map->zones.size(), 0);

    return map;
}

} // namespace dawg
