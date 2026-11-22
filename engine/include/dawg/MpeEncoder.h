#pragma once

#include "dawg/Node.h"
#include "dawg/Sequence.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <optional>

namespace dawg
{

/**
    Turns note events with their curves and vibrato into MIDI for a plugin. See
    docs/design/phase4-plugin-hosting.md, section 5.

    MPE mode uses a lower zone: master channel 1 and member channels 2–16, one note per member channel, with
    per-note pitch bend (pitch curve + vibrato + the fraction of the base pitch), channel pressure and CC74
    (timbre). Expression is evaluated every controlInterval samples and sent only when its quantised value
    changes. MIDI mode sends everything on channel 1 with polyphonic aftertouch for pressure and no bends.

    @note  Audio thread, except prepare(). Never allocates once the output buffer has been sized with
           requiredBufferBytes().
*/
class MpeEncoder final
{
public:
    /**
        Samples between expression updates (about 0.7 ms at 48 kHz, as in the built-in synth).
    */
    static constexpr int controlInterval = 32;

    /**
        Number of MPE member channels (2–16).
    */
    static constexpr int numMemberChannels = 15;

    /**
        Bytes a MidiBuffer needs to hold one block of output without allocating.

        @param maxBlockSize  The largest block size.
        @return              The size to pass to juce::MidiBuffer::ensureSize().
    */
    [[nodiscard]] static size_t requiredBufferBytes (int maxBlockSize) noexcept;

    /**
        Forgets all sounding notes and makes the next block send the zone configuration again.

        @param sampleRate  The sample rate, for vibrato timing.
    */
    void prepare (double sampleRate) noexcept;

    /**
        Changes the output settings. Takes effect at the next block, which then ends sounding notes and sends
        the configuration again.

        @param settings  The new settings.
        @note  Any thread (usually the message thread).
    */
    void setSettings (const MidiOutputSettings& settings) noexcept;

    /**
        Renders one block of MIDI from the context's events and live events.

        @param context  The node's process context (events, transport, sequence).
        @param midi     Receives the messages, with sample positions within the block. Not cleared first.
    */
    void process (const ProcessContext& context, juce::MidiBuffer& midi) noexcept;

private:
    struct Channel
    {
        ObjectId noteId = 0;
        const CompiledNote* note = nullptr; // null when the channel is free
        int noteNumber = 0;
        juce::uint64 age = 0; // note-on or release order, for allocation
        int bend = -1;
        int pressure = -1;
        int timbre = -1;
        bool live = false; // expression comes from live events (MIDI input) instead of curves
        double livePitch = 0.0;
        double livePressure = 0.0;
        double liveTimbre = 0.5;
        LinkGlide glide; // the way from a linked note to this one
    };

    void sendConfiguration (juce::MidiBuffer& midi) const noexcept;
    void endNotes (std::optional<ObjectId> noteId, bool release, int offset, juce::MidiBuffer& midi) noexcept;
    [[nodiscard]] int firstChannel() const noexcept;
    [[nodiscard]] int lastChannel() const noexcept;
    void adoptSequence (const TrackSequence& sequence, juce::MidiBuffer& midi) noexcept;
    void handle (const NoteEvent& event, const ProcessContext& context, juce::MidiBuffer& midi) noexcept;
    void handleLive (const NoteEvent& event, juce::MidiBuffer& midi) noexcept;
    void noteOn (const NoteEvent& event, double tick, const TempoMap& map, int offset, juce::MidiBuffer& midi) noexcept;
    void legato (const NoteEvent& event, double tick, const TempoMap& map, int offset, juce::MidiBuffer& midi) noexcept;
    void noteOff (Channel& channel, float velocity, int offset, juce::MidiBuffer& midi) noexcept;
    void updateExpression (Channel& channel, double tick, const TempoMap& map, int offset,
                           juce::MidiBuffer& midi) noexcept;
    void updateAll (const ProcessContext& context, int from, int to, juce::MidiBuffer& midi) noexcept;
    [[nodiscard]] Channel& allocate (int offset, juce::MidiBuffer& midi) noexcept;
    [[nodiscard]] int midiChannel (const Channel& channel) const noexcept;

    [[nodiscard]] static int pack (const MidiOutputSettings& settings) noexcept;

    std::array<Channel, numMemberChannels> m_channels {};
    MidiOutputSettings m_settings;
    std::atomic<int> m_requestedSettings { pack ({}) }; // written by the message thread
    int m_appliedSettings = pack ({});
    bool m_needsConfiguration = true;
    const TrackSequence* m_sequence = nullptr;
    double m_sampleRate = 48000.0;
    juce::uint64 m_counter = 0;
};

} // namespace dawg
