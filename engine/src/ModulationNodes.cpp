#include "dawg/BuiltinNodes.h"
#include "dawg/EnvelopeHandle.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Realtime.h"
#include "dawg/VoiceAllocator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

// Modulation sources (docs/design/modulation.md): a MIDI/audio-triggered ADSR, a free or tempo-synced LFO with an
// editable one-cycle shape, and Note Expression (docs/design/phase12-voice-scope.md, V2). All are monophonic per
// track; the voice scope runs them per note.

namespace dawg
{

namespace
{
constexpr double triggerThreshold = 0.01;
constexpr float minRateMs = 1.0f;
constexpr double msPerSecond = 1000.0;
constexpr float toggleThreshold = 0.5f;
constexpr size_t maxHeldNotes = 128;

constexpr std::array<const char*, 3> lfoZeroChoices { "0%", "50%", "100%" };

constexpr float quarterNoteCycle = 13.0f; // "1/4", the default

constexpr std::array<ParamInfo, 6> lfoParams { {
    { "sync", "Tempo sync", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "rateMs", "Rate", minRateMs, 10000.0f, 500.0f, ParamKind::continuous, "ms", 500.0f },
    { "cycle", "Cycle", 0.0f, static_cast<float> (lfoCycleChoices.size() - 1), quarterNoteCycle, ParamKind::choice, "",
      0.0f, lfoCycleChoices },
    { "retrigger", "Retrigger", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "phase", "Phase", 0.0f, 1.0f, 0.0f, ParamKind::continuous, "cycles" },
    { "zero", "Zero", 0.0f, 2.0f, 1.0f, ParamKind::choice, "", 0.0f, lfoZeroChoices },
} };

enum LfoParam : size_t
{
    syncParam,
    rateParam,
    cycleParam,
    retriggerParam,
    phaseParam,
    zeroParam,
};

const NodeDescriptor lfoDescriptor { nodetype::lfo, "LFO", modulationSourceInputs, modulationOutput,
                                     lfoParams,     true,  NodeCategory::utility };

// Attack, decay and release are each a free curve segment (CurveShape::free): a cubic Bezier whose handles point at
// its tension point, which sits anywhere in the rectangle between the segment's ends. The handle's time and level are
// percentages across that rectangle; 50 % and 50 % is a straight line.
constexpr std::array<ParamInfo, 10> adsrParams { {
    { "attackMs", "Attack", 0.0f, 10000.0f, 5.0f, ParamKind::continuous, "ms", 100.0f },
    { "decayMs", "Decay", 0.0f, 10000.0f, 100.0f, ParamKind::continuous, "ms", 500.0f },
    { "sustain", "Sustain", 0.0f, 100.0f, 70.0f, ParamKind::continuous, "%" },
    { "releaseMs", "Release", 0.0f, 10000.0f, 200.0f, ParamKind::continuous, "ms", 500.0f },
    { "attackHandleTime", "Attack handle time", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "attackHandleLevel", "Attack handle level", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "decayHandleTime", "Decay handle time", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "decayHandleLevel", "Decay handle level", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "releaseHandleTime", "Release handle time", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "releaseHandleLevel", "Release handle level", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
} };

enum AdsrParam : size_t
{
    attackParam,
    decayParam,
    sustainParam,
    releaseParam,
    attackHandleTimeParam,
    attackHandleLevelParam,
    decayHandleTimeParam,
    decayHandleLevelParam,
    releaseHandleTimeParam,
    releaseHandleLevelParam,
};

// An ADSR segment's handle (dawg/EnvelopeHandle.h).
using AdsrHandle = EnvelopeHandle;

[[nodiscard]] AdsrHandle adsrHandle (const ParamBlock& params, size_t timeParam, size_t levelParam) noexcept
{
    return { std::clamp (params.get (timeParam) / 100.0f, 0.0f, 1.0f),
             std::clamp (params.get (levelParam) / 100.0f, 0.0f, 1.0f) };
}

const NodeDescriptor adsrDescriptor { nodetype::adsr, "ADSR", modulationSourceInputs, modulationOutput,
                                      adsrParams,     true,   NodeCategory::utility };

constexpr std::array<PortInfo, 1> noteExpressionInputs { { { PortType::events, "Notes" } } };

constexpr std::array<PortInfo, 5> noteExpressionOutputs { {
    { PortType::modulation, "Pitch" },
    { PortType::modulation, "Pressure" },
    { PortType::modulation, "Timbre" },
    { PortType::modulation, "Velocity" },
    { PortType::modulation, "Gate" },
} };

const NodeDescriptor noteExpressionDescriptor {
    nodetype::noteExpression, "Note Expression", noteExpressionInputs, noteExpressionOutputs, {}, true,
    NodeCategory::utility
};

[[nodiscard]] float triggerLevel (const AudioInput& input, int sample) noexcept
{
    const auto left = input[0] != nullptr ? std::abs (input[0][sample]) : 0.0f;
    const auto right = input[1] != nullptr ? std::abs (input[1][sample]) : 0.0f;
    return std::max (left, right);
}

// The LFO's output for a position in its cycle: the shape (the Sine preset when it has none), mapped by the zero
// position.
// The zero position maps the shape's -1..1 travel onto the selected range:
//   0: 0 at the top, -1 at the bottom;
//   1: -1..+1 around the middle (the usual bipolar LFO);
//   2: 0 at the bottom, +1 at the top.
[[nodiscard]] float lfoValue (const Curve* shape, double cycle, int zero) noexcept
{
    const auto time = cycle * static_cast<double> (modulationShapeTicks);
    const auto raw = shape != nullptr && ! shape->empty() ? evaluateCurve (*shape, time, 0.0)
                                                          : evaluateCurve (lfoSineShape, time, 0.0);
    const auto value = std::clamp (raw, -1.0, 1.0);

    if (zero == 0)
        return static_cast<float> ((value - 1.0) * 0.5);

    if (zero == 2)
        return static_cast<float> ((value + 1.0) * 0.5);

    return static_cast<float> (value);
}

// The level change per sample that covers `distance` in `ms` (a whole step at once for no time).
[[nodiscard]] float stepPerSample (float distance, float ms, double sampleRate) noexcept
{
    constexpr float secondsPerMs = 0.001f;
    return ms > 0.0f ? distance / (ms * secondsPerMs * static_cast<float> (sampleRate)) : 1.0f;
}

// A source's note gate, counting held notes so overlapping notes release only when the last one ends. The pedal keeps it open for notes
// it catches, but doesn't open it: pressing it over a note already let go would start that note's envelope again.
class NoteGate
{
public:
    void handle (const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case NoteEventType::noteOn:
                if (m_count < m_notes.size())
                    m_notes[m_count++] = event.noteId;
                m_open = true;
                break;

            case NoteEventType::legato:
                // The note goes on as another: nothing retriggers.
                for (size_t i = 0; i < m_count; ++i)
                    if (m_notes[i] == event.fromNoteId)
                        m_notes[i] = event.noteId;
                break;

            case NoteEventType::noteOff:
            case NoteEventType::choke:
                for (size_t i = 0; i < m_count; ++i)
                    if (m_notes[i] == event.noteId)
                    {
                        m_notes[i] = m_notes[--m_count];
                        break;
                    }
                closeIfDone();
                break;

            case NoteEventType::releaseAll:
            case NoteEventType::chokeAll:
            case NoteEventType::sourceChanged:
                m_count = 0;
                if (event.type == NoteEventType::sourceChanged)
                    m_sustain = false;
                closeIfDone();
                break;

            case NoteEventType::sustain:
                m_sustain = event.value >= toggleThreshold;
                closeIfDone();
                break;

            case NoteEventType::expression:
            case NoteEventType::controller:
                break;
        }
    }

    [[nodiscard]] bool held() const noexcept { return m_open; }

    [[nodiscard]] bool sustain() const noexcept { return m_sustain; }

    void reset() noexcept
    {
        m_count = 0;
        m_sustain = false;
        m_open = false;
    }

private:
    void closeIfDone() noexcept
    {
        if (m_count == 0 && ! m_sustain)
            m_open = false;
    }

    std::array<ObjectId, maxHeldNotes> m_notes {};
    size_t m_count = 0;
    bool m_sustain = false;
    bool m_open = false;
};

class LfoNode final : public Node
{
public:
    LfoNode()
        : Node (lfoDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        reset();
    }

