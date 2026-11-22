#pragma once

#include "AudioLibrary.h"
#include "Bridge.h"
#include "MidiInputHub.h"
#include "MixerWindow.h"
#include "PianoRollWindow.h"
#include "PluginHost.h"
#include "PluginScanner.h"
#include "PluginWindows.h"
#include "ProjectManager.h"
#include "UiResources.h"
#include "WindowChrome.h"
#include "dawg/AudioEngine.h"
#include "dawg/Export.h"
#include "dawg/GraphCompiler.h"
#include "dawg/Session.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <thread>

namespace dawg
{

/**
    Hosts the web UI and owns the app's long-lived services: audio device, engine, session and plugin
    scanner.

    @note  Message thread only.
*/
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    /**
        Creates the services, opens the audio device and loads the UI.

        @param appProperties Application settings (audio device state, plugin list). Must outlive this object.
        @param devServerUrl  If set, the UI is loaded from this URL (the Vite dev server with hot reload)
                             instead of the bundled files.
    */
    MainComponent (juce::ApplicationProperties& appProperties, std::optional<juce::String> devServerUrl);

    /**
        Stops audio before the engine and session are destroyed.
    */
    ~MainComponent() override;

    MainComponent (const MainComponent&) = delete;
    MainComponent& operator= (const MainComponent&) = delete;
    MainComponent (MainComponent&&) = delete;
    MainComponent& operator= (MainComponent&&) = delete;

    /**
        Quits after resolving unsaved changes: asks the UI to prompt if there are any (main window close,
        Alt+F4, Cmd+Q).
    */
    void requestQuit();

    /**
        Lays out the web view to fill the component.
    */
    void resized() override;

    /**
        Paints the background and, if the UI is missing, instructions to build it.

        @param g  The graphics context.
    */
    void paint (juce::Graphics& g) override;

    /**
        Whether the web UI's files were found. Without them the window shows a message instead.

        @return  true if the UI runs.
    */
    [[nodiscard]] bool hasUi() const noexcept { return m_missingUiMessage.isEmpty(); }

    /**
        What the main window does about its web view's trouble: Reload interface in its window menu, and the window's
        events, logged and answered (docs/design/ui-recovery.md; UiRecovery.cpp).
    */
    [[nodiscard]] windowchrome::WindowHooks windowHooks();

private:
    void initialiseAudio();
    void initialiseModel();
    void updateEngine();
    void scheduleEngineUpdate();
    [[nodiscard]] ProcessSpec currentSpec() const;
    void initialiseBrowser();
    [[nodiscard]] juce::WebBrowserComponent::Options browserOptions();
    [[nodiscard]] juce::String pageUrl (const juce::String& window) const;
    void registerHandlers();
    void registerModelHandlers();
    void registerTransportHandlers();
    void registerPluginHandlers();
    void openAudioSettings();
    void checkAllocationGuard();
    void recompileEngine();
    void restartPlugins (const std::function<bool (const PluginRef&)>& which);
    void restartPluginNodes (const std::function<bool (ObjectId trackId, const GraphNode& node)>& which);
    [[nodiscard]] juce::MemoryBlock duplicateState (ObjectId trackId, ObjectId nodeId);
    void emitPluginStatus();
    [[nodiscard]] juce::var pluginStatus();
    [[nodiscard]] juce::var portWidths() const;
    [[nodiscard]] juce::var trackMeters();
    [[nodiscard]] juce::var invalidTracks();
    [[nodiscard]] PluginNode& requirePluginNode (ObjectId trackId, ObjectId nodeId) const;
    void stampPlugin (ObjectId trackId, ObjectId nodeId);
    void stampTrackPlugins (const Track& track);
    void stampBeforeCommand (const juce::String& command, const juce::var& args);
    void stampAllPlugins();
    void initialiseProjects();
    void registerProjectHandlers();
    void emitProjectState();
    void registerAudioHandlers();
    void registerAudioFileHandlers();
    void registerAudioClipHandlers();
    void registerAudioAnalysisHandlers();
    void registerAudioNormaliseHandlers();
    void importFiles (std::vector<juce::File> files, ObjectId trackId, Ticks start);
    void importSamples (std::vector<juce::File> files, ObjectId trackId, ObjectId nodeId, const juce::var& options);
    void importSfz (const juce::File& patch, ObjectId trackId, ObjectId nodeId);
    void importImpulse (juce::File file, ObjectId trackId, ObjectId nodeId);
    void locateMissing (ObjectId fileId);
    void registerRecordHandlers();
    void registerExplorerHandlers();
    void registerWindowHandlers();
    void showMixer (bool show);
    void showPianoRoll (bool show, int trackId, int clipId);
    [[nodiscard]] juce::var explorerRoots() const;
    void addExplorerRoot (const juce::File& folder);
    [[nodiscard]] juce::var listFolder (const juce::File& folder);
    [[nodiscard]] static juce::var searchFiles (const juce::File& folder, const juce::String& query);
    void startRecording (bool countIn);
    void finishRecording();
    void setArmed (ObjectId trackId, bool armed);

