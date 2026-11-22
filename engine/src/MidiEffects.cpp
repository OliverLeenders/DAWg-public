// The MIDI effects: Arpeggiator, Chord, Scale Lock and Note Repeat (docs/design/midi-effects.md).
//
// Each takes note events and gives note events. A note it makes is a copy of the note it came from (its source), with its
// own negative id and pitch, kept in the effect so the pointer it sends stays valid (ME4). The copies are found again in
// the sequence by their root note's id when it changes. Slots are handed out round-robin, so one that ended keeps its
// values while an instrument plays its release.

#include "dawg/MidiEffects.h"

#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Realtime.h"
#include "dawg/Scales.h"
#include "dawg/Sequence.h"
#include "dawg/StepSequencer.h"
#include "dawg/Swing.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace dawg
{

namespace midifx
{

std::array<int, 6> invert (const std::array<int, 6>& notes, int count, int inversion) noexcept
{
    auto result = notes;
    count = std::clamp (count, 1, 6);
    inversion = std::clamp (inversion, -(count - 1), count - 1);

    // The order of the notes by pitch: lowest first.
    std::array<int, 6> order { 0, 1, 2, 3, 4, 5 };
    std::sort (order.begin(), order.begin() + count,
               [&] (int a, int b) { return notes[static_cast<size_t> (a)] < notes[static_cast<size_t> (b)]; });

    for (int i = 0; i < std::abs (inversion); ++i)
    {
        if (inversion > 0)
            result[static_cast<size_t> (order[static_cast<size_t> (i)])] += 12;
        else
            result[static_cast<size_t> (order[static_cast<size_t> (count - 1 - i)])] -= 12;
    }

    return result;
}

ArpPick arpPick (ArpMode mode, int count, int octaves, juce::uint64 step, juce::uint32 random) noexcept
{
    count = std::max (1, count);
    octaves = std::clamp (octaves, 1, 4);
    const auto length = static_cast<juce::uint64> (count) * static_cast<juce::uint64> (octaves);
    const auto pick = [&] (juce::uint64 index) -> ArpPick
    {
        return { static_cast<int> (index % static_cast<juce::uint64> (count)),
                 static_cast<int> (index / static_cast<juce::uint64> (count)) };
    };

    switch (mode)
    {
        case ArpMode::up:
        case ArpMode::asPlayed:
            return pick (step % length);

        case ArpMode::down:
            return pick (length - 1 - step % length);

        case ArpMode::upDown:
        case ArpMode::downUp:
        {
            if (length == 1)
                return pick (0);

            const auto cycle = 2 * length - 2;
            const auto p = step % cycle;
            const auto index = p < length ? p : cycle - p;
            return pick (mode == ArpMode::upDown ? index : length - 1 - index);
        }

        case ArpMode::upAndDown:
        {
            const auto p = step % (2 * length);
            return pick (p < length ? p : 2 * length - 1 - p);
        }

        case ArpMode::random:
            return pick (static_cast<juce::uint64> (random) % length);

        case ArpMode::chord:
            return { -1, static_cast<int> (step % static_cast<juce::uint64> (octaves)) };
    }

    return pick (0);
}

double nextGridLine (const TempoMap& map, double tick, double stepTicks, double swing, bool after) noexcept
{
    const auto bar = map.barAt (std::max (0.0, tick)).bar;
    const auto start = static_cast<double> (map.tickOfBar (bar));
    const auto next = static_cast<double> (map.tickOfBar (bar + 1));
    const auto step = std::min (stepTicks, next - start);

    for (auto k = std::max (0LL, static_cast<long long> (std::floor ((tick - start) / step)) - 1);; ++k)
    {
        const auto line = start + swing::line (k, step, swing);

        if (line >= next)
            break;

        if (after ? line > tick : line >= tick)
            return line;
    }

    return next;
}

} // namespace midifx

namespace
{
using namespace midifx;

constexpr std::array<PortInfo, 1> notesPort { { { PortType::events, "Notes" } } };
constexpr auto notesInputs = withModulationInputs (notesPort);

// The arpeggiator's rates, 1/32 to 1/1, and note repeat's, 1/64T to 1/4: runs of the LFO's note values.
constexpr size_t arpRateFirst = 4;
constexpr size_t arpRateCount = 16;
constexpr size_t repeatRateFirst = 0;
constexpr size_t repeatRateCount = 14;
constexpr std::span<const char* const> arpRates { lfoCycleChoices.data() + arpRateFirst, arpRateCount };
constexpr std::span<const char* const> repeatRates { lfoCycleChoices.data() + repeatRateFirst, repeatRateCount };

[[nodiscard]] double rateTicks (size_t first, size_t count, float choice) noexcept
{
    const auto index = first + std::min (count - 1, static_cast<size_t> (std::max (0.0f, std::round (choice))));
    return lfoCycleBeats[index] * static_cast<double> (ticksPerBeat);
}

constexpr std::array<const char*, 2> velocityChoices { "From note", "Fixed" };
constexpr std::array<const char*, 8> arpModes { "Up",        "Down",      "Up-Down", "Down-Up",
                                                "Up & Down", "As played", "Random",  "Chord" };
constexpr std::array<const char*, 2> retriggerChoices { "Off", "On new note" };
constexpr std::array<const char*, 12> rootChoices {
    "C", "C\xe2\x99\xaf", "D", "D\xe2\x99\xaf", "E", "F", "F\xe2\x99\xaf",
    "G", "G\xe2\x99\xaf", "A", "A\xe2\x99\xaf", "B"
};
constexpr std::array<const char*, 4> lockModes { "Nearest", "Up", "Down", "Filter" };
constexpr std::array<const char*, 2> followChoices { "Own", "Project key" };
constexpr std::array<const char*, 2> strumOrders { "Up", "Down" };

constexpr auto scaleNames = []
{
    std::array<const char*, scales.size()> names {};

    for (size_t i = 0; i < scales.size(); ++i)
        names[i] = scales[i].name;

    return names;
}();

constexpr auto shapeNames = []
{
    std::array<const char*, chordShapes.size() + 1> names {};

    for (size_t i = 0; i < chordShapes.size(); ++i)
        names[i] = chordShapes[i].name;

    names[chordShapes.size()] = "Custom";
    return names;
}();

constexpr auto customShape = static_cast<float> (chordShapes.size());

// ---------------------------------------------------------------------------------------------------------
// What the four effects share: the notes coming in, the notes going out, bypass and the sequence.

constexpr size_t maxSources = 128;
constexpr size_t maxDerived = 1024;
constexpr auto never = std::numeric_limits<juce::int64>::max();

// The sequence note a note's start and curves are those of: 0 for a live one.
[[nodiscard]] ObjectId rootOf (const CompiledNote& note) noexcept
{
    return note.source != 0 ? note.source : std::max<ObjectId> (0, note.id);
}

// A sequence edit: the copy takes on its root's timing, curves and vibrato.
void refresh (CompiledNote& copy, const CompiledNote& root) noexcept
{
    copy.start = root.start;
    copy.end = root.end;
    copy.lanes = root.lanes;
    copy.vibrato = root.vibrato;
    copy.holdUntil = root.holdUntil;
}

class MidiEffectNode : public Node
{
public:
    explicit MidiEffectNode (const NodeDescriptor& descriptor)
        : Node (descriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override { m_sampleRate = spec.sampleRate; }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    void process (const ProcessContext& context) noexcept final
    {
        m_context = &context;
        m_out = context.eventsOut;
        m_position = 0;

        if (context.sequence != nullptr && context.sequence != m_sequence)
            adoptSequence (*context.sequence);

        const auto bypassed = m_bypassed.load();

        if (bypassed != m_wasBypassed)
        {
            m_wasBypassed = bypassed;

            if (bypassed)
                startBypass();
            else
                endBypass();
        }

        if (! bypassed)
            onBlock();

        if (context.eventsIn != nullptr)
        {
            for (const auto& event : context.eventsIn->events())
            {
                runUntil (std::clamp (event.sampleOffset, 0, context.numSamples - 1));
                m_event = &event;
                handle (event);
                m_event = nullptr;
            }
        }

        runUntil (context.numSamples);
        m_now += context.numSamples;
        m_context = nullptr;
        m_out = nullptr;
    }

protected:
    struct Source
    {
        bool used = false;
        bool held = false;     // its note-off hasn't come
        bool latched = false;  // the arpeggiator keeps it after its note-off
        bool passed = false;   // it plays as itself downstream, under its own id
        bool filtered = false; // Scale Lock dropped it
        ObjectId id = 0;
        CompiledNote note;
        juce::uint64 order = 0;
        std::array<float, 3> live {};
        std::array<bool, 3> hasLive {};
        juce::int64 lastHit = std::numeric_limits<juce::int64>::min();
    };

    enum class State
    {
        free,
        pending,
        sounding,
    };

    struct Derived
    {
        State state = State::free;
        bool passthrough = false; // a held source sent as itself when bypass was turned on
        ObjectId sourceId = 0;
        CompiledNote note;
        juce::int64 startAt = 0;
        juce::int64 offAt = never;
        int voice = 0;
    };

    // ---- for the effects

    // A made note's velocity: its source's, or with the Velocity choice on Fixed, a fixed one (1..127).
    [[nodiscard]] static float velocityFor (const Source& source, float mode, float fixed) noexcept
    {
        return mode >= 0.5f ? fixed / 127.0f : source.note.velocity;
    }

    [[nodiscard]] const ProcessContext& context() const noexcept { return *m_context; }
    [[nodiscard]] const Transport& transport() const noexcept { return *m_context->transport; }
    [[nodiscard]] float param (size_t index) const noexcept { return params().get (index); }
    [[nodiscard]] juce::int64 now() const noexcept { return m_now + m_position; }
    [[nodiscard]] juce::int64 blockStart() const noexcept { return m_now; }
    [[nodiscard]] int position() const noexcept { return m_position; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] std::span<Source> sources() noexcept { return m_sources; }
    [[nodiscard]] const NoteEvent* currentEvent() const noexcept { return m_event; }

    // The sample (on this node's clock) a tick plays at, in this block's transport.
    [[nodiscard]] juce::int64 sampleOfTick (double tick) const noexcept
    {
        const auto& t = transport();
        return m_now + std::llround (t.timeMap.tickToSample (tick)) - t.startSample;
    }

    // Makes a note from a source. It plays at `at` (now if at or before), and ends at `offAt` unless that's never.
    Derived* startDerived (const Source& source, double pitch, float velocity, juce::int64 at, juce::int64 offAt,
                           int voice) noexcept
    {
        auto* slot = allocate();

        if (slot == nullptr)
            return nullptr;

        slot->note = source.note;
        slot->note.id = derivedNoteId (serial(), m_counter++);
        slot->note.source = rootOf (source.note);
        slot->note.pitch = pitch;
        slot->note.velocity = std::clamp (velocity, 0.0f, 1.0f);
        slot->note.linkTo = 0;
        slot->note.linkFrom = 0;
        slot->note.glideSeconds = 0.0;
        slot->sourceId = source.id;
        slot->startAt = std::max (at, now());
        slot->offAt = offAt;
        slot->voice = voice;
        slot->passthrough = false;
        slot->state = State::pending;

        if (slot->startAt <= now())
            play (*slot, nullptr);

        return slot;
    }

    // Makes a note of the effect's own, from no source (the step sequencer): it plays at `at` and ends at `offAt`.
    Derived* startOwn (const CompiledNote& note, juce::int64 at, juce::int64 offAt) noexcept
    {
        auto* slot = allocate();

        if (slot == nullptr)
            return nullptr;

        slot->note = note;
        slot->note.id = derivedNoteId (serial(), m_counter++);
        slot->note.source = 0;
        slot->sourceId = 0;
        slot->startAt = std::max (at, now());
        slot->offAt = std::max (offAt, slot->startAt + 1);
        slot->voice = 0;
        slot->passthrough = false;
        slot->state = State::pending;

        if (slot->startAt <= now())
            play (*slot, nullptr);

        return slot;
    }

    // Starts a note as the continuation of another note's voice (a legato from `fromId`).
    Derived* startLegato (const Source& source, double pitch, float velocity, ObjectId fromId, int voice) noexcept
    {
        auto* slot = allocate();

        if (slot == nullptr)
            return nullptr;

        slot->note = source.note;
        slot->note.id = derivedNoteId (serial(), m_counter++);
        slot->note.source = rootOf (source.note);
        slot->note.pitch = pitch;
        slot->note.velocity = std::clamp (velocity, 0.0f, 1.0f);
        slot->note.linkTo = 0;
        slot->note.linkFrom = fromId;
        slot->sourceId = source.id;
        slot->startAt = now();
        slot->offAt = never;
        slot->voice = voice;
        slot->passthrough = false;
        play (*slot, &fromId);
        return slot;
    }

    // Plays a source as itself: the incoming event as it is, when it's the source's, or a copy under the source's id.
    void playAsItself (Source& source) noexcept
    {
        source.passed = true;

        if (m_event != nullptr && m_event->noteId == source.id
            && (m_event->type == NoteEventType::noteOn || m_event->type == NoteEventType::legato))
        {
            auto event = *m_event;
            event.sampleOffset = m_position;
            event.type = NoteEventType::noteOn;
            event.fromNoteId = 0;
            add (event);
            return;
        }

        auto* slot = allocate();

        if (slot == nullptr)
            return;

        slot->note = source.note;
        slot->sourceId = source.id;
        slot->passthrough = true;
        slot->offAt = never;
        slot->voice = 0;
        play (*slot, nullptr);
    }

    // Ends one note: a note-off, or a choke. A pending one just goes.
    void end (Derived& slot, bool choke) noexcept
    {
        if (slot.state == State::sounding)
            add ({ m_position, choke ? NoteEventType::choke : NoteEventType::noteOff, slot.note.id, nullptr });

        slot.state = State::free;
    }

    // Forgets a note without ending it: its voice went on as another.
    static void forget (Derived& slot) noexcept { slot.state = State::free; }

    // Ends the notes made from a source.
    void endOutputsOf (ObjectId sourceId, bool choke) noexcept
    {
        for (auto& slot : m_derived)
            if (slot.state != State::free && slot.sourceId == sourceId)
                end (slot, choke);
    }

    // Ends every note made, passthroughs aside.
    void endAllOutputs (bool choke) noexcept
    {
        for (auto& slot : m_derived)
            if (slot.state != State::free && ! slot.passthrough)
                end (slot, choke);
    }

    template <typename Visit>
    void forEachOutput (ObjectId sourceId, Visit&& visit) noexcept
    {
        for (auto& slot : m_derived)
            if (slot.state != State::free && ! slot.passthrough && slot.sourceId == sourceId)
                visit (slot);
    }

    [[nodiscard]] Source* findSource (ObjectId id) noexcept
    {
        for (auto& source : m_sources)
            if (source.used && source.id == id)
                return &source;

        return nullptr;
    }

    void removeSource (Source& source) noexcept { source = {}; }

    // ---- what the effects implement

    // A note arrives (not bypassed).
    virtual void onStart (Source& source) noexcept = 0;

    // A note ends (not bypassed, not passed through). Returns true to keep the source (latch).
    virtual bool onEnd (Source& source, bool choke) noexcept
    {
        endOutputsOf (source.id, choke);
        return false;
    }

    // A linked note continues `from` as `to`. By default the one ends and the other starts.
    virtual void onLegato (Source& from, Source& to) noexcept
    {
        endSource (from, false);
        onStart (to);
    }

    // At the start of each block that isn't bypassed, before its events.
    virtual void onBlock() noexcept {}

    // The next step of a timed effect on this node's clock, or never.
    [[nodiscard]] virtual juce::int64 nextStep() noexcept { return never; }

    // Plays the step nextStep() named; now() is its time.
    virtual void step() noexcept {}

    // Ends a source's notes, and the source unless onEnd keeps it.
    void endSource (Source& source, bool choke) noexcept
    {
        if (source.passed)
        {
            add ({ m_position, choke ? NoteEventType::choke : NoteEventType::noteOff, source.id, nullptr });

            for (auto& slot : m_derived)
                if (slot.passthrough && slot.sourceId == source.id)
                    forget (slot);

            removeSource (source);
            return;
        }

        if (! onEnd (source, choke))
            removeSource (source);
    }

    void add (const NoteEvent& event) noexcept
    {
        if (m_out != nullptr)
            std::ignore = m_out->add (event);
    }

private:
    [[nodiscard]] Derived* allocate() noexcept
    {
        for (size_t i = 0; i < m_derived.size(); ++i)
        {
            auto& slot = m_derived[m_cursor];
            m_cursor = (m_cursor + 1) % m_derived.size();

            if (slot.state == State::free)
                return &slot;
        }

        return nullptr;
    }

    // Sends a pending note's note-on (or legato from *fromId), then the source's live expression so far.
    void play (Derived& slot, const ObjectId* fromId) noexcept
    {
        slot.state = State::sounding;
        NoteEvent on { m_position, fromId != nullptr ? NoteEventType::legato : NoteEventType::noteOn, slot.note.id,
                       &slot.note };

        if (fromId != nullptr)
            on.fromNoteId = *fromId;

        add (on);

        if (slot.passthrough)
            return;

        if (const auto* source = findSource (slot.sourceId))
            for (size_t lane = 0; lane < source->live.size(); ++lane)
                if (source->hasLive[lane])
                    add ({ m_position, NoteEventType::expression, slot.note.id, nullptr, static_cast<int> (lane),
                           source->live[lane] });
    }

    // Plays what is due before an offset: notes starting and ending, and the effect's steps, in time order (an end
    // before a start at the same time, so a repeated key retriggers).
    void runUntil (int offset) noexcept
    {
        const auto limit = m_now + offset;

        for (;;)
        {
            Derived* due = nullptr;
            auto dueAt = never;
            auto dueStart = false;

            for (auto& slot : m_derived)
            {
                if (slot.state == State::sounding && slot.offAt < dueAt)
                {
                    due = &slot;
                    dueAt = slot.offAt;
                    dueStart = false;
                }
                else if (slot.state == State::pending && slot.startAt < dueAt)
                {
                    due = &slot;
                    dueAt = slot.startAt;
                    dueStart = true;
                }
            }

            const auto stepAt = m_wasBypassed ? never : nextStep();

            if (due != nullptr && dueAt < limit && dueAt <= stepAt)
            {
                m_position = std::clamp (static_cast<int> (dueAt - m_now), m_position, m_context->numSamples - 1);

                if (dueStart)
                    play (*due, nullptr);
                else
                    end (*due, false);

                continue;
            }

            if (stepAt < limit)
            {
                m_position = std::clamp (static_cast<int> (stepAt - m_now), m_position, m_context->numSamples - 1);
                step();
                continue;
            }

            break;
        }

        m_position = std::max (m_position, std::min (offset, m_context->numSamples - 1));
    }

    [[nodiscard]] Source* addSource (const NoteEvent& event) noexcept
    {
        for (auto& source : m_sources)
        {
            if (source.used)
                continue;

            source = {};
            source.used = true;
            source.held = true;
            source.id = event.noteId;
            source.note = *event.note;
            source.order = ++m_order;
            return &source;
        }

        return nullptr;
    }

    void handle (const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case NoteEventType::noteOn:
                handleNoteOn (event);
                return;

            case NoteEventType::legato:
                handleLegato (event);
                return;

            case NoteEventType::noteOff:
            case NoteEventType::choke:
                handleNoteEnd (event);
                return;

            case NoteEventType::expression:
                handleExpression (event);
                return;

            case NoteEventType::releaseAll:
            case NoteEventType::chokeAll:
                handleEndAll (event);
                return;

            case NoteEventType::sourceChanged:
                // What comes after forgets every note: start again from nothing.
                m_sources = {};

                for (auto& slot : m_derived)
                    slot.state = State::free;

                forward (event);
                return;

            case NoteEventType::sustain:
            case NoteEventType::controller:
                forward (event);
                return;
        }
    }

    void handleNoteOn (const NoteEvent& event) noexcept
    {
        auto* source = event.note != nullptr ? addSource (event) : nullptr;

        if (source == nullptr)
        {
            forward (event);
        }
        else if (m_wasBypassed)
        {
            source->passed = true;
            forward (event);
        }
        else
        {
            onStart (*source);
        }
    }

    void handleLegato (const NoteEvent& event) noexcept
    {
        auto* from = findSource (event.fromNoteId);
        auto* to = event.note != nullptr ? addSource (event) : nullptr;

        if (to == nullptr || m_wasBypassed)
        {
            if (to != nullptr)
                to->passed = true;

            if (from != nullptr)
                removeSource (*from);

            forward (event);
            return;
        }

        if (from == nullptr)
        {
            onStart (*to);
            return;
        }

        onLegato (*from, *to);

        if (from->used && ! from->latched)
            removeSource (*from);
    }

    void handleNoteEnd (const NoteEvent& event) noexcept
    {
        auto* source = findSource (event.noteId);

        if (source == nullptr)
        {
            forward (event);
            return;
        }

        source->held = false;

        if (source->passed)
        {
            forward (event);

            for (auto& slot : m_derived)
                if (slot.passthrough && slot.sourceId == source->id)
                    forget (slot);

            removeSource (*source);
            return;
        }

        if (! onEnd (*source, event.type == NoteEventType::choke))
            removeSource (*source);
    }

    void handleExpression (const NoteEvent& event) noexcept
    {
        auto* source = findSource (event.noteId);

        if (source == nullptr || source->passed)
        {
            forward (event);
            return;
        }

        if (event.lane >= 0 && event.lane < 3)
        {
            source->live[static_cast<size_t> (event.lane)] = event.value;
            source->hasLive[static_cast<size_t> (event.lane)] = true;
        }

        forEachOutput (source->id,
                       [&] (Derived& slot)
                       {
                           if (slot.state != State::sounding)
                               return;

                           auto copy = event;
                           copy.sampleOffset = m_position;
                           copy.noteId = slot.note.id;
                           add (copy);
                       });
    }

    // The transport stopped or jumped: notes from the sequence end, latched ones too; live ones play on. Notes from
    // another effect end with their own note-offs, which it sends.
    void handleEndAll (const NoteEvent& event) noexcept
    {
        const auto choke = event.type == NoteEventType::chokeAll;

        for (auto& source : m_sources)
        {
            if (! source.used || source.id < 0)
                continue;

            if (source.passed)
            {
                for (auto& slot : m_derived)
                    if (slot.passthrough && slot.sourceId == source.id)
                        forget (slot);
            }
            else
            {
                endOutputsOf (source.id, choke);
            }

            removeSource (source);
        }

        forward (event);
    }

    void forward (const NoteEvent& event) noexcept
    {
        auto copy = event;
        copy.sampleOffset = std::max (m_position, event.sampleOffset);
        add (copy);
    }

    void startBypass() noexcept
    {
        endAllOutputs (false);

        for (auto& source : m_sources)
        {
            if (! source.used || source.passed)
                continue;

            if (! source.held)
            {
                removeSource (source); // latched
                continue;
            }

            source.filtered = false;
            playAsItself (source);
        }
    }

    void endBypass() noexcept
    {
        for (auto& source : m_sources)
        {
            if (! source.used || ! source.passed)
                continue;

            add ({ m_position, NoteEventType::noteOff, source.id, nullptr });

            for (auto& slot : m_derived)
                if (slot.passthrough && slot.sourceId == source.id)
                    forget (slot);

            source.passed = false;
            onStart (source);
        }
    }

    // The sequence changed: copies follow their root notes, and notes whose root is gone stop.
    void adoptSequence (const TrackSequence& sequence) noexcept
    {
        m_sequence = &sequence;

        for (auto& source : m_sources)
        {
            const auto root = source.used ? rootOf (source.note) : 0;

            if (root == 0)
                continue;

            if (const auto* note = sequence.find (root))
            {
                refresh (source.note, *note);
                continue;
            }

            if (! source.passed)
                endOutputsOf (source.id, true);

            removeSource (source);
        }

        for (auto& slot : m_derived)
        {
            const auto root = slot.state != State::free ? rootOf (slot.note) : 0;

            if (root == 0)
                continue;

            if (const auto* note = sequence.find (root))
                refresh (slot.note, *note);
            else if (! slot.passthrough)
                end (slot, true);
        }
    }

    std::array<Source, maxSources> m_sources {};
    std::array<Derived, maxDerived> m_derived {};
    size_t m_cursor = 0;
    juce::uint32 m_counter = 0;
    juce::uint64 m_order = 0;
    juce::int64 m_now = 0;
    int m_position = 0;
    double m_sampleRate = 48000.0;
    const ProcessContext* m_context = nullptr;
    EventBuffer* m_out = nullptr;
    const NoteEvent* m_event = nullptr;
    const TrackSequence* m_sequence = nullptr;
    std::atomic<bool> m_bypassed { false };
    bool m_wasBypassed = false;
};

// ---------------------------------------------------------------------------------------------------------
// Scale Lock (ME12).

// Follow (docs/design/scale-highlighting.md, SH6): on Project key, the project's key replaces Root and Scale while
// one is set.
constexpr std::array<ParamInfo, 4> scaleLockParams { {
    { "root", "Root", 0.0f, 11.0f, 0.0f, ParamKind::choice, "", 0.0f, rootChoices },
    { "scale", "Scale", 0.0f, static_cast<float> (scales.size() - 1), 0.0f, ParamKind::choice, "", 0.0f, scaleNames },
    { "mode", "Mode", 0.0f, 3.0f, 0.0f, ParamKind::choice, "", 0.0f, lockModes },
    { "follow", "Follow", 0.0f, 1.0f, 1.0f, ParamKind::choice, "", 0.0f, followChoices },
} };

const NodeDescriptor scaleLockDescriptor {
    nodetype::scaleLock, "Scale Lock", notesInputs, notesPort, scaleLockParams, true, NodeCategory::midiEffect
};

class ScaleLockNode final : public MidiEffectNode
{
public:
    ScaleLockNode()
        : MidiEffectNode (scaleLockDescriptor)
    {
    }

    void setKeys (std::shared_ptr<const KeyTimeline> keys) override
    {
        if (keys != nullptr)
            m_keys.publish (std::move (keys));
    }

private:
    enum
    {
        rootParam,
        scaleParam,
        modeParam,
        followParam,
    };

    // Where a note goes: its pitch, or nothing if it is filtered out.
    [[nodiscard]] std::optional<double> locked (const CompiledNote& note) noexcept
    {
        const auto key = static_cast<int> (std::lround (note.pitch));
        const auto detune = note.pitch - key;
        // Following, the key in effect where the note plays (docs/design/scale-highlighting.md, SH9).
        const auto projectKey = std::lround (param (followParam)) == 1 ? keyNow() : ProjectKey {};
        const auto following = projectKey.enabled;
        const auto root = following ? projectKey.root : static_cast<int> (std::lround (param (rootParam)));
        const auto scale =
            std::min (scales.size() - 1, following ? static_cast<size_t> (projectKey.scale)
                                                   : static_cast<size_t> (std::lround (param (scaleParam))));
        const auto mode = static_cast<int> (std::lround (param (modeParam)));
        const auto mask = scales[scale].mask;

        if (mode == 3)
            return inScale (key, root, mask) ? std::optional (note.pitch) : std::nullopt;

        auto direction = ScaleDirection::nearest;

        if (mode == 1)
            direction = ScaleDirection::up;
        else if (mode == 2)
            direction = ScaleDirection::down;

        return lockToScale (key, root, mask, direction) + detune;
    }

    void onStart (Source& source) noexcept override
    {
        const auto pitch = locked (source.note);

        if (! pitch.has_value())
        {
            source.filtered = true;
            return;
        }

        if (std::abs (*pitch - source.note.pitch) < 1.0e-9)
            playAsItself (source);
        else
            std::ignore = startDerived (source, *pitch, source.note.velocity, now(), never, 0);
    }

    bool onEnd (Source& source, bool choke) noexcept override
    {
        if (! source.filtered)
            endOutputsOf (source.id, choke);

        return false;
    }

    void onLegato (Source& from, Source& to) noexcept override
    {
        const auto pitch = locked (to.note);

        // A filtered note drops its link: the voice it would continue ends, or it starts its own.
        if (! pitch.has_value())
        {
            to.filtered = true;
            endSource (from, false);
            return;
        }

        if (from.filtered)
        {
            onStart (to);
            return;
        }

        auto fromId = from.id;
        Derived* fromSlot = nullptr;

        if (! from.passed)
            forEachOutput (from.id, [&] (Derived& slot) { fromSlot = &slot; });

        if (fromSlot != nullptr)
        {
            if (fromSlot->state != State::sounding)
            {
                forget (*fromSlot);
                onStart (to);
                return;
            }

            fromId = fromSlot->note.id;
            forget (*fromSlot);
        }

        if (std::abs (*pitch - to.note.pitch) < 1.0e-9 && currentEvent() != nullptr)
        {
            to.passed = true;
            auto event = *currentEvent();
            event.fromNoteId = fromId;
            add (event);
            return;
        }

        std::ignore = startLegato (to, *pitch, to.note.velocity, fromId, 0);
    }

    // The key at the event being handled: at the playhead while stopped.
    [[nodiscard]] ProjectKey keyNow() noexcept
    {
        const auto* keys = m_keys.acquire();

        if (keys == nullptr)
            return {};

        const auto& t = transport();
        const auto samples = std::max (1, context().numSamples);
        return keys->at (t.startTick + (t.endTick - t.startTick) * position() / samples);
    }

    SnapshotExchange<KeyTimeline> m_keys;
};

// ---------------------------------------------------------------------------------------------------------
// Chord (ME11).

constexpr std::array<ParamInfo, 10> chordParams { {
    { "shape", "Shape", 0.0f, customShape, 0.0f, ParamKind::choice, "", 0.0f, shapeNames },
    { "note2", "Note 2", -24.0f, 24.0f, 4.0f, ParamKind::integer, "st" },
    { "note3", "Note 3", -24.0f, 24.0f, 7.0f, ParamKind::integer, "st" },
    { "note4", "Note 4", -24.0f, 24.0f, 0.0f, ParamKind::integer, "st" },
    { "note5", "Note 5", -24.0f, 24.0f, 0.0f, ParamKind::integer, "st" },
    { "note6", "Note 6", -24.0f, 24.0f, 0.0f, ParamKind::integer, "st" },
    { "inversion", "Inversion", -3.0f, 3.0f, 0.0f, ParamKind::integer },
    { "strum", "Strum", 0.0f, 200.0f, 0.0f, ParamKind::continuous, "ms" },
    { "strumOrder", "Strum order", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, strumOrders },
    { "velocity", "Velocity", 0.0f, 100.0f, 100.0f, ParamKind::continuous, "%" },
} };

const NodeDescriptor chordDescriptor { nodetype::chord,         "Chord", notesInputs, notesPort, chordParams, true,
                                       NodeCategory::midiEffect };

class ChordNode final : public MidiEffectNode
{
public:
    ChordNode()
        : MidiEffectNode (chordDescriptor)
    {
    }

private:
    enum
    {
        shapeParam = 0,
        note2Param = 1,
        inversionParam = 6,
        strumParam = 7,
        strumOrderParam = 8,
        velocityParam = 9,
    };

    struct Voicing
    {
        std::array<int, 6> notes {};
        int count = 1;
    };

    [[nodiscard]] Voicing voicing() const noexcept
    {
        Voicing result;
        const auto shape = static_cast<size_t> (std::lround (param (shapeParam)));

        if (shape < chordShapes.size())
        {
            result.notes = chordShapes[shape].notes;
            result.count = chordShapes[shape].count;
        }
        else
        {
            for (size_t i = 0; i < 5; ++i)
                if (const auto note = static_cast<int> (std::lround (param (note2Param + i))); note != 0)
                    result.notes[static_cast<size_t> (result.count++)] = note;
        }

        result.notes = invert (result.notes, result.count, static_cast<int> (std::lround (param (inversionParam))));
        return result;
    }

    [[nodiscard]] float velocityOf (const Source& source, int voice) const noexcept
    {
        return voice == 0 ? source.note.velocity : source.note.velocity * param (velocityParam) / 100.0f;
    }

    void onStart (Source& source) noexcept override
    {
        const auto chord = voicing();
        const auto strum = static_cast<double> (param (strumParam)) * sampleRate() / 1000.0;
        const auto down = param (strumOrderParam) >= 0.5f;

        // The strum goes through the notes by pitch.
        std::array<int, 6> rank {};

        for (int i = 0; i < chord.count; ++i)
            for (int j = 0; j < chord.count; ++j)
                if (chord.notes[static_cast<size_t> (j)] < chord.notes[static_cast<size_t> (i)]
                    || (chord.notes[static_cast<size_t> (j)] == chord.notes[static_cast<size_t> (i)] && j < i))
                    ++rank[static_cast<size_t> (i)];

        for (int i = 0; i < chord.count; ++i)
        {
            const auto order = down ? chord.count - 1 - rank[static_cast<size_t> (i)] : rank[static_cast<size_t> (i)];
            const auto at = now() + std::llround (strum * order);
            std::ignore = startDerived (source, source.note.pitch + chord.notes[static_cast<size_t> (i)],
                                        velocityOf (source, i), at, never, i);
        }
    }

    void onLegato (Source& from, Source& to) noexcept override
    {
        // Chord note i of one continues as chord note i of the next, without a strum.
        const auto chord = voicing();
        std::array<Derived*, 6> previous {};

        forEachOutput (from.id,
                       [&] (Derived& slot)
                       {
                           if (slot.voice >= 0 && slot.voice < 6)
                               previous[static_cast<size_t> (slot.voice)] = &slot;
                       });

        for (int i = 0; i < chord.count; ++i)
        {
            const auto pitch = to.note.pitch + chord.notes[static_cast<size_t> (i)];
            auto* before = previous[static_cast<size_t> (i)];
            previous[static_cast<size_t> (i)] = nullptr;

            if (before != nullptr && before->state == State::sounding)
            {
                const auto fromId = before->note.id;
                forget (*before);
                std::ignore = startLegato (to, pitch, velocityOf (to, i), fromId, i);
            }
            else
            {
                if (before != nullptr)
                    forget (*before);

                std::ignore = startDerived (to, pitch, velocityOf (to, i), now(), never, i);
            }
        }

        for (auto* rest : previous)
            if (rest != nullptr)
                end (*rest, false);
    }
};

// ---------------------------------------------------------------------------------------------------------
// What the arpeggiator and note repeat share: a step clock on the timeline's grid, or of its own while the transport
// is stopped (ME7, ME8).

class SteppedEffect : public MidiEffectNode
{
public:
    using MidiEffectNode::MidiEffectNode;

protected:
    [[nodiscard]] virtual double stepTicks() const noexcept = 0;
    [[nodiscard]] virtual double swingAmount() const noexcept = 0;
    [[nodiscard]] virtual bool hasKeys() noexcept = 0;
    virtual void playStep (juce::int64 offAfter) noexcept = 0;
    [[nodiscard]] virtual double gate() const noexcept = 0;

    // Starts the free clock at now, when keys arrive while nothing was held and the transport is stopped.
    void startFreeClock() noexcept
    {
        m_freePair = static_cast<double> (now());
        m_freeIndex = 0;
    }

    [[nodiscard]] juce::int64 stepLengthSamples() const noexcept
    {
        const auto& t = transport();
        return std::max<juce::int64> (1, std::llround (stepTicks() * t.timeMap.samplesPerTickAt (t.startTick)));
    }

    void onBlock() noexcept override
    {
        const auto& t = transport();

        if (t.started || t.stopped || t.jumped || t.looped)
        {
            // The grid starts again: steps sounding now end.
            endAllOutputs (false);
            m_freePair = static_cast<double> (blockStart());
            m_freeIndex = 0;
        }

        if (t.playing && (m_nextTick < t.startTick || t.started || t.jumped || t.looped))
            m_nextTick = nextGridLine (t.timeMap.tempoMap(), t.startTick, stepTicks(), swingAmount(), false);

        // Without keys, the free clock waits at the block start for the next one.
        if (! t.playing && ! hasKeys())
        {
            m_freePair = static_cast<double> (blockStart());
            m_freeIndex = 0;
        }
    }

    [[nodiscard]] juce::int64 nextStep() noexcept override
    {
        if (! hasKeys())
            return never;

        const auto& t = transport();

        if (t.playing)
            return m_nextTick < t.endTick ? std::max (blockStart(), sampleOfTick (m_nextTick)) : never;

        const auto length = static_cast<double> (stepLengthSamples());
        return std::llround (m_freePair + swing::line (m_freeIndex, length, swingAmount()));
    }

    void step() noexcept override
    {
        const auto& t = transport();

        if (t.playing)
        {
            const auto tick = m_nextTick;
            const auto off = sampleOfTick (tick + gate() * stepTicks());
            playStep (std::max (now() + 1, off));
            m_nextTick = nextGridLine (t.timeMap.tempoMap(), tick, stepTicks(), swingAmount(), true);
            return;
        }

        const auto length = static_cast<double> (stepLengthSamples());
        playStep (std::max (now() + 1, now() + std::llround (gate() * length)));

        if (m_freeIndex == 1)
        {
            m_freePair += 2.0 * length;
            m_freeIndex = 0;
        }
        else
        {
            m_freeIndex = 1;
        }
    }

private:
    double m_nextTick = -1.0;
    double m_freePair = 0.0;
    long long m_freeIndex = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Arpeggiator (ME10).

constexpr std::array<ParamInfo, 9> arpParams { {
    { "rate", "Rate", 0.0f, static_cast<float> (arpRateCount - 1), 3.0f, ParamKind::choice, "", 0.0f, arpRates },
    { "mode", "Mode", 0.0f, 7.0f, 0.0f, ParamKind::choice, "", 0.0f, arpModes },
    { "octaves", "Octaves", 1.0f, 4.0f, 1.0f, ParamKind::integer },
    { "gate", "Gate", 5.0f, 200.0f, 50.0f, ParamKind::continuous, "%" },
    { "swing", "Swing", 50.0f, 75.0f, 50.0f, ParamKind::continuous, "%" },
    { "velocity", "Velocity", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, velocityChoices },
    { "fixedVelocity", "Fixed velocity", 1.0f, 127.0f, 100.0f, ParamKind::integer },
    { "latch", "Latch", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "retrigger", "Retrigger", 0.0f, 1.0f, 1.0f, ParamKind::choice, "", 0.0f, retriggerChoices },
} };

const NodeDescriptor arpDescriptor { nodetype::arpeggiator,   "Arpeggiator", notesInputs, notesPort, arpParams, true,
                                     NodeCategory::midiEffect };

class ArpeggiatorNode final : public SteppedEffect
{
public:
    ArpeggiatorNode()
        : SteppedEffect (arpDescriptor)
    {
    }

    [[nodiscard]] juce::var meters() const override
    {
        juce::Array<juce::var> keys;

        for (size_t i = 0; i < static_cast<size_t> (m_shownCount.load()); ++i)
            keys.add (m_shown[i].load());

        return makeObject ({ { "keys", keys }, { "step", m_shownStep.load() } });
    }

private:
    enum
    {
        rateParam,
        modeParam,
        octavesParam,
        gateParam,
        swingParam,
        velocityParam,
        fixedVelocityParam,
        latchParam,
        retriggerParam,
    };

    [[nodiscard]] double stepTicks() const noexcept override
    {
        return rateTicks (arpRateFirst, arpRateCount, param (rateParam));
    }
    [[nodiscard]] double swingAmount() const noexcept override { return param (swingParam) / 100.0; }
    [[nodiscard]] double gate() const noexcept override { return param (gateParam) / 100.0; }

    [[nodiscard]] bool hasKeys() noexcept override
    {
        return std::ranges::any_of (sources(), [] (const Source& s) { return s.used && ! s.passed; });
    }

    [[nodiscard]] bool anyHeld() noexcept
    {
        return std::ranges::any_of (sources(), [] (const Source& s) { return s.used && s.held && ! s.passed; });
    }

    void onBlock() noexcept override
    {
        if (transport().started)
            m_random = 0x9e3779b9u;

        // Latch off: what is only latched goes.
        if (param (latchParam) < 0.5f)
            for (auto& source : sources())
                if (source.used && source.latched && ! source.held)
                    removeSource (source);

        SteppedEffect::onBlock();
    }

    void onStart (Source& source) noexcept override
    {
        // With latch on, the first key after all were let go starts a new set.
        const auto wasPlaying = hasKeysExcept (source);

        if (! anyHeldExcept (source))
            for (auto& other : sources())
                if (&other != &source && other.used && other.latched && ! other.held)
                    removeSource (other);

        if (param (retriggerParam) >= 0.5f || ! wasPlaying)
            m_step = 0;

        if (! wasPlaying && ! transport().playing)
            startFreeClock();
    }

    bool onEnd (Source& source, bool choke) noexcept override
    {
        if (choke)
        {
            endOutputsOf (source.id, true);
            return false;
        }

        // The step sounding plays to its gate.
        if (param (latchParam) >= 0.5f)
        {
            source.latched = true;
            return true;
        }

        return false;
    }

    [[nodiscard]] bool hasKeysExcept (const Source& except) noexcept
    {
        return std::ranges::any_of (sources(), [&] (const Source& s) { return &s != &except && s.used && ! s.passed; });
    }

    [[nodiscard]] bool anyHeldExcept (const Source& except) noexcept
    {
        return std::ranges::any_of (sources(),
                                    [&] (const Source& s) { return &s != &except && s.used && s.held && ! s.passed; });
    }

    void playStep (juce::int64 offAt) noexcept override
    {
        // The keys by pitch, or in the order played.
        std::array<Source*, maxSources> keys {};
        size_t count = 0;

        for (auto& source : sources())
            if (source.used && ! source.passed && count < keys.size())
                keys[count++] = &source;

        if (count == 0)
            return;

        const auto mode = static_cast<ArpMode> (std::clamp (static_cast<int> (std::lround (param (modeParam))), 0, 7));
        const auto byOrder = mode == ArpMode::asPlayed;
        std::sort (keys.begin(), keys.begin() + static_cast<std::ptrdiff_t> (count),
                   [byOrder] (const Source* a, const Source* b)
                   {
                       if (byOrder || std::abs (a->note.pitch - b->note.pitch) < 1.0e-9)
                           return a->order < b->order;
                       return a->note.pitch < b->note.pitch;
                   });

        m_random = m_random * 1664525u + 1013904223u;
        const auto octaves = static_cast<int> (std::lround (param (octavesParam)));
        const auto pick = arpPick (mode, static_cast<int> (count), octaves, m_step++, m_random >> 8);
        const auto velocityMode = param (velocityParam);
        const auto fixed = param (fixedVelocityParam);

        const auto play = [&] (Source& key)
        {
            std::ignore = startDerived (key, key.note.pitch + 12.0 * pick.octave,
                                        velocityFor (key, velocityMode, fixed), now(), offAt, 0);
        };

        if (pick.key < 0)
        {
            for (size_t i = 0; i < count; ++i)
                play (*keys[i]);
        }
        else
        {
            play (*keys[static_cast<size_t> (pick.key)]);
        }

        // For the panel: the keys in pattern order and the one playing.
        const auto shown = std::min<size_t> (count, m_shown.size());

        for (size_t i = 0; i < shown; ++i)
            m_shown[i].store (keys[i]->note.pitch);

        m_shownCount.store (static_cast<int> (shown));
        m_shownStep.store (pick.key);
    }

    juce::uint64 m_step = 0;
    juce::uint32 m_random = 0x9e3779b9u;
    std::array<std::atomic<double>, 16> m_shown {};
    std::atomic<int> m_shownCount { 0 };
    std::atomic<int> m_shownStep { -1 };
};

// ---------------------------------------------------------------------------------------------------------
// Note Repeat (ME13).

constexpr std::array<ParamInfo, 5> repeatParams { {
    { "rate", "Rate", 0.0f, static_cast<float> (repeatRateCount - 1), 7.0f, ParamKind::choice, "", 0.0f, repeatRates },
    { "gate", "Gate", 5.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "swing", "Swing", 50.0f, 75.0f, 50.0f, ParamKind::continuous, "%" },
    { "velocity", "Velocity", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, velocityChoices },
    { "fixedVelocity", "Fixed velocity", 1.0f, 127.0f, 100.0f, ParamKind::integer },
} };

const NodeDescriptor repeatDescriptor {
    nodetype::noteRepeat, "Note Repeat", notesInputs, notesPort, repeatParams, true, NodeCategory::midiEffect
};

class NoteRepeatNode final : public SteppedEffect
{
public:
    NoteRepeatNode()
        : SteppedEffect (repeatDescriptor)
    {
    }

private:
    enum
    {
        rateParam,
        gateParam,
        swingParam,
        velocityParam,
        fixedVelocityParam,
    };

    [[nodiscard]] double stepTicks() const noexcept override
    {
        return rateTicks (repeatRateFirst, repeatRateCount, param (rateParam));
    }
    [[nodiscard]] double swingAmount() const noexcept override { return param (swingParam) / 100.0; }
    [[nodiscard]] double gate() const noexcept override { return param (gateParam) / 100.0; }

    [[nodiscard]] bool hasKeys() noexcept override
    {
        return std::ranges::any_of (sources(), [] (const Source& s) { return s.used && ! s.passed; });
    }

    void hit (Source& source, juce::int64 offAt) noexcept
    {
        source.lastHit = now();
        std::ignore =
            startDerived (source, source.note.pitch,
                          velocityFor (source, param (velocityParam), param (fixedVelocityParam)), now(), offAt, 0);
    }

    void onStart (Source& source) noexcept override
    {
        const auto wasPlaying =
            std::ranges::any_of (sources(), [&] (const Source& s) { return &s != &source && s.used && ! s.passed; });

        if (! wasPlaying && ! transport().playing)
            startFreeClock();

        // The first hit is at the note's start; a step on the same sample doesn't hit it again.
        const auto length = stepLengthSamples();
        hit (source, now() + std::max<juce::int64> (1, std::llround (gate() * static_cast<double> (length))));
    }

    bool onEnd (Source& source, bool choke) noexcept override
    {
        // A hit plays to its gate, unless choked.
        if (choke)
            endOutputsOf (source.id, true);

        return false;
    }

    void playStep (juce::int64 offAt) noexcept override
    {
        const auto guard = stepLengthSamples() / 4;

        for (auto& source : sources())
            if (source.used && ! source.passed && now() - source.lastHit > guard)
                hit (source, offAt);
    }
};

// ---------------------------------------------------------------------------------------------------------
// Step Sequencer (docs/design/step-sequencer.md): notes from patterns of steps, inside the track's sequencer clips.
// Notes coming in pass through as themselves.

constexpr std::array<ParamInfo, 0> stepSequencerParams {};

const NodeDescriptor stepSequencerDescriptor {
    nodetype::stepSequencer, "Step Sequencer", notesInputs, notesPort, stepSequencerParams, true,
    NodeCategory::midiEffect
};

class StepSequencerNode final : public MidiEffectNode
{
public:
    StepSequencerNode()
        : MidiEffectNode (stepSequencerDescriptor)
    {
    }

    void configure (const GraphNode& node) override
    {
        MidiEffectNode::configure (node);

        if (node.steps != nullptr && node.steps != m_lastSteps)
        {
            m_lastSteps = node.steps;
            m_steps.publish (node.steps);
        }
    }

    void setStepPreview (int pattern) override { m_preview.store (pattern); }
    void setStepAudition (int pattern) override { m_audition.store (std::max (0, pattern)); }

private:
    void onStart (Source& source) noexcept override { playAsItself (source); }

    // Live notes (negative ids) held while stopped play the shown pattern, as long as one is held (SS8).
    [[nodiscard]] bool liveHeld() noexcept
    {
        return std::ranges::any_of (sources(), [] (const Source& source) { return source.used && source.id < 0; });
    }

    void onBlock() noexcept override
    {
        const auto* steps = m_steps.acquire();
        const auto& t = transport();
        const auto previewed = m_preview.load();
        auto preview = -1;

        if (! t.playing && previewed >= 0)
            preview = previewed;
        else if (! t.playing && liveHeld())
            preview = m_audition.load();

        // Starting, stopping, jumping, looping or a preview starting or stopping: what sounds ends.
        if (t.started || t.stopped || t.jumped || t.looped || preview != m_previewing)
        {
            endAllOutputs (false);
            m_previewStart = blockStart();
        }

        m_previewing = preview;

        if (steps == nullptr)
            return;

        if (t.playing)
        {
            const auto chasing = t.started || t.jumped || t.looped;

            for (const auto& span : context().sequence->sequencerSpans())
            {
                if (static_cast<double> (span.end) <= t.startTick || static_cast<double> (span.start) >= t.endTick)
                    continue;

                // Starting, jumping or looping: steps that began before this block and still sound start now, as a
                // clip's notes are chased. A track processed ahead of the playhead (its own latency) first sees the
                // clip from the latency on, and the step at the clip's start was lost.
                if (chasing)
                    stepseq::forEachStepHit (*steps, span, t.startTick - longestStep, t.startTick,
                                             [&] (const stepseq::Hit& hit)
                                             {
                                                 if (hit.end > t.startTick)
                                                     start (hit, blockStart(), sampleOfTick (hit.end));
                                             });

                stepseq::forEachStepHit (*steps, span, t.startTick, t.endTick, [&] (const stepseq::Hit& hit)
                                         { start (hit, sampleOfTick (hit.start), sampleOfTick (hit.end)); });
            }

            return;
        }

        // The panel's preview (SS8): the pattern loops from where the preview started, at the playhead's tempo.
        if (preview < 0)
            return;

        const auto samplesPerTick = std::max (1.0e-9, t.timeMap.samplesPerTickAt (t.startTick));
        const auto from = static_cast<double> (blockStart() - m_previewStart) / samplesPerTick;
        const auto to = static_cast<double> (blockStart() + context().numSamples - m_previewStart) / samplesPerTick;
        const stepseq::Span span { 0, std::numeric_limits<Ticks>::max() / 2, 0, preview, 0, 0 };
        stepseq::forEachStepHit (*steps, span, from, to,
                                 [&] (const stepseq::Hit& hit)
                                 {
                                     start (hit, m_previewStart + std::llround (hit.start * samplesPerTick),
                                            m_previewStart + std::llround (hit.end * samplesPerTick));
                                 });
    }

    void start (const stepseq::Hit& hit, juce::int64 at, juce::int64 offAt) noexcept
    {
        CompiledNote note;
        note.start = static_cast<Ticks> (std::llround (hit.start));
        note.end = std::max (note.start + 1, static_cast<Ticks> (std::llround (hit.end)));
        note.holdUntil = note.end;
        note.pitch = static_cast<double> (hit.pitch);
        note.velocity = hit.velocity;
        std::ignore = startOwn (note, at, offAt);
    }

    // The longest a step's note can be: 16 steps (a cell's longest) of the slowest rate.
    static constexpr double longestStep = 16.0 * static_cast<double> (stepRates.front().ticks);

    SnapshotExchange<StepSequence> m_steps;
    std::shared_ptr<const StepSequence> m_lastSteps;
    std::atomic<int> m_preview { -1 };
    std::atomic<int> m_audition { 0 };
    int m_previewing = -1;
    juce::int64 m_previewStart = 0;
};

} // namespace

namespace midifx
{

namespace
{
// A rendered note's curve: the source's from the note's start to its end, times made relative to the note.
[[nodiscard]] Curve sliceCurve (std::span<const CurvePoint> points, double shift, double length, double defaultValue)
{
    Curve result;

    if (points.empty())
        return result;

    const auto at = [&] (double time) { return evaluateCurve (points, time + shift, defaultValue); };
    CurvePoint first;
    first.time = 0;
    first.value = at (0.0);

    // The first point takes the shape of the segment it starts in.
    for (const auto& point : points)
        if (static_cast<double> (point.time) <= shift)
            first.shape = point.shape, first.tension = point.tension;

    result.push_back (first);

    for (const auto& point : points)
    {
        const auto time = static_cast<double> (point.time) - shift;

        if (time > 0.0 && time < length)
        {
            auto moved = point;
            moved.time = static_cast<Ticks> (std::llround (time));
            result.push_back (moved);
        }
    }

    if (result.size() > 1 || static_cast<double> (points.back().time) > shift)
    {
        CurvePoint last;
        last.time = static_cast<Ticks> (std::llround (length));
        last.value = at (length);

        if (last.time > result.back().time)
            result.push_back (last);
    }

    return result;
}
} // namespace

RenderedNotes renderMidiEffects (std::span<const GraphNode* const> effects, const NoteClip& clip, const TempoMap& map)
{
    constexpr double rate = 48000.0;
    constexpr int block = 512;

    // The clip alone, so only its notes go in.
    const std::vector<ClipPtr> clips { std::make_shared<const NoteClip> (clip) };
    const TrackSequence sequence (clips);
    const ProcessSpec spec { rate, block };
    auto sequencer = NodeRegistry::builtin().create (nodetype::sequencer);
    sequencer->prepare (spec);
    std::vector<std::unique_ptr<Node>> nodes;

    for (const auto* effect : effects)
    {
        auto node = NodeRegistry::builtin().create (effect->type);

        if (node == nullptr)
            continue;

        node->prepare (spec);
        const auto& infos = node->descriptor().params;

        for (size_t i = 0; i < infos.size(); ++i)
        {
            const auto it = effect->params.find (infos[i].id);
            node->params().set (i,
                                it != effect->params.end() ? static_cast<float> (it->second) : infos[i].defaultValue);
        }

        node->configure (*effect);
        nodes.push_back (std::move (node));
    }

    struct Played
    {
        CompiledNote note;
        double start = 0.0;
        double end = -1.0;
    };

    std::vector<Played> played;
    std::map<ObjectId, size_t> open;
    RenderedNotes result;
    const TimeMap timeMap (map, rate);
    const auto clipStart = static_cast<double> (clip.start);
    const auto clipEnd = static_cast<double> (clip.start + clip.length);
    const auto first = std::llround (timeMap.tickToSample (clipStart));
    const auto last = std::llround (timeMap.tickToSample (clipEnd));
    std::vector<EventBuffer> buffers (nodes.size() + 1);

    for (auto position = first; position < last; position += block)
    {
        const auto n = static_cast<int> (std::min<juce::int64> (block, last - position));
        Transport transport;
        transport.timeMap = timeMap;
        transport.playing = true;
        transport.started = position == first;
        transport.startSample = position;
        transport.startTick = position == first ? clipStart : timeMap.sampleToTick (static_cast<double> (position));
        transport.endTick = timeMap.sampleToTick (static_cast<double> (position + n));

        ProcessContext context;
        context.numSamples = n;
        context.transport = &transport;
        context.sequence = &sequence;
        buffers[0].clear();
        context.eventsOut = buffers.data();
        sequencer->process (context);

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            buffers[i + 1].clear();
            context.eventsIn = &buffers[i];
            context.eventsOut = &buffers[i + 1];
            nodes[i]->process (context);
        }

        for (const auto& event : buffers.back().events())
        {
            const auto tick =
                std::max (clipStart, timeMap.sampleToTick (static_cast<double> (position + event.sampleOffset)));

            switch (event.type)
            {
                case NoteEventType::noteOn:
                case NoteEventType::legato:
                {
                    if (event.note == nullptr)
                        break;

                    if (event.type == NoteEventType::legato)
                    {
                        if (const auto from = open.find (event.fromNoteId); from != open.end())
                        {
                            played[from->second].end = tick;
                            result.links.push_back ({ from->second, played.size(), event.note->glideSeconds * 1000.0 });
                            open.erase (from);
                        }
                    }

                    open[event.noteId] = played.size();
                    played.push_back ({ *event.note, tick });
                    break;
                }

                case NoteEventType::noteOff:
                case NoteEventType::choke:
                    if (const auto it = open.find (event.noteId); it != open.end())
                    {
                        played[it->second].end = tick;
                        open.erase (it);
                    }
                    break;

                case NoteEventType::releaseAll:
                case NoteEventType::chokeAll:
                case NoteEventType::sourceChanged:
                case NoteEventType::expression:
                case NoteEventType::sustain:
                case NoteEventType::controller:
                    break;
            }
        }
    }

    for (const auto& [id, index] : open)
        played[index].end = clipEnd;

    for (const auto& one : played)
    {
        Note note;
        const auto start = std::llround (one.start);
        note.start = start - clip.start;
        note.length = std::max<Ticks> (1, std::llround (std::min (one.end, clipEnd)) - start);
        note.pitch = one.note.pitch;
        note.velocity = one.note.velocity;
        note.releaseVelocity = one.note.releaseVelocity;
        const auto shift = static_cast<double> (start - one.note.start);

        for (size_t lane = 0; lane < numLanes; ++lane)
            note.expression[lane] = sliceCurve (one.note.lanes[lane], shift, static_cast<double> (note.length),
                                                laneInfos()[lane].defaultValue);

        note.vibrato = one.note.vibrato;
        note.vibrato.delay = std::max<Ticks> (0, note.vibrato.delay - (start - one.note.start));
        result.notes.push_back (std::move (note));
    }

    return result;
}

} // namespace midifx

void registerMidiEffects (NodeRegistry& registry)
{
    registry.add (arpDescriptor, [] { return std::make_unique<ArpeggiatorNode>(); });
    registry.add (chordDescriptor, [] { return std::make_unique<ChordNode>(); });
    registry.add (scaleLockDescriptor, [] { return std::make_unique<ScaleLockNode>(); });
    registry.add (repeatDescriptor, [] { return std::make_unique<NoteRepeatNode>(); });
    registry.add (stepSequencerDescriptor, [] { return std::make_unique<StepSequencerNode>(); });
}

} // namespace dawg