    void reset() noexcept override
    {
        m_started = false;
        m_triggered = false;
        m_gate.reset();
    }

    void configure (const GraphNode& node) override
    {
        m_bypassed.store (node.bypassed);

        // An empty shape is published too: it means the default sine. The exchange keeps every curve alive on this
        // thread, so the audio thread never releases one.
        m_shape.publish (std::make_shared<const Curve> (node.modulationShape));
    }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];

        if (m_bypassed.load() || context.transport == nullptr)
        {
            for (size_t ch = 0; ch < 2; ++ch)
                std::fill_n (out[ch], context.numSamples, 0.0f);

            return;
        }

        const auto& transport = *context.transport;
        const auto* shape = m_shape.acquire();
        const auto sync = params().get (syncParam) >= toggleThreshold;
        const auto rateMs = std::max (minRateMs, params().get (rateParam));
        const auto cycleIndex =
            std::clamp (static_cast<size_t> (std::max (0L, std::lround (params().get (cycleParam)))), size_t { 0 },
                        lfoCycleBeats.size() - 1);
        const auto syncBeats = lfoCycleBeats[cycleIndex];
        const auto retrigger = params().get (retriggerParam) >= toggleThreshold;
        const auto phaseOffset = static_cast<double> (params().get (phaseParam));
        const auto zero = static_cast<int> (std::lround (params().get (zeroParam)));

