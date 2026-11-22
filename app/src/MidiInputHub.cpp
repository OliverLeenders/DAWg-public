#include "MidiInputHub.h"

#include "dawg/VarUtils.h"

namespace dawg
{

namespace
{
constexpr const char* settingsKey = "midiInputs";
constexpr size_t maxPendingControllers = 1024;
} // namespace

MidiInputHub::MidiInputHub (juce::AudioDeviceManager& devices, AudioEngine& engine, juce::PropertySet& settings)
    : m_devices (devices),
      m_engine (engine),
      m_settings (settings)
{
    // An empty identifier receives every enabled device.
    m_devices.addMidiInputDeviceCallback ({}, this);
}

MidiInputHub::~MidiInputHub()
{
    m_devices.removeMidiInputDeviceCallback ({}, this);
}

void MidiInputHub::setTargets (std::vector<ObjectId> tracks)
{
    const juce::ScopedLock lock (m_lock);
    m_targets = std::move (tracks);
}

MidiInputSettings MidiInputHub::loadSettings (const juce::String& identifier) const
{
    const auto all = juce::JSON::parse (m_settings.getValue (settingsKey));
    const auto& saved = all[juce::Identifier (identifier.isEmpty() ? juce::String ("_") : identifier)];
    MidiInputSettings result;

    if (saved.isObject())
    {
        result.mpe = static_cast<bool> (saved["mpe"]);
        result.memberBendRange = static_cast<int> (saved.getProperty ("memberBendRange", result.memberBendRange));
        result.channelBendRange = static_cast<int> (saved.getProperty ("channelBendRange", result.channelBendRange));
    }

    return result;
}

void MidiInputHub::saveSettings (const juce::String& identifier, const MidiInputSettings& settings)
{
    auto all = juce::JSON::parse (m_settings.getValue (settingsKey));

    if (! all.isObject())
        all = newObject().get();

    all.getDynamicObject()->setProperty (juce::Identifier (identifier.isEmpty() ? juce::String ("_") : identifier),
                                         makeObject ({ { "mpe", settings.mpe },
                                                       { "memberBendRange", settings.memberBendRange },
                                                       { "channelBendRange", settings.channelBendRange } }));
    m_settings.setValue (settingsKey, juce::JSON::toString (all, true));
}

MidiInputDecoder& MidiInputHub::decoderFor (const juce::String& identifier)
{
    auto& decoder = m_decoders[identifier];

    if (decoder == nullptr)
        decoder = std::make_unique<MidiInputDecoder> ([this] { return m_engine.nextLiveNoteId(); },
                                                      loadSettings (identifier));

    return *decoder;
}

juce::var MidiInputHub::devices() const
{
    juce::Array<juce::var> result;

    for (const auto& device : juce::MidiInput::getAvailableDevices())
    {
        const auto settings = loadSettings (device.identifier);
        result.add (makeObject ({ { "identifier", device.identifier },
                                  { "name", device.name },
                                  { "enabled", m_devices.isMidiInputDeviceEnabled (device.identifier) },
                                  { "mpe", settings.mpe },
                                  { "memberBendRange", settings.memberBendRange },
                                  { "channelBendRange", settings.channelBendRange } }));
    }

    return result;
}

void MidiInputHub::configure (const juce::String& identifier, const juce::var& fields)
{
    if (fields.hasProperty ("enabled"))
        m_devices.setMidiInputDeviceEnabled (identifier, static_cast<bool> (fields["enabled"]));

    auto settings = loadSettings (identifier);

    if (fields.hasProperty ("mpe"))
        settings.mpe = static_cast<bool> (fields["mpe"]);

    if (fields.hasProperty ("memberBendRange"))
        settings.memberBendRange = std::clamp (static_cast<int> (fields["memberBendRange"]), 1, 96);

    if (fields.hasProperty ("channelBendRange"))
        settings.channelBendRange = std::clamp (static_cast<int> (fields["channelBendRange"]), 1, 96);

    saveSettings (identifier, settings);

    const juce::ScopedLock lock (m_lock);
    decoderFor (identifier).setSettings (settings);
}

std::vector<MidiInputHub::ControllerMessage> MidiInputHub::takeControllers()
{
    const juce::ScopedLock lock (m_lock);
    return std::exchange (m_controllers, {});
}

void MidiInputHub::releaseAll()
{
    const juce::ScopedLock lock (m_lock);
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    for (auto& [identifier, decoder] : m_decoders)
        decoder->releaseAll (now, [this] (const LiveInput& input) { send (input); });

    m_noteTracks.clear();
}

void MidiInputHub::send (const LiveInput& input)
{
    // Called with the lock held. Notes stay on the tracks they started on (M3).
    std::vector<ObjectId> tracks;

    if (input.type == NoteEventType::noteOn)
    {
        m_noteTracks[input.noteId] = m_targets;
        tracks = m_targets;
    }
    else if (input.noteId != 0)
    {
        const auto it = m_noteTracks.find (input.noteId);

        if (it == m_noteTracks.end())
            return;

        tracks = it->second;

        if (input.type == NoteEventType::noteOff)
            m_noteTracks.erase (it);
    }
    else
    {
        tracks = m_targets;
    }

    for (const auto track : tracks)
    {
        auto routed = input;
        routed.trackId = track;
        std::ignore = m_engine.sendLiveInput (routed);
    }
}

void MidiInputHub::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message)
{
    const auto identifier = source != nullptr ? source->getIdentifier() : juce::String();
    const auto time =
        message.getTimeStamp() > 0.0 ? message.getTimeStamp() : juce::Time::getMillisecondCounterHiRes() * 0.001;
    m_activity.fetch_add (1);

    const juce::ScopedLock lock (m_lock);

    if (message.isController() && m_controllers.size() < maxPendingControllers)
        m_controllers.push_back ({ source != nullptr ? source->getName() : juce::String(), message.getChannel(),
                                   message.getControllerNumber(), message.getControllerValue() });

    auto& decoder = decoderFor (identifier);

    if (decoder.process (message, time, [this] (const LiveInput& input) { send (input); }))
    {
        // The device announced MPE (or a bend range): remember it, and let the message thread save it.
        m_pendingSettings[identifier] = decoder.settings();
        m_settingsChanged.store (true);
    }
}

bool MidiInputHub::takeSettingsChanged()
{
    if (! m_settingsChanged.exchange (false))
        return false;

    std::map<juce::String, MidiInputSettings> pending;
    {
        const juce::ScopedLock lock (m_lock);
        pending = std::exchange (m_pendingSettings, {});
    }

    for (const auto& [identifier, settings] : pending)
        saveSettings (identifier, settings);

    return true;
}

} // namespace dawg
