#include "dawg/DrumSynth.h"

#include "dawg/DrumVoices.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Sequence.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <string>

// The Drum Synth (docs/design/drum-synth.md, DR1-DR11): sixteen pads on keys 36-51, each one of the DrumVoice models,
// played from a pool of 32 voices. A pad's structure (model, output, choke group, mono) comes from the node's model
// data through configure(), packed into one atomic per pad; its sound from the node's parameters, read when a hit
// starts. Note-offs are ignored: every model is a one-shot.

namespace dawg
{

namespace
{
constexpr size_t voiceCount = 32;     // DR8
constexpr int voicesPerPad = 4;       // DR8
constexpr double fadeSeconds = 0.005; // a choked or stolen voice fades over 5 ms, as the Sampler's

// One override of a kit pad's default: a common parameter (model off) or one of a model's own.
struct Override
{
    DrumModel model = DrumModel::off;
    const char* id = nullptr;
    float value = 0.0f;
};

struct PadDefault
{
    const char* name;
    DrumModel model;
    int chokeGroup;
    bool mono;
    float level; // dB: the kit's balance, kick and snare at 0
    std::array<Override, 7> overrides;
};

using M = DrumModel;

// The default kit, General MIDI's keys 36-51 (DR1): toms at six tunings, hats in choke group 1, crash and ride.
const std::array<PadDefault, drumPadCount> kit { {
    { "Kick", M::kick, 0, true, 0.0f, {} },
    { "Rim", M::rim, 0, false, -4.0f, {} },
    { "Snare", M::snare, 0, false, 0.0f, {} },
    { "Clap", M::clap, 0, false, -2.0f, {} },
    { "Snare 2",
      M::snare,
      0,
      false,
      -1.0f,
      { { { M::snare, "pitch", 220.0f },
          { M::snare, "snappy", 80.0f },
          { M::snare, "noiseTone", 7000.0f },
          { M::off, "decay", 80.0f } } } },
    { "Low floor tom", M::tom, 0, true, -5.0f, { { { M::tom, "pitch", 75.0f } } } },
    { "Closed hat", M::hat, 1, false, -9.0f, { { { M::off, "decay", 35.0f } } } },
    { "High floor tom", M::tom, 0, true, -5.0f, { { { M::tom, "pitch", 90.0f } } } },
    { "Pedal hat", M::hat, 1, false, -11.0f, { { { M::off, "decay", 25.0f }, { M::hat, "tone", 6000.0f } } } },
    { "Low tom", M::tom, 0, true, -5.0f, { { { M::tom, "pitch", 110.0f } } } },
    { "Open hat", M::hat, 1, false, -9.0f, { { { M::off, "decay", 300.0f } } } },
    { "Low-mid tom", M::tom, 0, true, -5.0f, { { { M::tom, "pitch", 135.0f } } } },
    { "High-mid tom", M::tom, 0, true, -5.0f, { { { M::tom, "pitch", 160.0f } } } },
    { "Crash", M::cymbal, 0, false, -10.0f, {} },
    { "High tom", M::tom, 0, true, -5.0f, { { { M::tom, "pitch", 195.0f } } } },
    { "Ride",
      M::cymbal,
      0,
      false,
      -12.0f,
      { { { M::cymbal, "ride", 100.0f },
          { M::cymbal, "tone", 9000.0f },
          { M::cymbal, "colour", 50.0f },
          { M::cymbal, "wash", 25.0f },
          { M::cymbal, "sizzle", 12.0f },
          { M::cymbal, "swell", 0.0f },
          { M::cymbal, "width", 40.0f } } } },
} };

// Every pad's parameters in one list: per pad the common five, then each model's own in model order.
struct Table
{
    std::deque<std::string> strings; // ids and names, whose c_str() the infos point at
    std::vector<ParamInfo> infos;
    std::array<size_t, drumModelCount> offsets {}; // within a pad: model m's first parameter (0 for the common ones)
    size_t perPad = 0;
};

[[nodiscard]] const char* keep (Table& table, std::string text)
{
    return table.strings.emplace_back (std::move (text)).c_str();
}

[[nodiscard]] float defaultFor (int pad, DrumModel model, const ParamInfo& info)
{
    // The kit's balance: toms 5 dB and hats and cymbals 9 to 12 dB under the kick and snare.
    if (model == DrumModel::off && std::strcmp (info.id, "level") == 0)
        return kit[static_cast<size_t> (pad)].level;

    for (const auto& o : kit[static_cast<size_t> (pad)].overrides)
        if (o.id != nullptr && o.model == model && std::strcmp (o.id, info.id) == 0)
            return o.value;

    return info.defaultValue;
}

[[nodiscard]] Table buildTable()
{
    Table table;
    auto next = static_cast<size_t> (drumCommonCount);

    for (size_t m = 1; m < drumModelCount; ++m)
    {
        table.offsets[m] = next;
        next += drumModelParams (static_cast<DrumModel> (m)).size();
    }

    table.perPad = next;
    table.infos.reserve (table.perPad * drumPadCount);

    for (int pad = 0; pad < drumPadCount; ++pad)
    {
        const auto number = std::to_string (pad + 1);

        for (const auto& common : drumCommonParams())
        {
            auto info = common;
            info.id = keep (table, "p" + number + "." + common.id);
            info.name = keep (table, "Pad " + number + " " + common.name);
            info.defaultValue = defaultFor (pad, DrumModel::off, common);
            table.infos.push_back (info);
        }

        for (size_t m = 1; m < drumModelCount; ++m)
        {
            const auto model = static_cast<DrumModel> (m);

            for (const auto& own : drumModelParams (model))
            {
                auto info = own;
                info.id = keep (table, "p" + number + "." + drumModelId (model) + "." + own.id);
                info.name = keep (table, "Pad " + number + " " + drumModelName (model) + " " + own.name);
                info.defaultValue = defaultFor (pad, model, own);
                table.infos.push_back (info);
            }
        }
    }

    return table;
}

[[nodiscard]] const Table& table()
{
    static const Table built = buildTable();
    return built;
}

constexpr std::array<PortInfo, 1> notesPort { { { PortType::events, "Notes" } } };
constexpr auto drumInputs = withModulationInputs (notesPort);

constexpr std::array<PortInfo, maxDrumOutputs> drumOutputs { {
    { PortType::audio, "Main" },
    { PortType::audio, "Out 2" },
    { PortType::audio, "Out 3" },
    { PortType::audio, "Out 4" },
    { PortType::audio, "Out 5" },
    { PortType::audio, "Out 6" },
    { PortType::audio, "Out 7" },
    { PortType::audio, "Out 8" },
    { PortType::audio, "Out 9" },
    { PortType::audio, "Out 10" },
    { PortType::audio, "Out 11" },
    { PortType::audio, "Out 12" },
    { PortType::audio, "Out 13" },
    { PortType::audio, "Out 14" },
    { PortType::audio, "Out 15" },
    { PortType::audio, "Out 16" },
} };

// A pad's structure in one word, for the audio thread: model, output (0-based), choke group and mono.
[[nodiscard]] std::uint32_t pack (const DrumPad& pad) noexcept
{
    const auto model = static_cast<std::uint32_t> (pad.model) & 0xfU;
    const auto output = static_cast<std::uint32_t> (std::clamp (pad.output, 1, maxDrumOutputs) - 1) & 0x1fU;
    const auto choke = static_cast<std::uint32_t> (std::clamp (pad.chokeGroup, 0, maxDrumChokeGroup)) & 0xfU;
    return model | (output << 4U) | (choke << 9U) | ((pad.mono ? 1U : 0U) << 13U);
}

struct PadState
{
    DrumModel model = DrumModel::off;
    size_t output = 0;
    int chokeGroup = 0;
    bool mono = false;
};

[[nodiscard]] PadState unpack (std::uint32_t word) noexcept
{
    return { static_cast<DrumModel> (std::min<std::uint32_t> (word & 0xfU, drumModelCount - 1)), (word >> 4U) & 0x1fU,
             static_cast<int> ((word >> 9U) & 0xfU), ((word >> 13U) & 1U) != 0 };
}

class DrumSynthNode final : public Node
{
public:
    DrumSynthNode()
        : Node (drumSynthDescriptor())
    {
        const auto pads = defaultDrumPads();

        for (size_t i = 0; i < m_padStates.size(); ++i)
            m_padStates[i].store (pack (pads[i]));
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        reset();
    }