        const auto* triggerInput = context.audioInputs.size() > 1 ? &context.audioInputs[1] : nullptr;
        const auto events = context.eventsIn != nullptr ? context.eventsIn->events() : std::span<const NoteEvent>();
        size_t nextEvent = 0;
        const auto ticksPerSample =
            context.numSamples > 0 ? (transport.endTick - transport.startTick) / context.numSamples : 0.0;
        const auto freeIncrement = m_sampleRate > 0.0 ? 1.0 / (m_sampleRate * rateMs / msPerSecond) : 0.0;
        const auto syncIncrement = ticksPerSample / static_cast<double> (ticksPerBeat) / syncBeats;

        // The compiler applies the parameters after prepare(), so the start phase is read on the first block.
        if (! m_started)
        {
            m_phase = std::fmod (phaseOffset, 1.0);
            m_started = true;
        }

        auto phase = m_phase;

        for (int s = 0; s < context.numSamples; ++s)
        {
            phase = takeEvents (events, nextEvent, s, retrigger ? phaseOffset : phase, phase);

            const auto trigger = triggerInput != nullptr ? triggerLevel (*triggerInput, s) : 0.0f;
            const auto audioTrigger = trigger > triggerThreshold;

            if (audioTrigger && ! m_triggered)
                phase = phaseOffset;

            m_triggered = audioTrigger;

            // A tempo-synced LFO without retriggering follows the timeline. MIDI or audio retriggering, and free
            // running, advance from the phase instead.
            const auto timeline = sync && ! retrigger && ! audioTrigger;
            const auto beats = (transport.startTick + ticksPerSample * s) / static_cast<double> (ticksPerBeat);
            const auto cycle = std::fmod (timeline ? phaseOffset + beats / syncBeats : phase, 1.0);
            const auto value = lfoValue (shape, cycle, zero);

            out[0][s] = value;
            out[1][s] = value;

            if (! timeline)
                phase = std::fmod (phase + (sync ? syncIncrement : freeIncrement), 1.0);
        }

