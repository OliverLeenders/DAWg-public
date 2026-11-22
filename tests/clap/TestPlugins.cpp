#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <clap/clap.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string_view>
#include <thread>
#include <tuple>

// Two small CLAP plugins for the host's tests (docs/design/clap-hosting.md, section 7): a sine instrument and a gain
// effect with a parameter, a state and some latency. The state reports what the plugin has received, so tests can read it.

namespace
{
constexpr clap_id gainParameter = 7;
constexpr clap_id extraParameter = 8; // there once a state with "extra=1" was set
constexpr uint32_t reportedLatency = 64;

// CLAP takes these lists as C arrays of strings, ended by a null pointer.
const std::array<const char*, 4> instrumentFeatures { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };
const std::array<const char*, 3> effectFeatures { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                                  nullptr };

const clap_plugin_descriptor_t instrumentDescriptor {
    CLAP_VERSION, "org.dawg.test.sine", "Test Sine", "DAWg", "", "", "", "1.0", "", instrumentFeatures.data()
};
const clap_plugin_descriptor_t effectDescriptor {
    CLAP_VERSION, "org.dawg.test.gain", "Test Gain", "DAWg", "", "", "", "1.0", "", effectFeatures.data()
};

struct Plugin
{
    clap_plugin_t plugin {};
    const clap_host_t* host = nullptr;
    bool instrument = false;
    double sampleRate = 48000.0;
    double gain = 1.0;
    double phase = 0.0;
    int key = -1;
    int noteId = -1;
    double tuning = 0.0;
    double pressure = 0.0;
    double modulation = 0.0;
    int expressions = 0;
    int mods = 0;
    int noteMods = 0;
    unsigned noteModIds = 0;
    int renderMode = 0;
    bool voiceHasAmount = false;
    int chokes = 0;
    bool noteEndDue = false;
    int noteEndId = -1;
    int noteOns = 0;
    int noteOffs = 0;
    int midiEvents = 0;
    int processCalls = 0;
    bool processing = false;
    bool wrongThread = false;
    bool hasExtra = false;
    clap_id initTimer = CLAP_INVALID_ID; // registered in init(), as Surge XT's editor does
};

Plugin& self (const clap_plugin_t* plugin)
{
    return *static_cast<Plugin*> (plugin->plugin_data);
}

// Some keys misbehave, for the tests of plugins in other processes.
constexpr int crashKey = 1;
constexpr int hangKey = 2;
constexpr int slowKey = 3;

void misbehave (int key)
{
    if (key == crashKey)
        *static_cast<volatile int*> (nullptr) = 1;
    else if (key == hangKey)
        std::this_thread::sleep_for (std::chrono::minutes (5));
    else if (key == slowKey)
        std::this_thread::sleep_for (std::chrono::milliseconds (60));
}

void handleEvent (Plugin& p, const clap_event_header_t& header)
{
    if (header.space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;

    switch (header.type)
    {
        case CLAP_EVENT_NOTE_ON:
            p.key = reinterpret_cast<const clap_event_note_t&> (header).key;
            p.noteId = reinterpret_cast<const clap_event_note_t&> (header).note_id;
            p.tuning = 0.0;
            ++p.noteOns;
            misbehave (p.key);
            break;
        case CLAP_EVENT_NOTE_OFF:
            p.key = -1;
            p.noteEndDue = true;
            p.noteEndId = reinterpret_cast<const clap_event_note_t&> (header).note_id;
            ++p.noteOffs;
            break;
        case CLAP_EVENT_NOTE_CHOKE:
            p.key = -1;
            ++p.chokes;
            break;
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            const auto& expression = reinterpret_cast<const clap_event_note_expression_t&> (header);
            ++p.expressions;

            if (expression.note_id == p.noteId && expression.expression_id == CLAP_NOTE_EXPRESSION_TUNING)
                p.tuning = expression.value;
            else if (expression.note_id == p.noteId && expression.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE)
                p.pressure = expression.value;

            break;
        }
        case CLAP_EVENT_PARAM_MOD:
        {
            const auto& mod = reinterpret_cast<const clap_event_param_mod_t&> (header);
            ++p.mods;

            if (mod.param_id != gainParameter)
                break;

            if (mod.note_id >= 0)
                p.noteModIds |= 1u << (static_cast<unsigned> (mod.note_id) % 32u);

            // A voice heeds only its own amount, once it has one.
            if (mod.note_id < 0 && ! p.voiceHasAmount)
            {
                p.modulation = mod.amount;
            }
            else if (mod.note_id >= 0 && mod.note_id == p.noteId)
            {
                p.voiceHasAmount = true;
                p.modulation = mod.amount;
                ++p.noteMods;
            }

            break;
        }
        case CLAP_EVENT_MIDI:
        {
            const auto& midi = reinterpret_cast<const clap_event_midi_t&> (header);
            const auto kind = midi.data[0] & 0xf0;
            ++p.midiEvents;

            if (kind == 0x90 && midi.data[2] > 0)
            {
                p.key = midi.data[1];
                ++p.noteOns;
            }
            else if (kind == 0x80 || kind == 0x90)
            {
                p.key = -1;
                ++p.noteOffs;
            }

            break;
        }
        case CLAP_EVENT_PARAM_VALUE:
        {
            const auto& value = reinterpret_cast<const clap_event_param_value_t&> (header);

            if (value.param_id == gainParameter)
                p.gain = value.value;

            break;
        }
        default:
            break;
    }
}

const clap_plugin_audio_ports_t audioPorts {
    [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t
    {
        if (self (plugin).instrument)
            return isInput ? 0 : 1;

        return isInput ? 2 : 1;
    },
    [] (const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) -> bool
    {
        if (index > (isInput ? 1u : 0u))
            return false;

        *info = {};
        info->id = isInput ? 1 + index * 10 : 2;
        std::snprintf (info->name, sizeof (info->name), index == 0 ? "Main" : "Key");
        info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};

const clap_plugin_note_ports_t notePorts {
    [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return isInput && self (plugin).instrument ? 1 : 0; },
    [] (const clap_plugin_t*, uint32_t index, bool, clap_note_port_info_t* info) -> bool
    {
        if (index != 0)
            return false;

        *info = {};
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf (info->name, sizeof (info->name), "Notes");
        return true;
    },
};

const clap_plugin_params_t params {
    [] (const clap_plugin_t* plugin) -> uint32_t { return self (plugin).hasExtra ? 2 : 1; },
    [] (const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info) -> bool
    {
        if (index == 1 && self (plugin).hasExtra)
        {
            *info = {};
            info->id = extraParameter;
            info->flags = CLAP_PARAM_IS_AUTOMATABLE;
            std::snprintf (info->name, sizeof (info->name), "Extra");
            info->max_value = 1.0;
            info->default_value = 0.5;
            return true;
        }

        if (index != 0)
            return false;

        *info = {};
        info->id = gainParameter;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE | CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID;
        std::snprintf (info->name, sizeof (info->name), "Gain");
        info->min_value = 0.0;
        info->max_value = 2.0;
        info->default_value = 1.0;
        return true;
    },
    [] (const clap_plugin_t* plugin, clap_id id, double* value) -> bool
    {
        if (id == extraParameter && self (plugin).hasExtra)
        {
            *value = 0.5;
            return true;
        }

        if (id != gainParameter)
            return false;

        *value = self (plugin).gain;
        return true;
    },
    [] (const clap_plugin_t*, clap_id, double value, char* out, uint32_t size) -> bool
    {
        std::snprintf (out, size, "%.2f x", value);
        return true;
    },
    [] (const clap_plugin_t*, clap_id, const char* text, double* value) -> bool
    {
        char* end = nullptr;
        *value = std::strtod (text, &end);
        return end != text;
    },
    [] (const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*)
    {
        for (uint32_t i = 0; i < in->size (in); ++i)
            handleEvent (self (plugin), *in->get (in, i));
    },
};

const clap_plugin_render_t render {
    [] (const clap_plugin_t*) -> bool { return false; },
    [] (const clap_plugin_t* plugin, clap_plugin_render_mode mode) -> bool
    {
        self (plugin).renderMode = mode;
        return true;
    },
};

const clap_plugin_latency_t latency { [] (const clap_plugin_t*) -> uint32_t { return reportedLatency; } };

// The state, with why it was saved: 0 without a state context, else the context's type.
bool saveState (const clap_plugin_t* plugin, const clap_ostream_t* stream, int why)
{
    const auto& p = self (plugin);
    std::array<char, 192> text {};
    const auto length =
        std::snprintf (text.data(), text.size(),
                       "gain=%.3f on=%d off=%d midi=%d calls=%d wrong=%d expr=%d mods=%d chokes=%d notemods=%d "
                       "modids=%d render=%d extra=%d why=%d inittimer=%d",
                       p.gain, p.noteOns, p.noteOffs, p.midiEvents, p.processCalls, p.wrongThread ? 1 : 0,
                       p.expressions, p.mods, p.chokes, p.noteMods, std::popcount (p.noteModIds), p.renderMode,
                       p.hasExtra ? 1 : 0, why, p.initTimer != CLAP_INVALID_ID ? 1 : 0);
    return stream->write (stream, text.data(), static_cast<uint64_t> (length)) == length;
}

bool loadState (const clap_plugin_t* plugin, const clap_istream_t* stream);

const clap_plugin_state_t state {
    [] (const clap_plugin_t* plugin, const clap_ostream_t* stream) -> bool { return saveState (plugin, stream, 0); },
    loadState,
};

const clap_plugin_state_context_t stateContext {
    [] (const clap_plugin_t* plugin, const clap_ostream_t* stream, uint32_t type) -> bool
    { return saveState (plugin, stream, static_cast<int> (type)); },
    [] (const clap_plugin_t* plugin, const clap_istream_t* stream, uint32_t) -> bool
    { return loadState (plugin, stream); },
};

bool loadState (const clap_plugin_t* plugin, const clap_istream_t* stream)
{
    std::array<char, 192> text {};
    const auto read = stream->read (stream, text.data(), text.size() - 1);

    if (read <= 0)
        return false;

    constexpr std::string_view prefix { "gain=" };

    if (std::string_view (text.data()).substr (0, prefix.size()) != prefix)
        return false;

    const auto* number = text.data() + prefix.size();
    char* end = nullptr;
    self (plugin).gain = std::strtod (number, &end);
    auto& p = self (plugin);

    // A state with "extra=1" adds a parameter, which the plugin says with params.rescan (CLAP_PARAM_RESCAN_ALL).
    if (std::strstr (text.data(), "extra=1") != nullptr && ! p.hasExtra)
    {
        p.hasExtra = true;

        if (const auto* hostParams =
                static_cast<const clap_host_params_t*> (p.host->get_extension (p.host, CLAP_EXT_PARAMS)))
            hostParams->rescan (p.host, CLAP_PARAM_RESCAN_ALL);
    }

    return end != number;
}

clap_process_status process (const clap_plugin_t* plugin, const clap_process_t* data)
{
    auto& p = self (plugin);
    ++p.processCalls;

    const auto* check =
        static_cast<const clap_host_thread_check_t*> (p.host->get_extension (p.host, CLAP_EXT_THREAD_CHECK));

    if (check == nullptr || ! check->is_audio_thread (p.host) || ! p.processing)
        p.wrongThread = true;

    const auto eventCount = data->in_events->size (data->in_events);
    uint32_t next = 0;

    for (uint32_t s = 0; s < data->frames_count; ++s)
    {
        while (next < eventCount)
        {
            const auto* event = data->in_events->get (data->in_events, next);

            if (event->time > s)
                break;

            handleEvent (p, *event);
            ++next;
        }

        for (uint32_t ch = 0; ch < 2; ++ch)
        {
            auto value = 0.0f;

            if (! p.instrument)
                value = data->audio_inputs[0].data32[ch][s] + data->audio_inputs[1].data32[ch][s];
            else if (p.key >= 0)
                value = 0.25f * static_cast<float> (std::sin (p.phase));

            data->audio_outputs[0].data32[ch][s] = value * static_cast<float> (p.gain + p.modulation);
        }

        if (p.key >= 0)
        {
            const auto hertz = 440.0 * std::pow (2.0, (p.key + p.tuning - 69) / 12.0);
            p.phase += 2.0 * std::numbers::pi * hertz / p.sampleRate;
        }
    }

    // The release is instant here, so the voice is over at once.
    if (p.noteEndDue)
    {
        p.noteEndDue = false;
        clap_event_note_t end {};
        end.header.size = sizeof (end);
        end.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        end.header.type = CLAP_EVENT_NOTE_END;
        end.note_id = p.noteEndId;
        data->out_events->try_push (data->out_events, &end.header);
    }

    return CLAP_PROCESS_CONTINUE;
}

const clap_plugin_t* create (const clap_host_t* host, const clap_plugin_descriptor_t* descriptor, bool instrument)
{
    auto* p = new Plugin(); // NOLINT(cppcoreguidelines-owning-memory): the host owns it, through destroy below
    p->host = host;
    p->instrument = instrument;
    p->plugin.desc = descriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = [] (const clap_plugin_t* plugin) -> bool
    {
        auto& self = *static_cast<Plugin*> (plugin->plugin_data);

        if (const auto* timers = static_cast<const clap_host_timer_support_t*> (
                self.host->get_extension (self.host, CLAP_EXT_TIMER_SUPPORT)))
            std::ignore = timers->register_timer (self.host, 1000, &self.initTimer);

        return true;
    };
    p->plugin.destroy = [] (const clap_plugin_t* plugin)
    { delete &self (plugin); }; // NOLINT(cppcoreguidelines-owning-memory)
    p->plugin.activate = [] (const clap_plugin_t* plugin, double sampleRate, uint32_t, uint32_t) -> bool
    {
        self (plugin).sampleRate = sampleRate;
        return true;
    };
    p->plugin.deactivate = [] (const clap_plugin_t*) {};
    p->plugin.start_processing = [] (const clap_plugin_t* plugin) -> bool
    {
        self (plugin).processing = true;
        return true;
    };
    p->plugin.stop_processing = [] (const clap_plugin_t* plugin) { self (plugin).processing = false; };
    p->plugin.reset = [] (const clap_plugin_t*) {};
    p->plugin.process = process;
    p->plugin.get_extension = [] (const clap_plugin_t*, const char* id) -> const void*
    {
        if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &audioPorts;
        if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0)
            return &notePorts;
        if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)
            return &params;
        if (std::strcmp (id, CLAP_EXT_LATENCY) == 0)
            return &latency;
        if (std::strcmp (id, CLAP_EXT_RENDER) == 0)
            return &render;
        if (std::strcmp (id, CLAP_EXT_STATE) == 0)
            return &state;
        if (std::strcmp (id, CLAP_EXT_STATE_CONTEXT) == 0)
            return &stateContext;
        return nullptr;
    };
    p->plugin.on_main_thread = [] (const clap_plugin_t*) {};
    return &p->plugin;
}

const clap_plugin_factory_t factory {
    [] (const clap_plugin_factory_t*) -> uint32_t { return 2; },
    [] (const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t*
    {
        if (index == 0)
            return &instrumentDescriptor;

        return index == 1 ? &effectDescriptor : nullptr;
    },
    [] (const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t*
    {
        if (std::strcmp (id, instrumentDescriptor.id) == 0)
            return create (host, &instrumentDescriptor, true);
        if (std::strcmp (id, effectDescriptor.id) == 0)
            return create (host, &effectDescriptor, false);
        return nullptr;
    },
};
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    [] (const char*) -> bool { return true; },
    [] {},
    [] (const char* id) -> const void* { return std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; },
};