    void reset() noexcept override
    {
        for (auto& voice : m_voices)
            voice.prepare (m_sampleRate); // silences it

        m_voicePads.fill (-1);
        m_counts.fill (0);
        m_order = 0;
    }

    void configure (const GraphNode& node) override
    {
        m_bypassed.store (node.bypassed);
        m_nodeId.store (static_cast<std::uint32_t> (node.id));
        const auto pads = drumPadsOf (node);

        for (size_t i = 0; i < m_padStates.size() && i < pads.size(); ++i)
            m_padStates[i].store (pack (pads[i]));
    }

    void process (const ProcessContext& context) noexcept override
    {
        const auto clearOutputs = [&]
        {
            for (size_t port = 0; port < context.audioOutputs.size(); ++port)
                if (port == 0 || (port < 32 && (context.wiredOutputs & (1U << port)) != 0))
                    for (auto* channel : context.audioOutputs[port])
                        juce::FloatVectorOperations::clear (channel, context.numSamples);
        };

        clearOutputs();
        auto position = 0;

        if (context.eventsIn != nullptr)
        {
            for (const auto& event : context.eventsIn->events())
            {
                render (context, position, event.sampleOffset);
                position = std::max (position, event.sampleOffset);
                handle (context, event);
            }
        }

        render (context, position, context.numSamples);

        if (m_bypassed.load())
            clearOutputs();
    }