        m_phase = phase;
    }

private:
    // Hands the events at sample `s` to the gate. A note-on moves the phase to `onNote` (the start phase when
    // retriggering, else the phase as it is).
    [[nodiscard]] double takeEvents (std::span<const NoteEvent> events, size_t& next, int s, double onNote,
                                     double phase) noexcept
    {
        for (; next < events.size() && events[next].sampleOffset <= s; ++next)
        {
            if (events[next].type == NoteEventType::noteOn)
                phase = onNote;

            m_gate.handle (events[next]);
        }

        return phase;
    }

    double m_sampleRate = 48000.0;
    double m_phase = 0.0;
    bool m_started = false; // whether m_phase has been set from the phase parameter
    bool m_triggered = false;
    NoteGate m_gate;
    std::atomic<bool> m_bypassed { false };
    SnapshotExchange<Curve> m_shape;
};

class AdsrNode final : public Node
{
public:
    AdsrNode()
        : Node (adsrDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        reset();
    }

    void reset() noexcept override
    {
        m_stage = Stage::idle;
        m_level = 0.0f;
        m_progress = 0.0f;
        m_shapeHint = 0.0;
        m_releaseFrom = 0.0f;
        m_gate.reset();
        m_gateOn = false;
        m_triggered = false;
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];

        if (m_bypassed.load() || context.transport == nullptr)
        {
            for (size_t ch = 0; ch < 2; ++ch)
                std::fill_n (out[ch], context.numSamples, 0.0f);

            return;
        }

        const auto attackMs = std::max (0.0f, params().get (attackParam));
        const auto decayMs = std::max (0.0f, params().get (decayParam));
        const auto sustain = std::clamp (params().get (sustainParam) / 100.0f, 0.0f, 1.0f); // a percentage
        const auto releaseMs = std::max (0.0f, params().get (releaseParam));
        const Steps steps { stepPerSample (1.0f, attackMs, m_sampleRate),
                            stepPerSample (1.0f, decayMs, m_sampleRate),
                            stepPerSample (1.0f, releaseMs, m_sampleRate),
                            sustain,
                            adsrHandle (params(), attackHandleTimeParam, attackHandleLevelParam),
                            adsrHandle (params(), decayHandleTimeParam, decayHandleLevelParam),
                            adsrHandle (params(), releaseHandleTimeParam, releaseHandleLevelParam) };

        const auto* triggerInput = context.audioInputs.size() > 1 ? &context.audioInputs[1] : nullptr;
        const auto events = context.eventsIn != nullptr ? context.eventsIn->events() : std::span<const NoteEvent>();
        size_t nextEvent = 0;

        for (int s = 0; s < context.numSamples; ++s)
        {
            while (nextEvent < events.size() && events[nextEvent].sampleOffset <= s)
                m_gate.handle (events[nextEvent++]);

            const auto trigger = triggerInput != nullptr ? triggerLevel (*triggerInput, s) : 0.0f;
            const auto audioGate = trigger > triggerThreshold;

            // An audio trigger starts the envelope, and holds it while the trigger stays above the threshold.
            if (audioGate && ! m_triggered && m_stage == Stage::idle)
                startAttack (steps);

            m_triggered = audioGate;
            setGate (m_gate.held() || audioGate, steps);
            advance (steps);

            out[0][s] = m_level;
            out[1][s] = m_level;
        }
    }

