#include "dawg/FaustNode.h"

#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/VarUtils.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <span>
#if DAWG_FAUST_LLVM
 #include <faust/dsp/llvm-dsp.h>
#else
 #include <faust/dsp/interpreter-dsp.h>
#endif
#include <faust/gui/PathBuilder.h>
#include <faust/gui/UI.h>

// Faust scripting (docs/design/phase11-faust-panels.md, F1-F4, F8).

namespace dawg
{

namespace
{
// libfaust keeps global state: compiling, creating instances and deleting factories happen one at a time.
std::mutex faustMutex;

constexpr int maxChannels = 16;

constexpr std::array<PortInfo, 1> emptyPorts { { { PortType::audio, "Audio" } } };
constexpr auto emptyInputs = withModulationInputs (emptyPorts);
const NodeDescriptor emptyDescriptor {
    nodetype::faust, "Faust", emptyInputs, emptyPorts, {}, true, NodeCategory::effect
};

// Splits a Faust menu or radio style ("menu{'Low':0;'High':1}") into its labels, if its values are 0, 1, 2, ...
[[nodiscard]] std::vector<std::string> choiceLabels (const std::string& style)
{
    const auto open = style.find ('{');
    const auto close = style.rfind ('}');

    if ((style.rfind ("menu", 0) != 0 && style.rfind ("radio", 0) != 0) || open == std::string::npos
        || close == std::string::npos || close < open)
        return {};

    std::vector<std::string> labels;
    const auto body = juce::String (style.substr (open + 1, close - open - 1));

    for (const auto& item : juce::StringArray::fromTokens (body, ";", "'"))
    {
        const auto label = item.upToLastOccurrenceOf (":", false, false).trim().unquoted();
        const auto value = item.fromLastOccurrenceOf (":", false, false).trim();

        if (value.getIntValue() != static_cast<int> (labels.size()) || ! value.containsOnly ("0123456789"))
            return {};

        labels.push_back (label.toStdString());
    }

    return labels;
}

[[nodiscard]] bool isWhole (double value) noexcept
{
    return std::abs (value - std::round (value)) < 1.0e-9;
}

// Collects a DSP's controls into a FaustProgram's parameters and layout.
class DescriptorBuilder final : public UI,
                                public PathBuilder
{
public:
    struct Output
    {
        std::deque<std::string> strings;
        std::deque<std::vector<const char*>> choices;
        std::vector<ParamInfo> params;
        size_t meters = 0;
        juce::var layout;
    };

    explicit DescriptorBuilder (Output& output)
        : m_output (output)
    {
        m_groups.push_back (
            makeObject ({ { "kind", "vgroup" }, { "label", "" }, { "items", juce::Array<juce::var>() } }));
    }

    [[nodiscard]] juce::var root() const { return m_groups.front(); }

    void openTabBox (const char* label) override { open ("tgroup", label); }
    void openHorizontalBox (const char* label) override { open ("hgroup", label); }
    void openVerticalBox (const char* label) override { open ("vgroup", label); }

    void closeBox() override
    {
        popLabel();

        if (m_groups.size() > 1)
            m_groups.pop_back();
    }

    void addButton (const char* label, FAUSTFLOAT* /*zone*/) override
    {
        addParam ("button", label, 0.0, 0.0, 1.0, 1.0);
    }
    void addCheckButton (const char* label, FAUSTFLOAT* /*zone*/) override
    {
        addParam ("checkbox", label, 0.0, 0.0, 1.0, 1.0);
    }

    void addVerticalSlider (const char* label, FAUSTFLOAT* /*zone*/, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max,
                            FAUSTFLOAT step) override
    {
        addParam ("vslider", label, init, min, max, step);
    }

    void addHorizontalSlider (const char* label, FAUSTFLOAT* /*zone*/, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max,
                              FAUSTFLOAT step) override
    {
        addParam ("hslider", label, init, min, max, step);
    }

    void addNumEntry (const char* label, FAUSTFLOAT* /*zone*/, FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max,
                      FAUSTFLOAT step) override
    {
        addParam ("nentry", label, init, min, max, step);
    }

    void addHorizontalBargraph (const char* label, FAUSTFLOAT* /*zone*/, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        addMeter ("hbargraph", label, min, max);
    }

    void addVerticalBargraph (const char* label, FAUSTFLOAT* /*zone*/, FAUSTFLOAT min, FAUSTFLOAT max) override
    {
        addMeter ("vbargraph", label, min, max);
    }

    void addSoundfile (const char* /*label*/, const char* /*url*/, Soundfile** /*soundfile*/) override {}

    void declare (FAUSTFLOAT* /*zone*/, const char* key, const char* value) override
    {
        if (key != nullptr && value != nullptr)
            m_pending[key] = value;
    }

private:
    void open (const char* kind, const char* label)
    {
        pushLabel (label);
        const auto group = makeObject (
            { { "kind", kind }, { "label", label }, { "items", juce::Array<juce::var>() }, { "style", style() } });
        m_pending.clear();
        add (group);
        m_groups.push_back (group);
    }

    void add (const juce::var& item) const
    {
        if (auto* items = m_groups.back()["items"].getArray())
            items->add (item);
    }

    [[nodiscard]] juce::String style() const
    {
        const auto it = m_pending.find ("style");
        return it != m_pending.end() ? juce::String (it->second) : juce::String();
    }

    [[nodiscard]] const char* keep (std::string text)
    {
        m_output.strings.push_back (std::move (text));
        return m_output.strings.back().c_str();
    }

    void addParam (const char* kind, const char* label, double init, double min, double max, double step)
    {
        const auto path = buildPath (label);

        ParamInfo info;
        info.id = keep (path);
        info.name = keep (label);
        info.minValue = static_cast<float> (min);
        info.maxValue = static_cast<float> (std::max (min, max));
        info.defaultValue = static_cast<float> (std::clamp (init, min, std::max (min, max)));

        if (const auto unit = m_pending.find ("unit"); unit != m_pending.end())
            info.unit = keep (unit->second);

        if (const auto scale = m_pending.find ("scale");
            scale != m_pending.end() && scale->second == "log" && min > 0.0)
            info.midValue = static_cast<float> (std::sqrt (min * max));

        const auto styleText = style().toStdString();

        if (std::string_view (kind) == "button" || std::string_view (kind) == "checkbox")
        {
            info.kind = ParamKind::toggle;
        }
        else if (const auto labels = choiceLabels (styleText);
                 ! labels.empty() && isWhole (min) && min >= 0.0 && min < 0.5 && isWhole (max)
                 && static_cast<size_t> (std::lround (max)) + 1 == labels.size())
        {
            auto& list = m_output.choices.emplace_back();

            for (const auto& choice : labels)
                list.push_back (keep (choice));

            info.kind = ParamKind::choice;
            info.choices = list;
        }
        else if (step >= 1.0 && isWhole (step) && isWhole (min) && isWhole (max))
        {
            info.kind = ParamKind::integer;
        }

        add (makeObject (
            { { "kind", kind }, { "label", label }, { "path", juce::String (path) }, { "style", style() } }));
        m_output.params.push_back (info);
        m_pending.clear();
    }

    void addMeter (const char* kind, const char* label, double min, double max)
    {
        const auto unit = m_pending.find ("unit");
        add (makeObject ({ { "kind", kind },
                           { "label", label },
                           { "path", juce::String (buildPath (label)) },
                           { "style", style() },
                           { "meter", static_cast<int> (m_output.meters++) },
                           { "min", min },
                           { "max", max },
                           { "unit", unit != m_pending.end() ? juce::String (unit->second) : juce::String() } }));
        m_pending.clear();
    }

    Output& m_output;
    std::vector<juce::var> m_groups;
    std::map<std::string, std::string> m_pending;
};

// Collects an instance's zones in the order DescriptorBuilder saw the controls.
class ZoneCollector final : public UI
{
public:
    std::vector<FAUSTFLOAT*> params;
    std::vector<FAUSTFLOAT*> meters;