    [[nodiscard]] juce::var meters() const override
    {
        juce::Array<juce::var> hits;

        for (const auto& count : m_hits)
            hits.add (static_cast<int> (count.load (std::memory_order_relaxed)));

        juce::var result (new juce::DynamicObject());
        result.getDynamicObject()->setProperty ("hits", hits);
        result.getDynamicObject()->setProperty ("voices", m_sounding.load (std::memory_order_relaxed));
        return result;
    }

private:
    void handle (const ProcessContext& context, const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case NoteEventType::noteOn:
            case NoteEventType::legato:
                if (event.note != nullptr)
                    hit (context, event);
                break;

            case NoteEventType::chokeAll:
            case NoteEventType::sourceChanged:
                for (auto& voice : m_voices)
                    voice.choke (fadeSeconds);

                m_counts.fill (0); // a seek or a start plays the same hits again (DR11)
                break;

            case NoteEventType::noteOff:
            case NoteEventType::choke:
            case NoteEventType::releaseAll:
            case NoteEventType::expression:
            case NoteEventType::sustain:
            case NoteEventType::controller:
                break; // one-shots: a note's end, expression and pedals change nothing
        }
    }

    void hit (const ProcessContext& context, const NoteEvent& event) noexcept
    {
        const auto pad = static_cast<int> (std::lround (event.note->pitch)) - firstDrumKey;

        if (pad < 0 || pad >= drumPadCount)
            return;

        const auto state = unpack (m_padStates[static_cast<size_t> (pad)].load());

        if (state.model == DrumModel::off)
            return;

        // DR8: a hit fades the other pads of its group, and its own pad's last hit when mono.
        for (size_t v = 0; v < voiceCount; ++v)
        {
            if (m_voicePads[v] < 0 || ! m_voices[v].active())
                continue;

            const auto other = m_voicePads[v];
            const auto otherGroup = unpack (m_padStates[static_cast<size_t> (other)].load()).chokeGroup;

            if ((other != pad && state.chokeGroup > 0 && otherGroup == state.chokeGroup)
                || (other == pad && state.mono))
                m_voices[v].choke (fadeSeconds);
        }

        auto drumHit = DrumHit::defaults (state.model);
        const auto& t = table();
        const auto base = static_cast<size_t> (pad) * t.perPad;
        const auto valueAt = [&] (size_t index)
        {
            const auto* modulated = context.modulatedValues (index);
            return modulated != nullptr ? modulated[event.sampleOffset] : params().get (index);
        };

        for (size_t c = 0; c < drumCommonCount; ++c)
            drumHit.common[c] = valueAt (base + c);

        const auto own = drumModelParams (state.model).size();

        for (size_t i = 0; i < own; ++i)
            drumHit.values[i] = valueAt (base + t.offsets[static_cast<size_t> (state.model)] + i);

        drumHit.velocity = std::max (1.0f, event.note->velocity * 127.0f);
        auto& count = m_counts[static_cast<size_t> (pad)];
        drumHit.seed =
            (m_nodeId.load() * 0x9e3779b1U) ^ (static_cast<std::uint32_t> (pad) * 0x85ebca77U) ^ (count * 0xc2b2ae3dU);
        ++count;

        // Section 3.5: a cymbal rings on, and a new hit strikes the one sounding (unless the pad is mono).
        if (state.model == DrumModel::cymbal && ! state.mono)
        {
            const auto newest = newestOf (pad);

            if (newest >= 0 && m_voices[static_cast<size_t> (newest)].restrike (drumHit))
            {
                m_voiceOrder[static_cast<size_t> (newest)] = ++m_order;
                m_hits[static_cast<size_t> (pad)].fetch_add (1, std::memory_order_relaxed);
                return;
            }
        }

        // At most voicesPerPad of a pad sound at once: the pad's oldest fades.
        if (countOf (pad) >= voicesPerPad)
            if (const auto oldest = oldestOf (pad, true); oldest >= 0)
                m_voices[static_cast<size_t> (oldest)].choke (fadeSeconds);

        const auto slot = freeVoice();
        m_voices[slot].trigger (drumHit);
        m_voicePads[slot] = pad;
        m_voiceOrder[slot] = ++m_order;
        m_hits[static_cast<size_t> (pad)].fetch_add (1, std::memory_order_relaxed);
    }

