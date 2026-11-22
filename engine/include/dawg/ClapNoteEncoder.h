#pragma once

#include "dawg/ClapPlugin.h"
#include "dawg/Modulation.h"
#include "dawg/Node.h"
#include "dawg/Sequence.h"

#include <array>
#include <vector>

namespace dawg
{

/**
    Turns a track's note events into CLAP's own notes (docs/design/clap-hosting.md, C8, C11, C12): each sounding note
    gets a note id, its pitch, pressure and timbre go out as note expressions, and its values keep going until the plugin
    says the voice has finished. A linked note carries on under the id of the note before it, gliding by tuning.

    @note  prepare(): message thread. The rest: audio thread; never allocates.
*/
class ClapNoteEncoder final : public ClapNoteEndListener
{
public:
    /**
        Forgets every note.

        @param sampleRate    The sample rate.
        @param maxBlockSize  The largest block.
    */
    void prepare (double sampleRate, int maxBlockSize);

    /**
        Sends a block's notes.

        @param context  The block's events, transport and sequence.
        @param output   The plugin.
    */
    void process (const ProcessContext& context, ClapNativeInput& output) noexcept;

    void noteEnded (int32_t noteId) noexcept override;

    /**
        Returns how many notes have values going out, for tests.
    */
    [[nodiscard]] size_t soundingNotes() const noexcept;

private:
    static constexpr size_t maxNotes = 64;
    static constexpr double unsentValue = -1.0e9;

    // What a note last sent a parameter.
    struct SentModulation
    {
        int parameter = -1;
        double amount = unsentValue;
    };

    struct Slot
    {
        bool used = false;
        ObjectId noteId = 0;                // what it plays now
        const CompiledNote* note = nullptr; // null once the note is gone from the sequence
        int32_t clapId = 0;
        int key = 0;
        juce::uint64 age = 0;
        bool released = false;
        bool sustained = false; // released while the pedal is down
        juce::int64 releasedSamples = 0;
        bool live = false;
        double livePitch = 0.0;
        double livePressure = 0.0;
        double liveTimbre = 0.5;
        LinkGlide glide;
        double tuning = unsentValue;
        double pressure = unsentValue;
        double brightness = unsentValue;
        std::array<SentModulation, maxModulationInputs> modulations {};
        std::array<NoteEvent, 8> sourceEvents {}; // this block's, for its LFO and ADSR instances
        size_t sourceEventCount = 0;
        int startedAt = -1; // the sample it started at, in this block
    };

    void adoptSequence (const TrackSequence& sequence, ClapNativeInput& output) noexcept;
    void handle (const NoteEvent& event, const ProcessContext& context, ClapNativeInput& output) noexcept;
    void start (const NoteEvent& event, const ProcessContext& context, ClapNativeInput& output) noexcept;
    void legato (const NoteEvent& event, const ProcessContext& context, ClapNativeInput& output) noexcept;
    void release (Slot& slot, uint32_t time, ClapNativeInput& output) const noexcept;
    void controller (const NoteEvent& event, ClapNativeInput& output) noexcept;
    void update (Slot& slot, int sample, const ProcessContext& context, ClapNativeInput& output) noexcept;
    void runSources (const ProcessContext& context, ClapNativeInput& output) noexcept;
    static void addSourceEvent (Slot& slot, const NoteEvent& event) noexcept;
    static void sendModulation (Slot& slot, uint32_t time, int parameter, double amount,
                                ClapNativeInput& output) noexcept;
    static void updateModulations (Slot& slot, uint32_t time, const ProcessContext& context, double pitch,
                                   double pressure, double timbre, double velocity, ClapNativeInput& output) noexcept;
    void updateAll (int from, int to, const ProcessContext& context, ClapNativeInput& output) noexcept;
    [[nodiscard]] Slot& allocate (uint32_t time, ClapNativeInput& output) noexcept;
    [[nodiscard]] Slot* holding (ObjectId noteId) noexcept;

    double m_sampleRate = 48000.0;
    std::array<Slot, maxNotes> m_slots {};
    const TrackSequence* m_sequence = nullptr;
    int32_t m_nextId = 0;
    juce::uint64 m_counter = 0;
    bool m_pedal = false;
    std::vector<float> m_sourceOutput;
    std::vector<float> m_sourceSilence;
    EventBuffer m_sourceEvents;
};

} // namespace dawg
