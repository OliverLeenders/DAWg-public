#include "dawg/MidiFileIO.h"

#include "dawg/Commands.h"
#include "dawg/MidiInput.h"
#include "dawg/Sequence.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

// MIDI files (docs/design/sampler-midi-delay-limiter.md, section 3). Reading goes through the decoder and recorder that
// live input uses, so a file's notes end up exactly like recorded ones.

namespace dawg
{

namespace
{
constexpr int defaultBpm = 120;
constexpr int drumChannel = 10;
constexpr int fileTicksPerBeat = 960;
constexpr int rpnLsbController = 100;
constexpr int rpnMsbController = 101;
constexpr int dataEntryController = 6;
constexpr int timbreController = 74;
constexpr int mpeConfigurationRpn = 6;
constexpr int masterChannel = 1;
constexpr int firstMemberChannel = 2;
constexpr int lastMemberChannel = 16;
constexpr int memberBendRange = 48;
constexpr int memberChannels = 15;
constexpr int pitchWheelCentre = 8192;
constexpr int pitchWheelMax = 16383;
constexpr Ticks controlStepTicks = ticksPerBeat / 64; // how finely curves are written

[[nodiscard]] bool validDenominator (int denominator) noexcept
{
    return denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16;
}

// Whether any channel 1 or 16 sends an MPE configuration message (RPN 6) with members.
[[nodiscard]] bool isMpeFile (const juce::MidiFile& file)
{
    for (int t = 0; t < file.getNumTracks(); ++t)
    {
        std::array<std::pair<int, int>, 16> rpn;
        rpn.fill ({ 127, 127 });

        for (const auto* holder : *file.getTrack (t))
        {
            const auto& message = holder->message;

            if (! message.isController())
                continue;

            auto& [msb, lsb] = rpn[static_cast<size_t> (message.getChannel() - 1)];
            const auto controller = message.getControllerNumber();
            const auto value = message.getControllerValue();

            if (controller == rpnMsbController)
                msb = value;
            else if (controller == rpnLsbController)
                lsb = value;
            else if (controller == dataEntryController && msb == 0 && lsb == mpeConfigurationRpn && value > 0)
                return true;
        }
    }

    return false;
}

// The tempo changes in ticks, the first at 0 (a file without one is 120 bpm).
[[nodiscard]] juce::var tempoOf (const juce::MidiFile& file, double scale)
{
    juce::MidiMessageSequence events;
    file.findAllTempoEvents (events);
    std::map<Ticks, double> tempo;

    for (const auto* holder : events)
        tempo[std::llround (holder->message.getTimeStamp() * scale)] =
            60.0 / holder->message.getTempoSecondsPerQuarterNote();

    tempo.try_emplace (0, static_cast<double> (defaultBpm));
    juce::Array<juce::var> result;

    for (const auto& [tick, bpm] : tempo)
        result.add (makeObject ({ { "tick", tick }, { "bpm", bpm } }));

    return result;
}

// The time signatures by bar, the first on bar 0. Signatures DAWg has no grid for (x/1, x/32) are left out.
[[nodiscard]] juce::var signaturesOf (const juce::MidiFile& file, double scale)
{
    juce::MidiMessageSequence events;
    file.findAllTimeSigEvents (events);
    juce::Array<juce::var> result;
    auto numerator = 4;
    auto denominator = 4;
    Ticks barStart = 0;
    auto bar = 0;

    const auto add = [&result] (int atBar, int top, int bottom)
    {
        if (auto* last = result.isEmpty() ? nullptr : &result.getReference (result.size() - 1);
            last != nullptr && static_cast<int> ((*last)["bar"]) == atBar)
            *last = makeObject ({ { "bar", atBar }, { "numerator", top }, { "denominator", bottom } });
        else
            result.add (makeObject ({ { "bar", atBar }, { "numerator", top }, { "denominator", bottom } }));
    };

    add (0, numerator, denominator);

    for (const auto* holder : events)
    {
        int top = 4;
        int bottom = 4;
        holder->message.getTimeSignatureInfo (top, bottom);

        if (top < 1 || ! validDenominator (bottom))
            continue;

        // Whole bars of the signature so far up to this event, or the next bar line if it lies inside one.
        const auto tick = std::llround (holder->message.getTimeStamp() * scale);
        const auto barLength = ticksPerBeat * 4 * numerator / denominator;
        const auto bars = (tick - barStart + barLength - 1) / barLength;
        bar += static_cast<int> (bars);
        barStart += bars * barLength;
        numerator = top;
        denominator = bottom;
        add (bar, top, bottom);
    }

    return result;
}

// The tick at the end of the bar holding `tick`, under the file's signatures.
[[nodiscard]] Ticks barCeiling (const juce::var& signatures, Ticks tick)
{
    Ticks start = 0;
    const auto& list = *signatures.getArray();

    for (int i = 0; i < list.size(); ++i)
    {
        const auto barLength =
            ticksPerBeat * 4 * static_cast<int> (list[i]["numerator"]) / static_cast<int> (list[i]["denominator"]);
        const auto last = i + 1 == list.size();
        const auto bars =
            last ? std::numeric_limits<Ticks>::max() / barLength
                 : static_cast<Ticks> (static_cast<int> (list[i + 1]["bar"]) - static_cast<int> (list[i]["bar"]));
        const auto end = last ? std::numeric_limits<Ticks>::max() : start + bars * barLength;

        if (tick <= end)
            return start + std::max<Ticks> (1, (tick - start + barLength - 1) / barLength) * barLength;

        start = end;
    }

    return tick;
}

// One part's messages through the decoder and recorder. Returns null if it plays no note.
[[nodiscard]] juce::var partOf (const std::vector<const juce::MidiMessage*>& messages, double scale, bool mpe,
                                const juce::String& name, bool drums, const juce::var& signatures)
{
    ObjectId nextId = 1;
    MidiInputSettings settings;
    settings.mpe = mpe;
    MidiInputDecoder decoder ([&nextId] { return nextId++; }, settings);
    NoteRecorder recorder;
    auto tick = 0.0;
    auto end = 0.0;

    const auto sink = [&] (const LiveInput& input) { recorder.add ({ input, tick }); };

    for (const auto* message : messages)
    {
        tick = message->getTimeStamp() * scale;
        end = std::max (end, tick);
        decoder.process (*message, tick, sink);
    }

    tick = end;
    decoder.releaseAll (end, sink);

    const auto length = std::llround (end) + 1;
    const auto notes = recorder.notes (0, length);

    if (notes.size() == 0)
        return {};

    return makeObject ({ { "name", name },
                         { "drums", drums },
                         { "length", barCeiling (signatures, length - 1) },
                         { "notes", notes },
                         { "controllers", recorder.controllers (0, length) } });
}
} // namespace

juce::var parseMidiFile (const juce::MemoryBlock& data, const juce::String& fileName)
{
    juce::MemoryInputStream stream (data, false);
    juce::MidiFile file;

    if (! file.readFrom (stream, true))
        throw CommandError ("This isn't a MIDI file");

    if (file.getTimeFormat() <= 0)
        throw CommandError ("This MIDI file is timed in SMPTE frames, which can't be imported");

    const auto scale = static_cast<double> (ticksPerBeat) / file.getTimeFormat();
    const auto mpe = isMpeFile (file);
    const auto signatures = signaturesOf (file, scale);
    juce::Array<juce::var> tracks;

    for (int t = 0; t < file.getNumTracks(); ++t)
    {
        const auto* track = file.getTrack (t);
        juce::String trackName;
        std::map<int, std::vector<const juce::MidiMessage*>> channels;
        std::set<int> withNotes;

        for (const auto* holder : *track)
        {
            const auto& message = holder->message;

            if (message.isTrackNameEvent() && trackName.isEmpty())
                trackName = message.getTextFromTextMetaEvent().trim();

            if (message.getChannel() == 0)
                continue;

            // An MPE track is one part: its channels are per note.
            channels[mpe ? 0 : message.getChannel()].push_back (&message);

            if (message.isNoteOn())
                withNotes.insert (message.getChannel());
        }

        for (const auto& [channel, messages] : channels)
        {
            if (! mpe && ! withNotes.contains (channel))
                continue;

            auto name = trackName.isNotEmpty() ? trackName : fileName;
            const auto several = ! mpe && withNotes.size() > 1;

            if (several)
                name += " ch " + juce::String (channel);

            auto part = partOf (messages, scale, mpe, name, channel == drumChannel, signatures);

            if (part.isObject())
                tracks.add (part);
        }
    }

    return makeObject ({ { "name", fileName },
                         { "tracks", tracks },
                         { "tempo", tempoOf (file, scale) },
                         { "signatures", signatures } });
}

namespace
{
[[nodiscard]] double toFile (double tick) noexcept
{
    return tick * fileTicksPerBeat / static_cast<double> (ticksPerBeat);
}

[[nodiscard]] juce::MidiMessage at (juce::MidiMessage message, double tick)
{
    message.setTimeStamp (toFile (tick));
    return message;
}

[[nodiscard]] int sevenBit (double value) noexcept
{
    return std::clamp (static_cast<int> (std::lround (value * 127.0)), 0, 127);
}

// Whether plain MIDI would lose something of a note.
[[nodiscard]] bool needsMpe (const CompiledNote& note) noexcept
{
    if (std::abs (note.pitch - std::round (note.pitch)) > 1.0e-6)
        return true;

    constexpr std::array<std::pair<Lane, double>, 4> lanes {
        { { Lane::pitch, 0.0 }, { Lane::pressure, 0.0 }, { Lane::timbre, 0.5 }, { Lane::vibrato, 0.0 } }
    };

    return std::ranges::any_of (lanes,
                                [&note] (const auto& lane)
                                {
                                    return std::ranges::any_of (
                                        note.lanes[static_cast<size_t> (lane.first)], [&lane] (const CurvePoint& point)
                                        { return std::abs (point.value - lane.second) > 1.0e-6; });
                                });
}

void addRpn (juce::MidiMessageSequence& out, int channel, int lsb, int value)
{
    out.addEvent (at (juce::MidiMessage::controllerEvent (channel, rpnMsbController, 0), 0.0));
    out.addEvent (at (juce::MidiMessage::controllerEvent (channel, rpnLsbController, lsb), 0.0));
    out.addEvent (at (juce::MidiMessage::controllerEvent (channel, dataEntryController, value), 0.0));
}

// A note on its own member channel, with its curves as bends, pressure and CC74.
void addMpeNote (juce::MidiMessageSequence& out, const CompiledNote& note, int channel, const TempoMap& map)
{
    const auto key = std::clamp (static_cast<int> (std::lround (note.pitch)), 0, 127);
    auto lastBend = -1;
    auto lastPressure = -1;
    auto lastTimbre = -1;

    const auto sendAt = [&] (double tick)
    {
        const auto semitones = note.pitch - key + note.pitchOffset (tick, map);
        const auto bend = std::clamp (
            pitchWheelCentre + static_cast<int> (std::lround (semitones / memberBendRange * pitchWheelCentre)), 0,
            pitchWheelMax);
        const auto pressure = sevenBit (note.evaluate (Lane::pressure, tick));
        const auto timbre = sevenBit (note.evaluate (Lane::timbre, tick));

        if (bend != lastBend)
            out.addEvent (at (juce::MidiMessage::pitchWheel (channel, bend), tick));

        if (pressure != lastPressure)
            out.addEvent (at (juce::MidiMessage::channelPressureChange (channel, pressure), tick));

        if (timbre != lastTimbre)
            out.addEvent (at (juce::MidiMessage::controllerEvent (channel, timbreController, timbre), tick));

        lastBend = bend;
        lastPressure = pressure;
        lastTimbre = timbre;
    };

    const auto start = static_cast<double> (note.start);
    const auto end = static_cast<double> (note.end);
    sendAt (start);
    out.addEvent (at (juce::MidiMessage::noteOn (channel, key, note.velocity), start));

    for (auto tick = note.start + controlStepTicks; tick < note.end; tick += controlStepTicks)
        sendAt (static_cast<double> (tick));

    out.addEvent (at (juce::MidiMessage::noteOff (channel, key, note.releaseVelocity), end));
}

void addControllers (juce::MidiMessageSequence& out, const TrackSequence& sequence, int channel)
{
    std::map<int, int> last;

    for (const auto& segment : sequence.controllerSegments())
    {
        for (auto tick = segment.start; tick < segment.end; tick += controlStepTicks)
        {
            const auto value = segment.valueAt (static_cast<double> (tick));
            const auto time = static_cast<double> (tick);

            if (segment.controller == controllerKeyPitchBend)
            {
                const auto bend = std::clamp (static_cast<int> (std::lround (value * pitchWheelMax)), 0, pitchWheelMax);

                if (const auto it = last.find (segment.controller); it == last.end() || it->second != bend)
                    out.addEvent (at (juce::MidiMessage::pitchWheel (channel, bend), time));

                last[segment.controller] = bend;
                continue;
            }

            const auto seven = sevenBit (value);

            if (const auto it = last.find (segment.controller); it != last.end() && it->second == seven)
                continue;

            last[segment.controller] = seven;
            out.addEvent (at (segment.controller == controllerKeyPressure
                                  ? juce::MidiMessage::channelPressureChange (channel, seven)
                                  : juce::MidiMessage::controllerEvent (channel, segment.controller, seven),
                              time));
        }
    }
}

[[nodiscard]] juce::MidiMessageSequence tempoTrack (const TempoMap& map, const juce::String& name)
{
    juce::MidiMessageSequence out;
    out.addEvent (at (juce::MidiMessage::textMetaEvent (3, name), 0.0));

    const auto tempoAt = [&out, &map] (Ticks tick)
    {
        out.addEvent (at (juce::MidiMessage::tempoMetaEvent (
                              static_cast<int> (std::lround (60.0e6 / map.bpmAt (static_cast<double> (tick))))),
                          static_cast<double> (tick)));
    };

    const auto& points = map.points();

    // MIDI tempos are steps: a ramp becomes one per beat.
    for (size_t i = 0; i < points.size(); ++i)
    {
        tempoAt (points[i].tick);

        if (i + 1 < points.size() && points[i].ramp)
            for (auto tick = points[i].tick + ticksPerBeat; tick < points[i + 1].tick; tick += ticksPerBeat)
                tempoAt (tick);
    }

    for (const auto& signature : map.signatures())
        out.addEvent (at (juce::MidiMessage::timeSignatureMetaEvent (signature.numerator, signature.denominator),
                          static_cast<double> (map.tickOfBar (signature.bar))));

    out.sort();
    return out;
}
} // namespace

juce::MemoryBlock writeMidiFile (const Project& project)
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote (fileTicksPerBeat);
    file.addTrack (tempoTrack (project.tempoMap, "Tempo"));