    [[nodiscard]] int countOf (int pad) const noexcept
    {
        int count = 0;

        for (size_t v = 0; v < voiceCount; ++v)
            count += m_voicePads[v] == pad && m_voices[v].active() && ! m_voices[v].choking() ? 1 : 0;

        return count;
    }

    // The pad's (or, for -1, any pad's) oldest voice, or -1; `unchoked` skips voices already fading.
    [[nodiscard]] int oldestOf (int pad, bool unchoked) const noexcept
    {
        int best = -1;

        for (size_t v = 0; v < voiceCount; ++v)
        {
            if (! m_voices[v].active() || (pad >= 0 && m_voicePads[v] != pad) || (unchoked && m_voices[v].choking()))
                continue;

            if (best < 0 || m_voiceOrder[v] < m_voiceOrder[static_cast<size_t> (best)])
                best = static_cast<int> (v);
        }

        return best;
    }

    // The pad's newest voice that isn't fading, or -1.
    [[nodiscard]] int newestOf (int pad) const noexcept
    {
        int best = -1;

        for (size_t v = 0; v < voiceCount; ++v)
        {
            if (! m_voices[v].active() || m_voicePads[v] != pad || m_voices[v].choking())
                continue;

            if (best < 0 || m_voiceOrder[v] > m_voiceOrder[static_cast<size_t> (best)])
                best = static_cast<int> (v);
        }

        return best;
    }

    // A silent voice, else the oldest fading one, else the oldest of all (cut short).
    [[nodiscard]] size_t freeVoice() const noexcept
    {
        for (size_t v = 0; v < voiceCount; ++v)
            if (! m_voices[v].active())
                return v;

        for (const auto unchoked : { false, true })
        {
            int best = -1;

            for (size_t v = 0; v < voiceCount; ++v)
                if ((unchoked || m_voices[v].choking())
                    && (best < 0 || m_voiceOrder[v] < m_voiceOrder[static_cast<size_t> (best)]))
                    best = static_cast<int> (v);

            if (best >= 0)
                return static_cast<size_t> (best);
        }

        return 0;
    }

