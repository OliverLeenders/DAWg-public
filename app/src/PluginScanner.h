#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>

namespace dawg
{

/**
    Scans installed plugins and keeps the resulting KnownPluginList persisted.

    Scanning runs on a background thread, with one child process per plugin file (see ScanWorker.h), so a
    crashing or hanging plugin only fails its own entry.

    @note  Public members: message thread only.
*/
class PluginScanner final : private juce::Thread,
                            private juce::AsyncUpdater
{
public:
    /**
        Longest a single plugin file may take to scan before its process is killed.
    */
    static constexpr int perPluginTimeoutMs = 30000;

    /**
        Creates a scanner and loads the previously saved plugin list.

        @param formats   The formats to scan. Must outlive the scanner.
        @param settings  Where the plugin list is persisted. Must outlive the scanner.
    */
    PluginScanner (juce::AudioPluginFormatManager& formats, juce::PropertiesFile& settings);

    /**
        Stops a running scan, waiting for the current plugin's process to finish or time out.
    */
    ~PluginScanner() override;

    PluginScanner (const PluginScanner&) = delete;
    PluginScanner& operator= (const PluginScanner&) = delete;
    PluginScanner (PluginScanner&&) = delete;
    PluginScanner& operator= (PluginScanner&&) = delete;

    /**
        Clears the plugin list and rescans every format's default locations. Does nothing if a scan is
        already running.
    */
    void startScan();

    /**
        Returns true while a scan is running.
    */
    [[nodiscard]] bool isScanning() const noexcept { return m_scanning.load(); }

    /**
        Returns the plugins found so far.
    */
    [[nodiscard]] juce::KnownPluginList& getKnownPlugins() noexcept { return m_knownPlugins; }

    /**
        Returns the scan state for the UI.

        @return  { scanning, progress, current, plugins: [...], failed: [...] }
    */
    [[nodiscard]] juce::var toVar() const;

    /**
        Sets the callback invoked on the message thread whenever the scan state or the plugin list changes.

        @param callback  The callback, or nullptr to remove it.
    */
    void setChangeCallback (std::function<void()> callback) { m_onChange = std::move (callback); }

private:
    void run() override;
    void handleAsyncUpdate() override;

    void scanFile (juce::AudioPluginFormat& format, const juce::String& identifier);
    void setCurrent (const juce::String& name, float progress);
    void save();

    juce::AudioPluginFormatManager& m_formats;
    juce::PropertiesFile& m_settings;
    juce::KnownPluginList m_knownPlugins;

    std::atomic<bool> m_scanning { false };
    mutable juce::CriticalSection m_stateLock;
    float m_progress = 0.0f;
    juce::String m_current;
    juce::StringArray m_failed;
    std::function<void()> m_onChange;
};

} // namespace dawg
