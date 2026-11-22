#pragma once

#include "dawg/AudioEngine.h"

#include <array>
#include <functional>
#include <map>
#include <vector>

namespace dawg
{

/**
    How one MIDI input device is read (docs/design/phase9-export-midi-input.md, M1).
*/
struct MidiInputSettings
{
    /**
        True: MPE (one member channel per note, per-note bend, pressure and timbre). False: plain MIDI, where pitch
        bend and channel pressure apply to every note held on the channel.
    */
    bool mpe = false;

    /**
        Pitch-bend range in semitones of MPE member channels.
    */
    int memberBendRange = 48;

    /**
        Pitch-bend range in semitones of MPE master channels and of channels in plain MIDI mode.
    */
    int channelBendRange = 2;

    /**
        Compares all fields.

        @param other  The settings to compare with.
        @return       True if they are equal.
    */
    [[nodiscard]] bool operator== (const MidiInputSettings& other) const noexcept = default;
};

/**
    Turns the MIDI messages of one input device into live events (docs/design/phase9-export-midi-input.md, M2, M3).

    MPE: notes on member channels; pitch bend, channel pressure, polyphonic aftertouch and CC74 become per-note
    expression; pitch bend on a master channel (1 or 16) moves every note. Plain MIDI: pitch bend and channel
    pressure become expression of the notes held on their channel. Both: CC64 is sustain, other controllers go to
    plugins. MPE configuration messages (RPN 6) switch to MPE, RPN 0 sets bend ranges.

    @note  Not thread-safe: use it from one MIDI thread at a time.
*/
class MidiInputDecoder final
{
public:
    /**
        Receives decoded events (their trackId is 0: routing is the caller's).
    */
    using Sink = std::function<void (const LiveInput&)>;

    /**
        Creates a decoder.

        @param newNoteId  Returns a fresh live note id (usually AudioEngine::nextLiveNoteId()).
        @param settings   The device's settings.
    */
    MidiInputDecoder (std::function<ObjectId()> newNoteId, MidiInputSettings settings);

    /**
        Returns the current settings (configuration messages from the device may have changed them).
    */
    [[nodiscard]] const MidiInputSettings& settings() const noexcept { return m_settings; }

    /**
        Changes the settings. Held notes keep sounding.

        @param settings  The new settings.
    */
    void setSettings (const MidiInputSettings& settings) noexcept { m_settings = settings; }

    /**
        Decodes one message.

        @param message  The message.
        @param time     When it arrived (see LiveInput::time).
        @param sink     Receives the resulting events.
        @return         True if the message changed the settings (an MPE configuration message).
    */
    bool process (const juce::MidiMessage& message, double time, const Sink& sink);

    /**
        Ends every held note and lifts the sustain pedal (when the device closes or the input moves).

        @param time  The time to use.
        @param sink  Receives the note-offs.
    */
    void releaseAll (double time, const Sink& sink);

private:
    struct ChannelState
    {
        float bend = 0.0f; // semitones
        float pressure = 0.0f;
        float timbre = 0.5f;
        int rpnMsb = 127;
        int rpnLsb = 127;
        std::array<ObjectId, 128> notes {}; // live note id per held key, 0 when up
    };

    [[nodiscard]] bool isMasterChannel (int channel) const noexcept;
    [[nodiscard]] float pitchOf (int channel) const noexcept;
    void sendExpression (int channel, Lane lane, float value, double time, const Sink& sink) const;
    bool handleController (int channel, int controller, int value, double time, const Sink& sink);
    void noteOn (int channel, const juce::MidiMessage& message, double time, const Sink& sink);
    void noteOff (int channel, const juce::MidiMessage& message, double time, const Sink& sink);
    void pitchWheel (int channel, int value, double time, const Sink& sink);
    void channelPressure (int channel, int value, double time, const Sink& sink);
    void polyPressure (int channel, int note, int value, double time, const Sink& sink);

    std::function<ObjectId()> m_newNoteId;
    MidiInputSettings m_settings;
    std::array<ChannelState, 16> m_channels {};
    bool m_sustain = false;
};

/**
    Builds note clips from recorded live events of one track (docs/design/phase9-export-midi-input.md, M5, M6).
    Expression is stored as curves relative to the note start, simplified with a tolerance per lane.
*/
class NoteRecorder final
{
public:
    /**
        Adds a recorded event. Only note-on, note-off and expression events matter.

        @param event  The event, with its timeline position.
    */
    void add (const RecordedInput& event);

    /**
        Whether nothing (no note, no controller) was recorded.
    */
    [[nodiscard]] bool empty() const noexcept { return m_notes.empty() && m_controllers.empty(); }

    /**
        Returns the recorded controllers (sustain, CCs, pitch bend, pressure) as clip controller curves
        (docs/design/phase10-automation.md, A2).

        @param clipStart  The clip's start: positions are relative to it.
        @param end        Where recording stopped (or the loop end).
        @param pass       Only what was recorded in this loop pass, or -1 for every pass (overdub).
        @return           { controller: [[time, value, shape]] }; the sustain pedal is a step curve.
    */
    [[nodiscard]] juce::var controllers (Ticks clipStart, Ticks end, int pass = -1) const;

    /**
        Returns the recorded notes as addClip / addNotes arguments, relative to a clip start.

        @param clipStart  The clip's start: note starts are relative to it. Notes starting before it are dropped.
        @param end        Where recording stopped: notes still held end here, and notes starting after it are dropped.
        @param loopEnd    With loop recording, the loop end: a note held across the wrap ends there. 0 for none.
        @param pass       Only the notes started in this loop pass, or -1 for every pass (overdub).
        @return           [{ start, length, pitch, velocity, releaseVelocity, expression? }]
    */
    [[nodiscard]] juce::var notes (Ticks clipStart, Ticks end, Ticks loopEnd = 0, int pass = -1) const;

    /**
        How many loop passes something was recorded in: one past the last pass with an event.
    */
    [[nodiscard]] int passes() const noexcept { return m_passes; }

    /**
        Simplifies a sampled curve (Ramer–Douglas–Peucker): keeps the fewest points that stay within a tolerance of
        the original. Exposed for tests.

        @param points     (time, value) pairs, sorted by time.
        @param tolerance  The largest allowed deviation in value.
        @return           The kept points (always the first and the last).
    */
    [[nodiscard]] static std::vector<std::pair<double, double>>
    simplify (const std::vector<std::pair<double, double>>& points, double tolerance);

private:
    struct Recorded
    {
        double start = 0.0;
        double end = -1.0; // negative while held
        double pitch = 60.0;
        float velocity = 0.8f;
        float releaseVelocity = 0.5f;
        int pass = 0;
        std::array<std::vector<std::pair<double, double>>, 3> points; // pitch, pressure, timbre: (tick, value)
    };

    struct ControllerPoint
    {
        double tick = 0.0;
        double value = 0.0;
        int pass = 0;
    };

    [[nodiscard]] static juce::var curveVar (const std::vector<std::pair<double, double>>& recorded, double start,
                                             double end, size_t lane);

    std::vector<Recorded> m_notes;
    std::map<ObjectId, size_t> m_open;                         // live note id -> index of the held note
    std::map<int, std::vector<ControllerPoint>> m_controllers; // clip controller key -> points
    int m_passes = 0;
};

} // namespace dawg