    // Faust scripting (docs/design/phase11-faust-panels.md; FaustControl.cpp).
    void initialiseFaust();
    void shutdownFaust();
    void faustCompiled();
    void addFaustStatus (juce::DynamicObject& status);
    [[nodiscard]] juce::var faustMeters();
    [[nodiscard]] juce::var loopMeters();
    [[nodiscard]] juce::var voiceCounts();
    void registerFaustHandlers();

    // Device panels (docs/design/phase11-faust-panels.md, P1 and P2; PanelControl.cpp).
    void registerPanelHandlers();
    using WatchedNode = std::pair<ObjectId, ObjectId>;
    // Sets the nodes a window watches (an empty list forgets the window) and runs the taps of every window's nodes.
    void watchNodes (const juce::String& window, std::vector<WatchedNode> nodes);
    void emitAnalysis();
    void emitAnalysis (const WatchedNode& watched);
    [[nodiscard]] const NodeDescriptor* descriptorOf (ObjectId trackId, const GraphNode& node) const;

    // MIDI input (docs/design/phase9-export-midi-input.md; MidiControl.cpp).
    void initialiseMidi();
    void registerMidiHandlers();
    void updateMidiTargets();
    void pollMidi();
    void applyMidiMapping (const MidiMapping& mapping, int value);
    [[nodiscard]] juce::Array<juce::var> noteRecordingCommands (double stopBeats);
    // Whether loop recording merges every pass of notes into one clip instead of a take per pass (takes-lanes.md, T4).
    [[nodiscard]] bool loopOverdub() const;
    [[nodiscard]] int preRollBars() const;

    // Automation (docs/design/phase10-automation.md; AutomationControl.cpp).
    struct AutomationCapture;
    void registerAutomationHandlers();
    void touchAutomation (ObjectId trackId, const AutomationTarget& target, bool touching);
    void touchAutomationFromMidi (ObjectId trackId, const AutomationTarget& target);
    void syncAutomationOverrides();
    [[nodiscard]] double manualAutomationValue (const Track& track, const AutomationTarget& target) const;
    void pollAutomation();
    void startWriteModeCaptures (const Project& project);
    void sampleCapture (AutomationCapture& capture, const Project& project, double tick) const;
    void finishAutomationPass();
    static void addCaptureCommands (const AutomationCapture& capture, const Track& track,
                                    juce::Array<juce::var>& commands);
    [[nodiscard]] juce::var automationValues();

    // Export (docs/design/phase9-export-midi-input.md, X1–X5; ExportControl.cpp).
    struct ExportJob;
    void registerComponentHandlers();
    void registerExportHandlers();
    void registerMidiFileHandlers();
    void registerMidiImportExportHandlers();

    // When the web view stops showing the UI (docs/design/ui-recovery.md; UiRecovery.cpp).
    void startUiRecovery();
    void registerRecoveryHandlers();
    void reloadUi (const juce::String& reason);
    void windowEvent (const juce::String& what);
    void checkUiHeartbeat (bool appActive, bool minimised);
    void askToQuit();
    void showNativeQuitPrompt();

    // Update checks (docs/design/update-checks.md; UpdateControl.cpp).
    void registerUpdateHandlers();
    void stopUpdateFetch();
    void startExport (const juce::var& options);
    void startOfflineExport();
    void showAudioEditor (bool show, int clipId);
    void showInstrumentEditor (bool show, int trackId, int nodeId);
    void renderStretchesThenExport (std::vector<StretchJob> stretch);
    void stretchReady (const juce::Array<juce::var>& clipIds);
    void emitStretchStatus (const juce::Array<juce::var>& ready);
    [[nodiscard]] juce::String renderExportPasses (ExportJob& job, const ProcessSpec& spec,
                                                   const std::function<void (const juce::var&)>& progress);
    void startRealtimeExport();
    void pollRealtimeExport (float peak);
    void finaliseExport();
    void endExport (const juce::String& error);

    // Track renders: freeze, bounce and render selection (docs/design/audio-features.md, AU14-AU17).
    struct TrackRender
    {
        ExportRange range;
        juce::File target;
        std::vector<juce::File> tapTargets; // one per range.taps, written beside the target (a freeze, AU15)
        juce::String label;
        std::function<void (const juce::File& file, juce::int64 frames)> done;
        std::optional<Project> project; // compiled for the render instead of the session's (Consolidate)
    };
    void startTrackRender (TrackRender render);
    void finishTrackRender();
    void freezeTrack (ObjectId trackId);
    void bounceTrack (ObjectId trackId);
    void consolidateClips (const juce::var& clipIds);
    void renderSelection (const juce::var& trackIds, const juce::var& clipIds);
    [[nodiscard]] juce::var renderedFile (const juce::File& file, juce::int64 frames, const juce::String& name) const;
    [[nodiscard]] juce::File exportFolder() const;
    void waitForExport();
    [[nodiscard]] bool offlineExportRunning() const;
    void stopTransport();
    [[nodiscard]] juce::var audioDeviceInfo() const;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    juce::ApplicationProperties& m_properties;
    juce::AudioDeviceManager m_deviceManager;