    for (const auto& track : project.tracks)
    {
        if (track->kind != TrackKind::instrument)
            continue;

        const auto played = playedClips (track->clips);
        const TrackSequence sequence { std::span<const ClipPtr> (played) };
        const auto notes = sequence.notes();

        if (notes.empty())
            continue;

        juce::MidiMessageSequence out;
        out.addEvent (at (juce::MidiMessage::textMetaEvent (3, track->name), 0.0));

        if (std::ranges::any_of (notes, needsMpe))
        {
            addRpn (out, masterChannel, mpeConfigurationRpn, memberChannels);

            for (auto channel = firstMemberChannel; channel <= lastMemberChannel; ++channel)
                addRpn (out, channel, 0, memberBendRange);

            // Each note takes the channel free longest; with more than the channels hold, the one ending soonest.
            std::array<Ticks, memberChannels> busyUntil {};

            for (const auto& note : notes)
            {
                const auto slot = std::ranges::min_element (busyUntil) - busyUntil.begin();
                busyUntil[static_cast<size_t> (slot)] = note.end;
                addMpeNote (out, note, firstMemberChannel + static_cast<int> (slot), project.tempoMap);
            }

            addControllers (out, sequence, masterChannel);
        }
        else
        {
            const auto channel = track->drumMode == "drums" ? drumChannel : masterChannel;

            for (const auto& note : notes)
            {
                const auto key = std::clamp (static_cast<int> (std::lround (note.pitch)), 0, 127);
                out.addEvent (
                    at (juce::MidiMessage::noteOn (channel, key, note.velocity), static_cast<double> (note.start)));
                out.addEvent (at (juce::MidiMessage::noteOff (channel, key, note.releaseVelocity),
                                  static_cast<double> (note.end)));
            }

            addControllers (out, sequence, channel);
        }

        out.sort();
        file.addTrack (out);
    }

    juce::MemoryOutputStream stream;
    file.writeTo (stream, 1);
    return stream.getMemoryBlock();
}

} // namespace dawg