    void openTabBox (const char* /*label*/) override {}
    void openHorizontalBox (const char* /*label*/) override {}
    void openVerticalBox (const char* /*label*/) override {}
    void closeBox() override {}
    void addButton (const char* /*label*/, FAUSTFLOAT* zone) override { params.push_back (zone); }
    void addCheckButton (const char* /*label*/, FAUSTFLOAT* zone) override { params.push_back (zone); }

    void addVerticalSlider (const char* /*label*/, FAUSTFLOAT* zone, FAUSTFLOAT /*init*/, FAUSTFLOAT /*min*/,
                            FAUSTFLOAT /*max*/, FAUSTFLOAT /*step*/) override
    {
        params.push_back (zone);
    }

    void addHorizontalSlider (const char* /*label*/, FAUSTFLOAT* zone, FAUSTFLOAT /*init*/, FAUSTFLOAT /*min*/,
                              FAUSTFLOAT /*max*/, FAUSTFLOAT /*step*/) override
    {
        params.push_back (zone);
    }

    void addNumEntry (const char* /*label*/, FAUSTFLOAT* zone, FAUSTFLOAT /*init*/, FAUSTFLOAT /*min*/,
                      FAUSTFLOAT /*max*/, FAUSTFLOAT /*step*/) override
    {
        params.push_back (zone);
    }