private:
    // Per-sample progress through each segment for the current parameters, the sustain level, and each segment's
    // handle.
    struct Steps
    {
        float attack = 1.0f;
        float decay = 1.0f;
        float release = 1.0f;
        float sustain = 1.0f;
        AdsrHandle attackHandle;
        AdsrHandle decayHandle;
        AdsrHandle releaseHandle;
    };

    // The attack starts where its curve passes the current level, so a retrigger rises from there without a jump.
    void startAttack (const Steps& steps) noexcept
    {
        m_stage = Stage::attack;
        m_progress = steps.attackHandle.progressFor (std::clamp (m_level, 0.0f, 1.0f));
    }

    void setGate (bool gate, const Steps& steps) noexcept
    {
        if (gate && ! m_gateOn)
        {
            startAttack (steps);
        }
        else if (! gate && m_gateOn)
        {
            m_releaseFrom = m_level;
            m_progress = 0.0f;
            m_stage = Stage::release;
        }

        m_gateOn = gate;
    }

    void advance (const Steps& steps) noexcept
    {
        switch (m_stage)
        {
            case Stage::idle:
                m_level = 0.0f;
                break;

            case Stage::attack:
                m_progress += steps.attack;

                if (m_progress >= 1.0f)
                {
                    m_level = 1.0f;
                    m_progress = 0.0f;
                    m_stage = Stage::decay;
                }
                else
                {
                    m_level = steps.attackHandle.shape (m_progress, m_shapeHint);
                }
                break;

            case Stage::decay:
                m_progress += steps.decay;

                if (m_progress >= 1.0f)
                {
                    m_level = steps.sustain;
                    m_stage = Stage::sustain;
                }
                else
                {
                    m_level = 1.0f - (1.0f - steps.sustain) * steps.decayHandle.shape (m_progress, m_shapeHint);
                }
                break;

            case Stage::sustain:
                m_level = steps.sustain;
                break;

            case Stage::release:
                m_progress += steps.release;

                if (m_progress >= 1.0f)
                {
                    m_level = 0.0f;
                    m_stage = Stage::idle;
                }
                else
                {
                    m_level = m_releaseFrom * (1.0f - steps.releaseHandle.shape (m_progress, m_shapeHint));
                }
                break;
        }
    }

    enum class Stage
    {
        idle,
        attack,
        decay,
        sustain,
        release,
    };

    double m_sampleRate = 48000.0;
    Stage m_stage = Stage::idle;
    float m_level = 0.0f;
    float m_progress = 0.0f;  // through the current segment, 0..1
    double m_shapeHint = 0.0; // the curved segment's last Bezier solution (EnvelopeHandle::shape())
    float m_releaseFrom = 0.0f;
    bool m_gateOn = false;
    bool m_triggered = false;
    NoteGate m_gate;
    std::atomic<bool> m_bypassed { false };
};
// The expression of the most recent held note as modulation signals (docs/design/phase12-voice-scope.md, V2). Notes
// are tracked by a VoiceAllocator, so sustain, live expression and edits work as in the synth; when the newest note
// ends, an older held one takes over, and when none is held the values stay where they were with the gate at 0.
class NoteExpressionNode final : public Node
{
public:
    NoteExpressionNode()
        : Node (noteExpressionDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override { reset(); }

    void reset() noexcept override
    {
        m_notes.clear();
        m_current.reset();
        m_expression = {};
        m_velocity = 0.0f;
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    void process (const ProcessContext& context) noexcept override
    {
        if (m_bypassed.load() || context.transport == nullptr)
        {
            for (const auto& out : context.audioOutputs)
                for (auto* channel : out)
                    std::fill_n (channel, context.numSamples, 0.0f);

            return;
        }

        if (context.sequence != nullptr)
            m_notes.adoptSequence (*context.sequence);

        int position = 0;

        if (context.eventsIn != nullptr)
        {
            for (const auto& event : context.eventsIn->events())
            {
                render (context, position, event.sampleOffset);
                handle (context, event);
                position = event.sampleOffset;
            }
        }

        render (context, position, context.numSamples);
    }

private:
    static constexpr int controlInterval = 32; // samples between expression evaluations, as in the synth

    [[nodiscard]] static double tickAt (const ProcessContext& context, int sample) noexcept
    {
        const auto& transport = *context.transport;
        return transport.startTick
               + (transport.endTick - transport.startTick) * static_cast<double> (sample) / context.numSamples;
    }

    void handle (const ProcessContext& context, const NoteEvent& event) noexcept
    {
        std::ignore = m_notes.handle (event);

        // Only held notes count: released and choked ones end at once.
        const auto voices = m_notes.voices();

        for (size_t i = 0; i < voices.size(); ++i)
            if (voices[i].state == VoiceState::releasing || voices[i].state == VoiceState::choking)
                m_notes.free (i);

        std::optional<size_t> newest;

        for (size_t i = 0; i < voices.size(); ++i)
            if (voices[i].state == VoiceState::held && (! newest.has_value() || voices[i].age > voices[*newest].age))
                newest = i;

        // A different note (or live expression on this one) jumps to its values at once.
        if (newest.has_value())
        {
            m_expression = m_notes.updateExpression (*newest, tickAt (context, event.sampleOffset), *context.transport);
            m_velocity = voices[*newest].velocity;
        }

        m_current = newest;
    }

    // Writes [from, to): the expression interpolated linearly in steps of controlInterval, velocity and gate held.
    void render (const ProcessContext& context, int from, int to) noexcept
    {
        const auto& outputs = context.audioOutputs;
        const auto velocity = m_velocity;
        const auto gate = m_current.has_value() ? 1.0f : 0.0f;

        for (int start = from; start < to; start += controlInterval)
        {
            const auto end = std::min (to, start + controlInterval);
            const auto first = m_expression;

            if (m_current.has_value())
                m_expression = m_notes.updateExpression (*m_current, tickAt (context, end), *context.transport);

            const auto length = static_cast<double> (end - start);

            for (int s = start; s < end; ++s)
            {
                const auto x = static_cast<double> (s - start + 1) / length;
                const std::array values {
                    static_cast<float> (pitchSignal (first.pitch + (m_expression.pitch - first.pitch) * x)),
                    static_cast<float> (first.pressure + (m_expression.pressure - first.pressure) * x),
                    static_cast<float> (first.timbre + (m_expression.timbre - first.timbre) * x),
                    velocity,
                    gate,
                };

                for (size_t output = 0; output < values.size() && output < outputs.size(); ++output)
                {
                    outputs[output][0][s] = values[output];
                    outputs[output][1][s] = values[output];
                }
            }
        }
    }

    VoiceAllocator m_notes;
    std::optional<size_t> m_current; // the newest held note's voice
    VoiceExpression m_expression;
    float m_velocity = 0.0f;
    std::atomic<bool> m_bypassed { false };
};

// The Macros device (docs/design/macros-and-modulation-column.md, MC5, MC6): eight knobs, each with a neutral position,
// each sending its own modulation output.
constexpr std::array<const char*, 3> macroNeutralChoices { "Bottom", "Centre", "Top" };

constexpr auto macroParams = []
{
    constexpr std::array<const char*, maxMacros> ids { "macro1", "macro2", "macro3", "macro4",
                                                       "macro5", "macro6", "macro7", "macro8" };
    constexpr std::array<const char*, maxMacros> names { "Macro 1", "Macro 2", "Macro 3", "Macro 4",
                                                         "Macro 5", "Macro 6", "Macro 7", "Macro 8" };
    constexpr std::array<const char*, maxMacros> neutralIds { "neutral1", "neutral2", "neutral3", "neutral4",
                                                              "neutral5", "neutral6", "neutral7", "neutral8" };
    constexpr std::array<const char*, maxMacros> neutralNames { "Macro 1 neutral", "Macro 2 neutral", "Macro 3 neutral",
                                                                "Macro 4 neutral", "Macro 5 neutral", "Macro 6 neutral",
                                                                "Macro 7 neutral", "Macro 8 neutral" };
    std::array<ParamInfo, static_cast<size_t> (maxMacros) * 2> params {};

    for (size_t i = 0; i < maxMacros; ++i)
    {
        // The knobs are what macros modulate with; they take no modulation themselves.
        params[i] = {
            ids[i], names[i], 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%", 0.0f, {}, ModulationPolarity::bipolar,
            false
        };
        params[maxMacros + i] = { neutralIds[i], neutralNames[i],    0.0f, 2.0f, 0.0f, ParamKind::choice, "",
                                  0.0f,          macroNeutralChoices };
    }

    return params;
}();

constexpr std::array<PortInfo, maxMacros> macroOutputs { {
    { PortType::modulation, "Macro 1" },
    { PortType::modulation, "Macro 2" },
    { PortType::modulation, "Macro 3" },
    { PortType::modulation, "Macro 4" },
    { PortType::modulation, "Macro 5" },
    { PortType::modulation, "Macro 6" },
    { PortType::modulation, "Macro 7" },
    { PortType::modulation, "Macro 8" },
} };

const NodeDescriptor macrosDescriptor { nodetype::macros,     "Macros", {}, macroOutputs, macroParams, true,
                                        NodeCategory::utility };

// What a knob sends for its value (0..1) and neutral position (MC6): 0 at the neutral position, ±1 turned all the way
// from it.
[[nodiscard]] float macroSignal (float value, int neutral) noexcept
{
    if (neutral == 1)
        return value * 2.0f - 1.0f;

    return neutral == 2 ? value - 1.0f : value;
}

class MacrosNode final : public Node
{
public:
    MacrosNode()
        : Node (macrosDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override { reset(); }

    void reset() noexcept override { m_started = false; }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    // Follows another instance's knobs from now on (followMacros). The one being replaced is kept, as the audio thread
    // may still be reading it.
    void follow (std::shared_ptr<Node> leader)
    {
        if (leader.get() == m_leader.get())
            return;

        m_leaderParams.store (leader != nullptr ? &leader->params() : nullptr);

        if (m_leader != nullptr)
            m_retired.push_back (std::move (m_leader));

        m_leader = std::move (leader);
    }

    void process (const ProcessContext& context) noexcept override
    {
        const auto* leader = m_leaderParams.load();
        const auto& knobs = leader != nullptr ? *leader : params();
        const auto bypassed = m_bypassed.load();
        const auto length = static_cast<float> (std::max (1, context.numSamples));

        for (size_t k = 0; k < maxMacros && k < context.audioOutputs.size(); ++k)
        {
            const auto value = std::clamp (knobs.get (k) / 100.0f, 0.0f, 1.0f);
            const auto neutral = static_cast<int> (std::lround (knobs.get (maxMacros + k)));
            const auto target = bypassed ? 0.0f : macroSignal (value, neutral);
            // A knob moved since the last block ramps across this one; a new voice or a fresh start jumps.
            const auto from = m_started ? m_last[k] : target;

            for (auto* channel : context.audioOutputs[k])
                if (channel != nullptr)
                    for (int s = 0; s < context.numSamples; ++s)
                        channel[s] = from + (target - from) * static_cast<float> (s + 1) / length;

            m_last[k] = target;
        }

        m_started = true;
    }

private:
    std::array<float, maxMacros> m_last {};
    bool m_started = false;
    std::atomic<bool> m_bypassed { false };
    std::atomic<const ParamBlock*> m_leaderParams { nullptr };
    std::shared_ptr<Node> m_leader;
    std::vector<std::shared_ptr<Node>> m_retired;
};
} // namespace

void followMacros (Node& follower, std::shared_ptr<Node> leader)
{
    auto* macros = dynamic_cast<MacrosNode*> (&follower);

    if (macros != nullptr && dynamic_cast<MacrosNode*> (leader.get()) != nullptr && leader.get() != &follower)
        macros->follow (std::move (leader));
}

void registerModulationNodes (NodeRegistry& registry)
{
    registry.add (lfoDescriptor, [] { return std::make_unique<LfoNode>(); });
    registry.add (adsrDescriptor, [] { return std::make_unique<AdsrNode>(); });
    registry.add (noteExpressionDescriptor, [] { return std::make_unique<NoteExpressionNode>(); });
    registry.add (macrosDescriptor, [] { return std::make_unique<MacrosNode>(); });
}

} // namespace dawg
