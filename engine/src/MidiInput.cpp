#include "dawg/MidiInput.h"

#include "dawg/VarUtils.h"

#include <algorithm>
#include <cmath>

namespace dawg
{

namespace
{
constexpr int lowerMasterChannel = 1;
constexpr int upperMasterChannel = 16;
constexpr int pitchWheelCentre = 8192;
constexpr int sustainController = 64;
constexpr int timbreController = 74;
constexpr int dataEntryMsb = 6;
constexpr int rpnLsbController = 100;
constexpr int rpnMsbController = 101;
constexpr int rpnPitchBendRange = 0;
constexpr int rpnMpeConfiguration = 6;

// Simplification tolerances per lane (pitch in semitones; pressure and timbre normalised), and for controllers.
constexpr std::array<double, 3> tolerances { 0.03, 0.01, 0.01 };
constexpr double controllerTolerance = 0.01;
constexpr int sustainKey = 64;

[[nodiscard]] float sevenBit (int value) noexcept
{
    return static_cast<float> (value) / 127.0f;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------
// MidiInputDecoder

MidiInputDecoder::MidiInputDecoder (std::function<ObjectId()> newNoteId, MidiInputSettings settings)
    : m_newNoteId (std::move (newNoteId)),
      m_settings (settings)
{
}

bool MidiInputDecoder::isMasterChannel (int channel) const noexcept
{
    return m_settings.mpe && (channel == lowerMasterChannel || channel == upperMasterChannel);
}

float MidiInputDecoder::pitchOf (int channel) const noexcept
{
    const auto& state = m_channels[static_cast<size_t> (channel - 1)];

    if (! m_settings.mpe || isMasterChannel (channel))
        return state.bend;

    // A member channel's bend adds to its zone's master bend.
    const auto master = channel < upperMasterChannel ? lowerMasterChannel : upperMasterChannel;
    return state.bend + m_channels[static_cast<size_t> (master - 1)].bend;
}

void MidiInputDecoder::sendExpression (int channel, Lane lane, float value, double time, const Sink& sink) const
{
    for (const auto id : m_channels[static_cast<size_t> (channel - 1)].notes)
    {
        if (id == 0)
            continue;

        LiveInput input;
        input.noteId = id;
        input.type = NoteEventType::expression;
        input.lane = static_cast<int> (lane);
        input.value = value;
        input.time = time;
        sink (input);
    }
}

bool MidiInputDecoder::process (const juce::MidiMessage& message, double time, const Sink& sink)
{
    const auto channel = message.getChannel();

    if (channel < 1 || channel > 16)
        return false;

    if (message.isNoteOn())
        noteOn (channel, message, time, sink);
    else if (message.isNoteOff())
        noteOff (channel, message, time, sink);
    else if (message.isPitchWheel())
        pitchWheel (channel, message.getPitchWheelValue(), time, sink);
    else if (message.isChannelPressure())
        channelPressure (channel, message.getChannelPressureValue(), time, sink);
    else if (message.isAftertouch())
        polyPressure (channel, message.getNoteNumber(), message.getAfterTouchValue(), time, sink);
    else if (message.isController())
        return handleController (channel, message.getControllerNumber(), message.getControllerValue(), time, sink);

    return false;
}

void MidiInputDecoder::noteOn (int channel, const juce::MidiMessage& message, double time, const Sink& sink)
{
    auto& state = m_channels[static_cast<size_t> (channel - 1)];
    auto& slot = state.notes[static_cast<size_t> (message.getNoteNumber())];

    if (slot != 0)
    {
        // The same key again without a note-off: end the old note first.
        LiveInput off;
        off.noteId = slot;
        off.type = NoteEventType::noteOff;
        off.time = time;
        sink (off);
    }

    LiveInput on;
    on.noteId = m_newNoteId();
    on.pitch = message.getNoteNumber();
    on.velocity = message.getFloatVelocity();
    on.initial = { pitchOf (channel), state.pressure, state.timbre };
    on.time = time;
    slot = on.noteId;
    sink (on);
}

void MidiInputDecoder::noteOff (int channel, const juce::MidiMessage& message, double time, const Sink& sink)
{
    auto& slot = m_channels[static_cast<size_t> (channel - 1)].notes[static_cast<size_t> (message.getNoteNumber())];

    if (slot == 0)
        return;

    LiveInput off;
    off.noteId = slot;
    off.type = NoteEventType::noteOff;
    off.velocity = message.getFloatVelocity();
    off.time = time;
    sink (off);
    slot = 0;
}

void MidiInputDecoder::pitchWheel (int channel, int value, double time, const Sink& sink)
{
    const auto range =
        m_settings.mpe && ! isMasterChannel (channel) ? m_settings.memberBendRange : m_settings.channelBendRange;
    m_channels[static_cast<size_t> (channel - 1)].bend =
        static_cast<float> (value - pitchWheelCentre) / pitchWheelCentre * static_cast<float> (range);

    if (! isMasterChannel (channel))
    {
        sendExpression (channel, Lane::pitch, pitchOf (channel), time, sink);
        return;
    }

    // A master bend moves every note of the zone.
    for (int ch = 1; ch <= 16; ++ch)
        if (! isMasterChannel (ch))
            sendExpression (ch, Lane::pitch, pitchOf (ch), time, sink);
}

void MidiInputDecoder::channelPressure (int channel, int value, double time, const Sink& sink)
{
    auto& state = m_channels[static_cast<size_t> (channel - 1)];
    state.pressure = sevenBit (value);

    if (! isMasterChannel (channel))
    {
        sendExpression (channel, Lane::pressure, state.pressure, time, sink);
        return;
    }

    LiveInput input;
    input.type = NoteEventType::controller;
    input.lane = controllerPressure;
    input.value = state.pressure;
    input.time = time;
    sink (input);
}

void MidiInputDecoder::polyPressure (int channel, int note, int value, double time, const Sink& sink)
{
    const auto id = m_channels[static_cast<size_t> (channel - 1)].notes[static_cast<size_t> (note)];

    if (id == 0)
        return;

    LiveInput input;
    input.noteId = id;
    input.type = NoteEventType::expression;
    input.lane = static_cast<int> (Lane::pressure);
    input.value = sevenBit (value);
    input.time = time;
    sink (input);
}

bool MidiInputDecoder::handleController (int channel, int controller, int value, double time, const Sink& sink)
{
    auto& state = m_channels[static_cast<size_t> (channel - 1)];

    switch (controller)
    {
        case rpnMsbController:
            state.rpnMsb = value;
            return false;

        case rpnLsbController:
            state.rpnLsb = value;
            return false;

        case dataEntryMsb:
        {
            if (state.rpnMsb != 0)
                return false;

            if (state.rpnLsb == rpnMpeConfiguration && (channel == lowerMasterChannel || channel == upperMasterChannel))
            {
                // MPE configuration message: a zone with members switches to MPE, none switches it off.
                const auto previous = m_settings;
                m_settings.mpe = value > 0;
                return ! (previous == m_settings);
            }

            if (state.rpnLsb == rpnPitchBendRange)
            {
                const auto previous = m_settings;
                auto& range = m_settings.mpe && ! isMasterChannel (channel) ? m_settings.memberBendRange
                                                                            : m_settings.channelBendRange;
                range = std::clamp (value, 1, 96);
                return ! (previous == m_settings);
            }

            return false;
        }

        case sustainController:
        {
            const auto down = value >= 64;

            if (down != m_sustain)
            {
                m_sustain = down;
                LiveInput input;
                input.type = NoteEventType::sustain;
                input.value = down ? 1.0f : 0.0f;
                input.time = time;
                sink (input);
            }

            return false;
        }

        case timbreController:
            if (m_settings.mpe && ! isMasterChannel (channel))
            {
                state.timbre = sevenBit (value);
                sendExpression (channel, Lane::timbre, state.timbre, time, sink);
                return false;
            }

            break;

        default:
            break;
    }

    LiveInput input;
    input.type = NoteEventType::controller;
    input.lane = controller;
    input.value = sevenBit (value);
    input.time = time;
    sink (input);
    return false;
}

void MidiInputDecoder::releaseAll (double time, const Sink& sink)
{
    for (auto& state : m_channels)
    {
        for (auto& id : state.notes)
        {
            if (id == 0)
                continue;

            LiveInput off;
            off.noteId = id;
            off.type = NoteEventType::noteOff;
            off.time = time;
            sink (off);
            id = 0;
        }
    }

    if (m_sustain)
    {
        m_sustain = false;
        LiveInput input;
        input.type = NoteEventType::sustain;
        input.time = time;
        sink (input);
    }
}

// ---------------------------------------------------------------------------------------------------------
// NoteRecorder

void NoteRecorder::add (const RecordedInput& event)
{
    const auto& input = event.input;
    m_passes = std::max (m_passes, event.pass + 1);

    switch (input.type)
    {
        case NoteEventType::noteOn:
        {
            Recorded note;
            note.start = event.tick;
            note.pitch = input.pitch;
            note.velocity = input.velocity;
            note.pass = event.pass;

            for (size_t lane = 0; lane < note.points.size(); ++lane)
                note.points[lane].emplace_back (event.tick, static_cast<double> (input.initial[lane]));

            m_open[input.noteId] = m_notes.size();
            m_notes.push_back (std::move (note));
            return;
        }

        case NoteEventType::noteOff:
            if (const auto it = m_open.find (input.noteId); it != m_open.end())
            {
                auto& note = m_notes[it->second];
                note.end = event.tick;
                note.releaseVelocity = input.velocity;
                m_open.erase (it);
            }
            return;

        case NoteEventType::expression:
            if (const auto it = m_open.find (input.noteId);
                it != m_open.end() && input.lane >= 0 && input.lane < static_cast<int> (Recorded {}.points.size()))
                m_notes[it->second].points[static_cast<size_t> (input.lane)].emplace_back (
                    event.tick, static_cast<double> (input.value));
            return;

        case NoteEventType::sustain:
            m_controllers[sustainKey].push_back ({ event.tick, input.value >= 0.5f ? 1.0 : 0.0, event.pass });
            return;

        case NoteEventType::controller:
        {
            // Clip controller keys (docs/design/phase10-automation.md, A2): pitch bend stored 0..1 around 0.5.
            auto value = static_cast<double> (input.value);
            auto key = input.lane;

            if (input.lane == controllerPitchBend)
            {
                key = controllerKeyPitchBend;
                value = (value + 1.0) / 2.0;
            }
            else if (input.lane == controllerPressure)
            {
                key = controllerKeyPressure;
            }

            if (key >= 0 && key <= controllerKeyPressure)
                m_controllers[key].push_back ({ event.tick, std::clamp (value, 0.0, 1.0), event.pass });

            return;
        }

        case NoteEventType::choke:
        case NoteEventType::releaseAll:
        case NoteEventType::chokeAll:
        case NoteEventType::legato:
        case NoteEventType::sourceChanged:
            return;
    }
}

juce::var NoteRecorder::controllers (Ticks clipStart, Ticks end, int pass) const
{
    const auto result = newObject();

    for (const auto& [key, recorded] : m_controllers)
    {
        // Content positions of the new clip; loop passes overlap, so sort and keep the last value per tick.
        std::vector<std::pair<double, double>> points;

        for (const auto& point : recorded)
            if ((pass < 0 || point.pass == pass) && point.tick >= static_cast<double> (clipStart)
                && point.tick < static_cast<double> (end))
                points.emplace_back (std::round (point.tick) - static_cast<double> (clipStart), point.value);

        std::ranges::stable_sort (points, {}, &std::pair<double, double>::first);

        if (points.empty())
            continue;

        // The pedal is a step curve; other controllers are simplified like expression.
        const auto sustain = key == sustainKey;
        const auto kept = sustain ? points : simplify (points, controllerTolerance);
        juce::Array<juce::var> curve;

        for (size_t i = 0; i < kept.size(); ++i)
        {
            const auto& [time, value] = kept[i];
            const auto repeated = i > 0 && sustain && std::abs (kept[i - 1].second - value) < 1.0e-9;
            const auto sameTime = i + 1 < kept.size() && std::abs (kept[i + 1].first - time) < 0.5;

            if (repeated || sameTime || curve.size() >= static_cast<int> (limits::maxCurvePoints))
                continue;

            curve.add (juce::Array<juce::var> { std::llround (time), value, sustain ? 1 : 0 });
        }

        result->setProperty (juce::String (key), curve);
    }

    return juce::var (result.get());
}

std::vector<std::pair<double, double>> NoteRecorder::simplify (const std::vector<std::pair<double, double>>& points,
                                                               double tolerance)
{
    if (points.size() <= 2)
        return points;

    // Iterative Ramer–Douglas–Peucker over index ranges.
    std::vector<bool> keep (points.size(), false);
    keep.front() = true;
    keep.back() = true;
    std::vector<std::pair<size_t, size_t>> ranges { { 0, points.size() - 1 } };

    while (! ranges.empty())
    {
        const auto [first, last] = ranges.back();
        ranges.pop_back();

        const auto [t0, v0] = points[first];
        const auto [t1, v1] = points[last];
        auto worst = 0.0;
        auto worstIndex = first;

        for (auto i = first + 1; i < last; ++i)
        {
            const auto [t, v] = points[i];
            const auto expected = t1 > t0 ? v0 + (v1 - v0) * (t - t0) / (t1 - t0) : v0;
            const auto deviation = std::abs (v - expected);

            if (deviation > worst)
            {
                worst = deviation;
                worstIndex = i;
            }
        }

        if (worst > tolerance)
        {
            keep[worstIndex] = true;
            ranges.emplace_back (first, worstIndex);
            ranges.emplace_back (worstIndex, last);
        }
    }

    std::vector<std::pair<double, double>> result;

    for (size_t i = 0; i < points.size(); ++i)
        if (keep[i])
            result.push_back (points[i]);

    return result;
}

juce::var NoteRecorder::curveVar (const std::vector<std::pair<double, double>>& recorded, double start, double end,
                                  size_t lane)
{
    static constexpr std::array<double, 3> laneDefaults { 0.0, 0.0, 0.5 };

    // Only points within the note (later ones would belong to a wrapped pass), relative to its start.
    std::vector<std::pair<double, double>> points;

    for (const auto& [tick, value] : recorded)
        if (tick >= start && tick <= end)
            points.emplace_back (tick - start, value);

    const auto kept = simplify (points, tolerances[lane]);

    // A lane that stays at its default is left out.
    if (kept.empty() || (kept.size() == 1 && std::abs (kept.front().second - laneDefaults[lane]) < 1.0e-6))
        return {};

    juce::Array<juce::var> curve;
    Ticks previous = -1;

    for (const auto& [time, value] : kept)
    {
        const auto t = std::llround (time);

        if (t == previous || curve.size() >= static_cast<int> (limits::maxCurvePoints))
            continue;

        curve.add (juce::Array<juce::var> { t, value });
        previous = t;
    }

    return curve;
}

juce::var NoteRecorder::notes (Ticks clipStart, Ticks end, Ticks loopEnd, int pass) const
{
    static constexpr std::array<const char*, 3> laneIds { "pitch", "pressure", "timbre" };
    juce::Array<juce::var> result;

    for (const auto& note : m_notes)
    {
        const auto start = std::llround (note.start);

        if (start < clipStart || start >= end || (pass >= 0 && note.pass != pass))
            continue;

        // Held notes end where recording stopped; a note held across a loop wrap ends at the loop end.
        auto noteEnd = note.end < 0.0 ? static_cast<double> (end) : note.end;

        if (noteEnd < note.start)
            noteEnd = static_cast<double> (loopEnd > 0 ? loopEnd : end);

        const auto expression = newObject();

        for (size_t lane = 0; lane < note.points.size(); ++lane)
            if (const auto curve = curveVar (note.points[lane], note.start, noteEnd, lane); curve.isArray())
                expression->setProperty (laneIds[lane], curve);

        auto fields = makeObject ({ { "start", start - clipStart },
                                    { "length", std::max<Ticks> (1, std::llround (noteEnd) - start) },
                                    { "pitch", note.pitch },
                                    { "velocity", note.velocity },
                                    { "releaseVelocity", note.releaseVelocity } });

        if (! expression->getProperties().isEmpty())
            fields.getDynamicObject()->setProperty ("expression", juce::var (expression.get()));

        result.add (fields);
    }

    return result;
}

} // namespace dawg
