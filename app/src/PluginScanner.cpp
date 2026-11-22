#include "PluginScanner.h"

#include "ScanWorker.h"
#include "dawg/VarUtils.h"

#include <vector>

namespace dawg
{

namespace
{
constexpr const char* knownPluginsKey = "knownPlugins";
constexpr int stopTimeoutMs = PluginScanner::perPluginTimeoutMs + 5000;

[[nodiscard]] juce::String displayName (juce::AudioPluginFormat& format, const juce::String& identifier)
{
    return juce::File::isAbsolutePath (identifier) ? juce::File (identifier).getFileNameWithoutExtension()
                                                   : format.getNameOfPluginFromIdentifier (identifier);
}

[[nodiscard]] juce::var describe (const juce::PluginDescription& type)
{
    return makeObject ({
        { "id", type.createIdentifierString() },
        { "name", type.name },
        { "manufacturer", type.manufacturerName },
        { "format", type.pluginFormatName },
        { "category", type.category },
        { "version", type.version },
        { "isInstrument", type.isInstrument },
        { "file", type.fileOrIdentifier },
    });
}
} // namespace

PluginScanner::PluginScanner (juce::AudioPluginFormatManager& formats, juce::PropertiesFile& settings)
    : juce::Thread ("DAWg plugin scanner"),
      m_formats (formats),
      m_settings (settings)
{
    if (const auto xml = m_settings.getXmlValue (knownPluginsKey))
        m_knownPlugins.recreateFromXml (*xml);
}

PluginScanner::~PluginScanner()
{
    std::ignore = stopThread (stopTimeoutMs);
    cancelPendingUpdate();
}

void PluginScanner::startScan()
{
    if (m_scanning.exchange (true))
        return;

    // The previous run has finished but the thread object may still be winding down.
    std::ignore = stopThread (stopTimeoutMs);

    {
        const juce::ScopedLock sl (m_stateLock);
        m_progress = 0.0f;
        m_current.clear();
        m_failed.clear();
    }

    m_knownPlugins.clear();
    std::ignore = startThread();
    triggerAsyncUpdate();
}

void PluginScanner::run()
{
    struct Job
    {
        juce::AudioPluginFormat* format;
        juce::String identifier;
    };

    std::vector<Job> jobs;

    for (auto* format : m_formats.getFormats())
        for (const auto& id : format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false))
            jobs.push_back ({ format, id });

    for (size_t i = 0; i < jobs.size() && ! threadShouldExit(); ++i)
    {
        setCurrent (displayName (*jobs[i].format, jobs[i].identifier),
                    static_cast<float> (i) / static_cast<float> (jobs.size()));
        scanFile (*jobs[i].format, jobs[i].identifier);
    }

    setCurrent ({}, 1.0f);
    m_scanning.store (false);
    triggerAsyncUpdate();
}

void PluginScanner::setCurrent (const juce::String& name, float progress)
{
    {
        const juce::ScopedLock sl (m_stateLock);
        m_current = name;
        m_progress = progress;
    }

    triggerAsyncUpdate();
}

void PluginScanner::scanFile (juce::AudioPluginFormat& format, const juce::String& identifier)
{
    const juce::TemporaryFile output (".xml");
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const juce::StringArray command { exe.getFullPathName(), scanWorkerSwitch, format.getName(), identifier,
                                      output.getFile().getFullPathName() };

    juce::ChildProcess child;
    auto ok = child.start (command, 0);

    if (ok && ! child.waitForProcessToFinish (perPluginTimeoutMs))
    {
        std::ignore = child.kill();
        ok = false;
    }

    ok = ok && child.getExitCode() == static_cast<juce::uint32> (ScanWorkerExitCode::success);

    const auto xml = ok ? juce::parseXML (output.getFile()) : nullptr;

    if (xml != nullptr)
    {
        for (const auto* element : xml->getChildIterator())
        {
            juce::PluginDescription description;

            if (description.loadFromXml (*element))
                std::ignore = m_knownPlugins.addType (description);
        }
    }
    else
    {
        const juce::ScopedLock sl (m_stateLock);
        m_failed.add (identifier);
    }
}

void PluginScanner::handleAsyncUpdate()
{
    if (! m_scanning.load())
        save();

    if (m_onChange != nullptr)
        m_onChange();
}

void PluginScanner::save()
{
    if (const auto xml = m_knownPlugins.createXml())
        m_settings.setValue (knownPluginsKey, xml.get());

    std::ignore = m_settings.saveIfNeeded();
}

juce::var PluginScanner::toVar() const
{
    juce::Array<juce::var> plugins;

    for (const auto& type : m_knownPlugins.getTypes())
        plugins.add (describe (type));

    const juce::ScopedLock sl (m_stateLock);
    return makeObject ({
        { "plugins", plugins },
        { "scanning", m_scanning.load() },
        { "progress", m_progress },
        { "current", m_current },
        { "failed", juce::var (m_failed) },
    });
}

} // namespace dawg