    void addHorizontalBargraph (const char* /*label*/, FAUSTFLOAT* zone, FAUSTFLOAT /*min*/,
                                FAUSTFLOAT /*max*/) override
    {
        meters.push_back (zone);
    }
    void addVerticalBargraph (const char* /*label*/, FAUSTFLOAT* zone, FAUSTFLOAT /*min*/, FAUSTFLOAT /*max*/) override
    {
        meters.push_back (zone);
    }
    void addSoundfile (const char* /*label*/, const char* /*url*/, Soundfile** /*soundfile*/) override {}
};

[[nodiscard]] std::vector<PortInfo> portsFor (int channels, const char* single, const char* prefix,
                                              std::deque<std::string>& strings)
{
    const auto count = (channels + 1) / 2;
    std::vector<PortInfo> ports;

    // Channels go to ports in pairs; an odd last channel is a mono port (docs/design/mono-ports.md, MP5).
    for (int i = 0; i < count; ++i)
    {
        strings.push_back (count == 1 ? std::string (single) : prefix + std::to_string (i + 1));
        ports.push_back ({ PortType::audio, strings.back().c_str(), 2 * i + 1 == channels ? 1 : 2 });
    }

    return ports;
}
// Returns one channel of a Faust DSP's output port after rendering, or nullptr if it has no such port.
[[nodiscard]] const float* outputChannel (const std::vector<std::vector<float>>& outputs, size_t port,
                                          size_t ch) noexcept
{
    const auto first = port * 2;

    if (first >= outputs.size())
        return nullptr;

    return outputs[std::min (first + ch, outputs.size() - 1)].data();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// FaustProgram

// Scripts compile to native code with libfaust's LLVM backend when the build has it (cmake/Faust.cmake), otherwise to
// bytecode for its interpreter (docs/design/phase11-faust-panels.md, F1).
namespace
{
#if DAWG_FAUST_LLVM
using FaustFactory = llvm_dsp_factory;

[[nodiscard]] FaustFactory* createFaustFactory (const std::string& code, std::span<const char*> argv,
                                                std::string& error)
{
    constexpr int optimise = -1; // the backend's highest optimisation level
    return createDSPFactoryFromString ("faust", code, static_cast<int> (argv.size()), argv.data(), "", error, optimise);
}

void deleteFaustFactory (FaustFactory* factory)
{
    deleteDSPFactory (factory);
}
#else
using FaustFactory = interpreter_dsp_factory;

[[nodiscard]] FaustFactory* createFaustFactory (const std::string& code, std::span<const char*> argv,
                                                std::string& error)
{
    return createInterpreterDSPFactoryFromString ("faust", code, static_cast<int> (argv.size()), argv.data(), error);
}

void deleteFaustFactory (FaustFactory* factory)
{
    deleteInterpreterDSPFactory (factory);
}
#endif
} // namespace

struct FaustProgram::Impl
{
    FaustFactory* factory = nullptr;
    int inputs = 0;
    int outputs = 0;
    DescriptorBuilder::Output controls;
    std::deque<std::string> portNames;
    std::vector<PortInfo> inputPorts;
    std::vector<PortInfo> outputPorts;
    NodeDescriptor descriptor;

    Impl() = default;
    Impl (const Impl&) = delete;
    Impl& operator= (const Impl&) = delete;
    Impl (Impl&&) = delete;
    Impl& operator= (Impl&&) = delete;

    ~Impl()
    {
        const std::scoped_lock lock (faustMutex);
        deleteFaustFactory (factory);
    }

    // Creates an instance (deleted with delete before the factory).
    [[nodiscard]] std::unique_ptr<::dsp> createInstance() const
    {
        const std::scoped_lock lock (faustMutex);
        return std::unique_ptr<::dsp> (factory->createDSPInstance());
    }
};

FaustProgram::FaustProgram (std::unique_ptr<Impl> impl)
    : m_impl (std::move (impl))
{
}

FaustProgram::~FaustProgram() = default;

std::shared_ptr<const FaustProgram> FaustProgram::compile (const juce::String& code, const juce::File& libraries,
                                                           juce::String& error)
{
    auto impl = std::make_unique<Impl>();
    std::unique_ptr<::dsp> probe;

    {
        const std::scoped_lock lock (faustMutex);
        const auto libraryPath = libraries.getFullPathName().toStdString();
        std::array<const char*, 2> argv { "-I", libraryPath.c_str() };
        std::string message;

        impl->factory = createFaustFactory (code.toStdString(), argv, message);

        if (impl->factory == nullptr)
        {
            error = juce::String (message).trim();

            if (error.isEmpty())
                error = "The script did not compile";

            return nullptr;
        }

        probe.reset (impl->factory->createDSPInstance());
    }

    if (probe == nullptr)
    {
        error = "The script compiled but could not be instantiated";
        return nullptr;
    }

    impl->inputs = probe->getNumInputs();
    impl->outputs = probe->getNumOutputs();

    if (impl->inputs > maxChannels || impl->outputs > maxChannels)
    {
        error = "Scripts may have at most " + juce::String (maxChannels) + " inputs and outputs";
        return nullptr;
    }

    DescriptorBuilder builder (impl->controls);
    probe->buildUserInterface (&builder);
    impl->controls.layout = builder.root();
    probe.reset();

    impl->inputPorts = portsFor (impl->inputs, "In", "In ", impl->portNames);
    impl->inputPorts.insert (impl->inputPorts.end(), modulationInputPorts.begin(), modulationInputPorts.end());
    impl->outputPorts = portsFor (impl->outputs, "Out", "Out ", impl->portNames);
    impl->descriptor = NodeDescriptor { nodetype::faust,       "Faust", impl->inputPorts,    impl->outputPorts,
                                        impl->controls.params, true,    NodeCategory::effect };

    return std::shared_ptr<const FaustProgram> (new FaustProgram (std::move (impl))); // private constructor
}

const NodeDescriptor& FaustProgram::descriptor() const noexcept
{
    return m_impl->descriptor;
}

int FaustProgram::numInputs() const noexcept
{
    return m_impl->inputs;
}

int FaustProgram::numOutputs() const noexcept
{
    return m_impl->outputs;
}

const juce::var& FaustProgram::layout() const noexcept
{
    return m_impl->controls.layout;
}

size_t FaustProgram::numMeters() const noexcept
{
    return m_impl->controls.meters;
}

juce::var FaustProgram::toVar() const
{
    auto result = dawg::toVar (descriptor());

    if (auto* object = result.getDynamicObject())
    {
        object->setProperty ("channels", makeObject ({ { "inputs", numInputs() }, { "outputs", numOutputs() } }));
        object->setProperty ("layout", layout());
    }

    return result;
}

// ---------------------------------------------------------------------------------------------------------
// FaustCompiler

FaustCompiler::FaustCompiler()
    : m_libraries (juce::File (DAWG_FAUST_LIBRARIES_DIR))
{
}

FaustCompiler::~FaustCompiler()
{
    {
        const std::scoped_lock lock (m_mutex);
        m_quit = true;
    }

    m_wake.notify_all();

    if (m_thread.joinable())
        m_thread.join();
}

FaustCompiler& FaustCompiler::shared()
{
    // Never destroyed: at exit, libfaust's own globals may already be gone (macOS destroys them first), and deleting
    // the cached factories then fails on their lock. The process ends anyway.
    static auto* const compiler = new FaustCompiler(); // NOLINT(cppcoreguidelines-owning-memory)
    return *compiler;
}

void FaustCompiler::setLibraryDirectory (const juce::File& directory)
{
    const std::scoped_lock lock (m_mutex);
    m_libraries = directory;
    std::erase_if (m_entries, [] (const auto& entry) { return entry.second.error.isNotEmpty(); });
}

juce::File FaustCompiler::libraryDirectory() const
{
    const std::scoped_lock lock (m_mutex);
    return m_libraries;
}

void FaustCompiler::setBackground (bool background)
{
    const std::scoped_lock lock (m_mutex);
    m_background = background;

    if (background && ! m_thread.joinable())
        m_thread = std::thread ([this] { run(); });
}

void FaustCompiler::setListener (std::function<void()> listener)
{
    const std::scoped_lock lock (m_mutex);
    m_listener = std::move (listener);
}

FaustCompiler::Result FaustCompiler::request (const juce::String& code)
{
    std::unique_lock lock (m_mutex);
    auto key = code.toStdString();

    if (const auto it = m_entries.find (key); it != m_entries.end())
    {
        it->second.lastUse = ++m_useCounter;
        return { it->second.program, it->second.error, it->second.pending };
    }

    m_entries[key] = Entry { nullptr, {}, true, ++m_useCounter };

    if (m_background)
    {
        m_queue.push_back (std::move (key));
        lock.unlock();
        m_wake.notify_one();
        return { nullptr, {}, true };
    }

    const auto libraries = m_libraries;
    lock.unlock();

    juce::String error;
    auto program = FaustProgram::compile (code, libraries, error);
    store (key, program, error);
    return { std::move (program), error, false };
}

FaustCompiler::Result FaustCompiler::find (const juce::String& code) const
{
    const std::scoped_lock lock (m_mutex);

    if (const auto it = m_entries.find (code.toStdString()); it != m_entries.end())
        return { it->second.program, it->second.error, it->second.pending };

    return {};
}

void FaustCompiler::run()
{
    for (;;)
    {
        std::string code;
        juce::File libraries;

        {
            std::unique_lock lock (m_mutex);
            m_wake.wait (lock, [this] { return m_quit || ! m_queue.empty(); });

            if (m_quit)
                return;

            code = std::move (m_queue.front());
            m_queue.erase (m_queue.begin());
            libraries = m_libraries;
        }

        juce::String error;
        auto program = FaustProgram::compile (juce::String (code), libraries, error);
        store (code, std::move (program), error);

        std::function<void()> listener;

        {
            const std::scoped_lock lock (m_mutex);
            listener = m_listener;
        }

        if (listener != nullptr)
            listener();
    }
}

void FaustCompiler::store (const std::string& code, std::shared_ptr<const FaustProgram> program,
                           const juce::String& error)
{
    const std::scoped_lock lock (m_mutex);
    auto& entry = m_entries[code];
    entry.program = std::move (program);
    entry.error = error;
    entry.pending = false;
    entry.lastUse = ++m_useCounter;
    evict();
}

void FaustCompiler::evict()
{
    // Programs nodes still run (use count above 1) and pending entries stay.
    while (m_entries.size() > cacheSize)
    {
        auto oldest = m_entries.end();

        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (! it->second.pending && (it->second.program == nullptr || it->second.program.use_count() == 1)
                && (oldest == m_entries.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;

        if (oldest == m_entries.end())
            return;

        m_entries.erase (oldest);
    }
}

std::shared_ptr<const FaustProgram> compiledFaustProgram (const juce::String& code)
{
    return FaustCompiler::shared().find (code).program;
}

const NodeDescriptor& faustDescriptor() noexcept
{
    return emptyDescriptor;
}

// ---------------------------------------------------------------------------------------------------------
// FaustNode

struct FaustNode::Dsp
{
    std::unique_ptr<::dsp> instance;
    std::vector<FAUSTFLOAT*> zones;
    std::vector<FAUSTFLOAT*> meterZones;
    std::unique_ptr<std::atomic<float>[]> meters; // NOLINT(modernize-avoid-c-arrays): atomics cannot live in a vector
    std::vector<std::vector<float>> inputs;       // mono inputs mixed down, and silence
    std::vector<std::vector<float>> outputs;
    std::vector<float*> inputPointers;
    std::vector<float*> outputPointers;
    std::vector<float> silence;
};

FaustNode::FaustNode (std::shared_ptr<const FaustProgram> program, std::shared_ptr<FaustNode> predecessor)
    : Node (program != nullptr ? program->descriptor() : faustDescriptor()),
      m_program (std::move (program)),
      m_predecessor (std::move (predecessor))
{
    if (m_predecessor != nullptr)
        m_predecessor->releaseFinishedFade();

    if (m_program == nullptr)
        return;

    m_dsp = std::make_unique<Dsp>();
    m_dsp->instance = m_program->impl().createInstance();

    if (m_dsp->instance == nullptr)
    {
        m_dsp = nullptr;
        return;
    }

    ZoneCollector zones;
    m_dsp->instance->buildUserInterface (&zones);
    m_dsp->zones = std::move (zones.params);
    m_dsp->meterZones = std::move (zones.meters);
    m_dsp->meters =
        std::make_unique<std::atomic<float>[]> (m_dsp->meterZones.size()); // NOLINT(modernize-avoid-c-arrays)

    for (size_t i = 0; i < m_dsp->meterZones.size(); ++i)
        m_dsp->meters[i].store (0.0f);
}

FaustNode::~FaustNode()
{
    if (m_dsp != nullptr)
    {
        const std::scoped_lock lock (faustMutex);
        m_dsp->instance = nullptr;
    }
}

float FaustNode::meterValue (size_t index) const noexcept
{
    return m_dsp != nullptr && index < m_dsp->meterZones.size() ? m_dsp->meters[index].load() : 0.0f;
}

bool FaustNode::releaseFinishedFade()
{
    if (m_predecessor == nullptr || m_fadeFrom.load() != nullptr)
        return false;

    m_predecessor = nullptr;
    return true;
}

void FaustNode::prepare (const ProcessSpec& spec)
{
    m_spec = spec;

    if (m_dsp != nullptr)
    {
        const auto block = static_cast<size_t> (spec.maxBlockSize);
        m_dsp->instance->init (static_cast<int> (spec.sampleRate));
        m_dsp->inputs.assign (static_cast<size_t> (m_program->numInputs()), std::vector<float> (block));
        m_dsp->outputs.assign (static_cast<size_t> (m_program->numOutputs()), std::vector<float> (block));
        m_dsp->inputPointers.assign (m_dsp->inputs.size(), nullptr);
        m_dsp->outputPointers.clear();

        for (auto& channel : m_dsp->outputs)
            m_dsp->outputPointers.push_back (channel.data());

        m_dsp->silence.assign (block, 0.0f);
    }

    // Fade from the replaced node only if it runs at the same settings (it was prepared and not re-created for a
    // device change).
    const auto canFade = m_predecessor != nullptr && m_predecessor->m_spec == spec;
    m_fadeLength = std::max (1, static_cast<int> (spec.sampleRate * crossfadeSeconds));
    m_fadePosition = 0;
    m_fadeFrom.store (canFade ? m_predecessor.get() : nullptr);
}

void FaustNode::reset() noexcept
{
    if (m_dsp != nullptr)
        m_dsp->instance->instanceClear();

    m_fadeFrom.store (nullptr);
}

void FaustNode::configure (const GraphNode& node)
{
    m_bypassed.store (node.bypassed);
}

void FaustNode::passThrough (const ProcessContext& context) const noexcept
{
    for (size_t port = 0; port < context.audioOutputs.size(); ++port)
        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto* out = context.audioOutputs[port][ch];

            if (port == 0 && ! context.audioInputs.empty())
            {
                if (context.audioInputs[0][ch] != out)
                    juce::FloatVectorOperations::copy (out, context.audioInputs[0][ch], context.numSamples);
            }
            else
            {
                juce::FloatVectorOperations::clear (out, context.numSamples);
            }
        }

    // A stereo input through to a mono output is folded (MP2).
    const auto& ports = descriptor();

    if (! context.audioInputs.empty() && ! context.audioOutputs.empty() && ! ports.inputs.empty()
        && ! ports.outputs.empty() && ports.inputs[0].channels == 2 && ports.outputs[0].channels == 1)
    {
        auto* out = context.audioOutputs[0][0];
        juce::FloatVectorOperations::add (out, context.audioInputs[0][1], context.numSamples);
        juce::FloatVectorOperations::multiply (out, 0.5f, context.numSamples);
    }
}

void FaustNode::render (const ProcessContext& context) noexcept
{
    auto& dsp = *m_dsp;
    const auto n = context.numSamples;

    for (size_t i = 0; i < dsp.zones.size(); ++i)
        *dsp.zones[i] = params().get (i);

    // Channel pairs come from stereo ports; an odd last channel is a mono port, which the compiler feeds the fold of
    // a stereo source (MP5): its view has the one channel on both sides.
    const auto inputs = dsp.inputs.size();

    for (size_t c = 0; c < inputs; ++c)
    {
        const auto port = c / 2;

        if (port >= context.audioInputs.size())
        {
            dsp.inputPointers[c] = dsp.silence.data();
            continue;
        }

        const auto& in = context.audioInputs[port];
        dsp.inputPointers[c] =
            const_cast<float*> (in[c % 2]); // NOLINT(cppcoreguidelines-pro-type-const-cast): Faust only reads inputs
    }

    dsp.instance->compute (n, dsp.inputPointers.data(), dsp.outputPointers.data());

    for (size_t i = 0; i < dsp.meterZones.size(); ++i)
        dsp.meters[i].store (*dsp.meterZones[i]);
}

void FaustNode::mixFade (const ProcessContext& context) noexcept
{
    auto* old = m_fadeFrom.load();
    const auto n = context.numSamples;

    if (old->m_dsp != nullptr && ! old->m_bypassed.load())
        old->render (context);

    const auto start = m_fadePosition;

    for (size_t port = 0; port < context.audioOutputs.size(); ++port)
        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto* out = context.audioOutputs[port][ch];
            const float* from = nullptr;

            if (old->m_dsp != nullptr && ! old->m_bypassed.load())
                from = outputChannel (old->m_dsp->outputs, port, ch);
            else if (port < context.audioInputs.size())
                from = context.audioInputs[port][ch];

            for (int i = 0; i < n; ++i)
            {
                const auto g = std::min (1.0f, static_cast<float> (start + i) / static_cast<float> (m_fadeLength));
                out[i] = out[i] * g + (from != nullptr ? from[i] * (1.0f - g) : 0.0f);
            }
        }

    m_fadePosition += n;

    if (m_fadePosition >= m_fadeLength)
        m_fadeFrom.store (nullptr);
}

void FaustNode::process (const ProcessContext& context) noexcept
{
    if (m_dsp == nullptr || m_bypassed.load())
    {
        passThrough (context);
        return;
    }

    render (context);

    for (size_t port = 0; port < context.audioOutputs.size(); ++port)
        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto* out = context.audioOutputs[port][ch];

            if (const auto* channel = outputChannel (m_dsp->outputs, port, ch))
                juce::FloatVectorOperations::copy (out, channel, context.numSamples);
            else
                juce::FloatVectorOperations::clear (out, context.numSamples);
        }

    if (m_fadeFrom.load() != nullptr)
        mixFade (context);
}

} // namespace dawg