    void render (const ProcessContext& context, int from, int to) noexcept
    {
        if (to <= from)
            return;

        int sounding = 0;

        for (size_t v = 0; v < voiceCount; ++v)
        {
            if (m_voicePads[v] < 0)
                continue;

            if (! m_voices[v].active())
            {
                m_voicePads[v] = -1;
                continue;
            }

            // A pad's output, or Main while nothing is wired to it (MO2).
            auto port = unpack (m_padStates[static_cast<size_t> (m_voicePads[v])].load()).output;

            if (port >= context.audioOutputs.size() || port >= 32 || (context.wiredOutputs & (1U << port)) == 0)
                port = 0;

            const auto& out = context.audioOutputs[port];
            m_voices[v].render (out[0] + from, out[1] + from, to - from);
            sounding += m_voices[v].active() ? 1 : 0;
        }

        m_sounding.store (sounding, std::memory_order_relaxed);
    }

    std::array<DrumVoice, voiceCount> m_voices {};
    std::array<int, voiceCount> m_voicePads {};
    std::array<std::uint64_t, voiceCount> m_voiceOrder {};
    std::uint64_t m_order = 0;
    std::array<std::uint32_t, drumPadCount> m_counts {};
    std::array<std::atomic<std::uint32_t>, drumPadCount> m_padStates {};
    std::array<std::atomic<std::uint32_t>, drumPadCount> m_hits {};
    std::atomic<int> m_sounding { 0 };
    std::atomic<std::uint32_t> m_nodeId { 0 };
    std::atomic<bool> m_bypassed { false };
    double m_sampleRate = 48000.0;
};
} // namespace

size_t drumParamsPerPad() noexcept
{
    return table().perPad;
}

juce::String drumParamId (int pad, DrumModel model, const char* id)
{
    auto result = "p" + juce::String (pad + 1) + ".";

    if (model != DrumModel::off)
        result << drumModelId (model) << ".";

    return result + id;
}

int drumParamIndex (int pad, DrumModel model, const char* id) noexcept
{
    if (pad < 0 || pad >= drumPadCount)
        return -1;

    const auto& t = table();
    const auto params = model == DrumModel::off ? drumCommonParams() : drumModelParams (model);

    for (size_t i = 0; i < params.size(); ++i)
        if (std::strcmp (params[i].id, id) == 0)
            return static_cast<int> (static_cast<size_t> (pad) * t.perPad + t.offsets[static_cast<size_t> (model)] + i);

    return -1;
}

std::optional<DrumModel> drumModelFromId (const juce::String& id) noexcept
{
    for (size_t m = 0; m < drumModelCount; ++m)
        if (id == drumModelId (static_cast<DrumModel> (m)))
            return static_cast<DrumModel> (m);

    return std::nullopt;
}

std::vector<DrumPad> defaultDrumPads()
{
    std::vector<DrumPad> pads;
    pads.reserve (drumPadCount);

    for (const auto& pad : kit)
        pads.push_back ({ pad.name, pad.model, 1, pad.chokeGroup, pad.mono, false });

    return pads;
}

std::vector<DrumPad> drumPadsOf (const GraphNode& node)
{
    return node.drumPads.size() == static_cast<size_t> (drumPadCount) ? node.drumPads : defaultDrumPads();
}

const NodeDescriptor& drumSynthDescriptor()
{
    static const NodeDescriptor descriptor {
        nodetype::drumSynth, "Drum Synth", drumInputs, drumOutputs, table().infos, true, NodeCategory::instrument
    };
    return descriptor;
}

void registerDrumSynth (NodeRegistry& registry)
{
    registry.add (drumSynthDescriptor(), [] { return std::make_unique<DrumSynthNode>(); });
}

} // namespace dawg