    // Declared before the engine and compiler: plugin instances (owned by nodes there) must be destroyed
    // before their formats and the host listening to them.
    juce::AudioPluginFormatManager m_pluginFormats;
    std::unique_ptr<PluginScanner> m_pluginScanner;
    std::unique_ptr<PluginHost> m_pluginHost;
    juce::String m_lastPluginStatus; // what the UI was last sent, to skip unchanged status
    juce::String m_lastPortWidths;   // likewise for the compiled port widths

    // Before the engine and compiler too: streamed audio sources use its read-ahead thread.
    std::unique_ptr<AudioLibrary> m_audio;
    // Stretch renders (docs/design/time-stretching.md, TS7): before the engine, which plays them.
    std::unique_ptr<StretchRenderer> m_stretch;
    bool m_stretchRecompileScheduled = false;
    juce::Array<juce::var> m_lastStretchPending;
    std::map<juce::String, juce::File> m_uploads; // audio dropped into the UI, arriving in chunks
    std::unique_ptr<juce::FileChooser> m_audioChooser;
    std::unique_ptr<juce::FileChooser> m_midiChooser;
    std::unique_ptr<juce::FileChooser> m_samplerChooser;
    std::shared_future<juce::StringArray> m_midiFilesIndex; // the bundled MIDI files' relative paths, for search

    // Recording (docs/design/phase7-audio-tracks.md, T9): armed tracks are session state.
    struct Take
    {
        ObjectId trackId = 0;
        juce::File file;
        int channels = 1;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    };

    std::set<ObjectId> m_armed;
    std::vector<Take> m_takes;
    juce::TimeSliceThread m_recordThread { "DAWg recorder" };

    AudioEngine m_engine;
    GraphCompiler m_compiler;
    juce::uint32 m_lastLateCheck = 0;
    std::map<juce::String, std::vector<WatchedNode>> m_watchesByWindow; // per window name (nodes.watch)
    std::vector<WatchedNode> m_watchedNodes;                            // the union of them, whose taps run
    std::unique_ptr<juce::dsp::FFT> m_fft;
    std::vector<float> m_analysisLeft;
    std::vector<float> m_analysisRight;
    std::vector<float> m_fftData;

    // Each watched node's spectrum, averaged over time per FFT bin (P2), and when it was last updated.
    struct SpectrumState
    {
        std::vector<float> powers;
        std::vector<float> sidePowers;
        double updatedMs = 0.0;
        // The voice whose instance is watched, for a node inside a Voice Instrument's voice graph; -1 for a track node.
        int voice = -1;
    };

    std::map<WatchedNode, SpectrumState> m_spectra;
    Session m_session;

    // MIDI input, after the engine it plays and before the device manager stops.
    std::unique_ptr<MidiInputHub> m_midi;
    ObjectId m_selectedTrack = 0;
    std::optional<MidiMapping> m_learning;                                     // the parameter waiting for a controller
    std::map<juce::String, std::pair<juce::String, double>> m_mappingGestures; // target -> gesture, last time
    std::map<ObjectId, NoteRecorder> m_noteRecorders; // armed instrument tracks while recording
    int m_midiActivity = 0;                           // messages received in the last timer period

    std::shared_ptr<ExportJob> m_export; // the running export, if any
    juce::uint32 m_lastHeartbeat = 0;    // when the page last said it is alive (ui.alive)
    bool m_heartbeatSeen = false;        // whether it has since it was loaded
    bool m_quitPromptPending = false;    // the unsaved-changes prompt for quitting is out
    bool m_quitPromptShown = false;      // and the page said it shows it
    std::thread m_updateFetch;           // fetching the releases list from GitHub
    std::atomic<bool> m_updateBusy { false };
    std::atomic<bool> m_updateCancel { false };
    bool m_engineUpdateScheduled = false;
    std::map<juce::String, std::shared_ptr<AutomationCapture>> m_automationCaptures; // parameters being written
    std::unique_ptr<juce::FileChooser> m_exportChooser;

    // Declared after the engine and compiler: editor windows must close before their plugins are destroyed.
    std::unique_ptr<PluginWindows> m_pluginWindows;

    std::unique_ptr<ProjectManager> m_projects;

    UiResources m_uiResources;
    std::optional<juce::String> m_devServerUrl;
    Bridge m_bridge;
    std::unique_ptr<juce::WebBrowserComponent> m_browser;
    [[nodiscard]] juce::DocumentWindow* windowNamed (const juce::String& name);

    std::unique_ptr<MixerWindow> m_mixerWindow;           // after the bridge: destroyed before it
    std::unique_ptr<PianoRollWindow> m_pianoRollWindow;   // after the bridge: destroyed before it
    std::unique_ptr<PianoRollWindow> m_audioEditorWindow; // the audio editor (time-stretching.md, TS13)
    std::unique_ptr<PianoRollWindow>
        m_instrumentWindow; // an instrument's editor (macros-and-modulation-column.md, MC14)
    juce::String m_missingUiMessage;
    std::size_t m_reportedAllocationViolations = 0;

    JUCE_LEAK_DETECTOR (MainComponent)
};

} // namespace dawg
