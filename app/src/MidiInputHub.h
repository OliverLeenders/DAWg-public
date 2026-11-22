#pragma once

#include "dawg/AudioEngine.h"
#include "dawg/MidiInput.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <memory>
#include <vector>

namespace dawg
{

/**
    Reads the enabled MIDI input devices and plays them on the input tracks (docs/design/phase9-export-midi-input.md,
    M1–M4): decodes each device (MPE or plain MIDI), routes notes to the target tracks and passes controller
    messages on for MIDI learn.

    The device callbacks run on MIDI threads. They are serialised by a lock, so the engine's live input queue keeps
    a single producer. Everything else is message thread only.
*/
class MidiInputHub final : private juce::MidiInputCallback
{
public:
    /**
        A controller message, for MIDI learn and mappings (M8).
    */
    struct ControllerMessage
    {
        /**
            The device's name.
        */
        juce::String device;

        /**
            MIDI channel, 1..16.
        */
        int channel = 1;

        /**
            Controller number.
        */
        int controller = 0;

        /**
            Value, 0..127.
        */
        int value = 0;
    };

    /**
        Starts listening to all enabled MIDI inputs of a device manager.

        @param devices   The device manager. Must outlive the hub.
        @param engine    The engine that plays the input. Must outlive the hub.
        @param settings  Where per-device settings are kept (key "midiInputs"). Must outlive the hub.
    */
    MidiInputHub (juce::AudioDeviceManager& devices, AudioEngine& engine, juce::PropertySet& settings);

    /**
        Stops listening.
    */
    ~MidiInputHub() override;

    MidiInputHub (const MidiInputHub&) = delete;
    MidiInputHub& operator= (const MidiInputHub&) = delete;
    MidiInputHub (MidiInputHub&&) = delete;
    MidiInputHub& operator= (MidiInputHub&&) = delete;

    /**
        Sets the tracks that hear input from now on. Held notes stay on the tracks they started on.

        @param tracks  The armed instrument tracks, or the selected instrument track, or none.
    */
    void setTargets (std::vector<ObjectId> tracks);

    /**
        Lists the MIDI inputs with their state and settings.

        @return  [{ identifier, name, enabled, mpe, memberBendRange, channelBendRange }]
    */
    [[nodiscard]] juce::var devices() const;

    /**
        Enables or disables a MIDI input and changes its settings.

        @param identifier  The device identifier.
        @param fields      { enabled?, mpe?, memberBendRange?, channelBendRange? }
    */
    void configure (const juce::String& identifier, const juce::var& fields);

    /**
        Takes the controller messages received since the last call.

        @return  The messages, oldest first.
    */
    [[nodiscard]] std::vector<ControllerMessage> takeControllers();

    /**
        Takes and resets the count of messages received (for an activity indicator).
    */
    [[nodiscard]] int takeActivity() noexcept { return m_activity.exchange (0); }

    /**
        Whether a device changed its own settings (an MPE configuration message) since the last call, saving that
        change. MIDI threads only note it; the settings file is written here, on the message thread.
    */
    [[nodiscard]] bool takeSettingsChanged();

    /**
        Ends all held notes (when the project changes or the device setup changes).
    */
    void releaseAll();

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;
    [[nodiscard]] MidiInputSettings loadSettings (const juce::String& identifier) const;
    void saveSettings (const juce::String& identifier, const MidiInputSettings& settings);
    MidiInputDecoder& decoderFor (const juce::String& identifier);
    void send (const LiveInput& input);

    juce::AudioDeviceManager& m_devices;
    AudioEngine& m_engine;
    juce::PropertySet& m_settings;

    juce::CriticalSection m_lock; // MIDI threads and the message thread
    std::map<juce::String, std::unique_ptr<MidiInputDecoder>> m_decoders;
    std::vector<ObjectId> m_targets;
    std::map<ObjectId, std::vector<ObjectId>> m_noteTracks; // live note -> tracks it plays on
    std::vector<ControllerMessage> m_controllers;
    std::map<juce::String, MidiInputSettings> m_pendingSettings; // announced by a device, waiting to be saved
    std::atomic<int> m_activity { 0 };
    std::atomic<bool> m_settingsChanged { false };
};

} // namespace dawg
