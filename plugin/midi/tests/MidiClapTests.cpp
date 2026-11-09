// SPDX-License-Identifier: AGPL-3.0-or-later
// The built Sedef MIDI CLAP, hosted the way a validator's state tests host it: parameter values
// set through the plug-in's own event paths (a processed block, and a flush), the state saved, a
// second instance loading it. The second instance must ask the host to re-read its values, end
// up with the same values, and save the same bytes again, also through short reads and writes.
#include <catch2/catch_test_macros.hpp>
#include <clap/clap.h>
#include <cstring>
#include <juce_core/juce_core.h>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace
{
/// A host with just what the state tests need: a main thread, an audio thread flag, and a record
/// of what the plug-in asked for.
struct Host
{
    clap_host host {};
    clap_host_thread_check threadCheck {};
    clap_host_params params {};
    clap_host_state state {};
    std::thread::id mainThread = std::this_thread::get_id();
    bool inAudio = false;
    bool callbackRequested = false;
    clap_param_rescan_flags rescans = 0;

    Host()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "sedef-midi-tests";
        host.vendor = "sedef";
        host.url = "";
        host.version = "1";
        host.get_extension = [] (const clap_host* h, const char* id) -> const void* {
            auto* self = static_cast<Host*> (h->host_data);
            if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0)
                return &self->threadCheck;
            if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)
                return &self->params;
            if (std::strcmp (id, CLAP_EXT_STATE) == 0)
                return &self->state;
            return nullptr;
        };
        host.request_restart = [] (const clap_host*) {
        };
        host.request_process = [] (const clap_host*) {
        };
        host.request_callback = [] (const clap_host* h) {
            static_cast<Host*> (h->host_data)->callbackRequested = true;
        };
        threadCheck.is_main_thread = [] (const clap_host* h) {
            const auto* self = static_cast<const Host*> (h->host_data);
            return !self->inAudio && std::this_thread::get_id() == self->mainThread;
        };
        threadCheck.is_audio_thread = [] (const clap_host* h) {
            return static_cast<const Host*> (h->host_data)->inAudio;
        };
        params.rescan = [] (const clap_host* h, clap_param_rescan_flags flags) {
            static_cast<Host*> (h->host_data)->rescans |= flags;
        };
        params.clear = [] (const clap_host*, clap_id, clap_param_clear_flags) {
        };
        params.request_flush = [] (const clap_host*) {
        };
        state.mark_dirty = [] (const clap_host*) {
        };
    }
};

struct Library
{
    juce::DynamicLibrary lib;
    const clap_plugin_entry* entry = nullptr;
    const clap_plugin_factory* factory = nullptr;

    Library()
    {
        const juce::File bundle (SEDEF_MIDI_CLAP_PATH);
#if JUCE_MAC
        const auto binary =
            bundle.getChildFile ("Contents/MacOS").findChildFiles (juce::File::findFiles, false)[0];
#else
        const auto binary = bundle;
#endif
        REQUIRE (lib.open (binary.getFullPathName()));
        entry = static_cast<const clap_plugin_entry*> (lib.getFunction ("clap_entry"));
        REQUIRE (entry != nullptr);
        REQUIRE (entry->init (bundle.getFullPathName().toRawUTF8()));
        factory =
            static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
        REQUIRE (factory != nullptr);
    }
    ~Library()
    {
        if (entry != nullptr)
            entry->deinit();
    }
};

struct Instance
{
    Host& host;
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;

    Instance (Library& library, Host& h) : host (h)
    {
        plugin = library.factory->create_plugin (library.factory, &host.host,
                                                 "io.github.mr-kaynak.sedef-midi");
        REQUIRE (plugin != nullptr);
        REQUIRE (plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (
            plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state =
            static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        REQUIRE (params != nullptr);
        REQUIRE (state != nullptr);
        poll();
    }
    ~Instance() { plugin->destroy (plugin); }

    /// Runs what the plug-in asked to run on the main thread, as a host's idle callback does.
    void poll()
    {
        while (host.callbackRequested)
        {
            host.callbackRequested = false;
            plugin->on_main_thread (plugin);
        }
    }

    std::vector<clap_param_info> infos() const
    {
        std::vector<clap_param_info> out (params->count (plugin));
        for (std::uint32_t i = 0; i < out.size(); ++i)
            REQUIRE (params->get_info (plugin, i, &out[i]));
        return out;
    }

    std::map<clap_id, double> values() const
    {
        std::map<clap_id, double> out;
        for (const auto& info : infos())
            REQUIRE (params->get_value (plugin, info.id, &out[info.id]));
        return out;
    }

    std::vector<std::uint8_t> save (std::size_t chunk = 0) const
    {
        struct Sink
        {
            std::vector<std::uint8_t> bytes;
            std::size_t chunk;
        } sink { {}, chunk };
        clap_ostream stream {};
        stream.ctx = &sink;
        stream.write = [] (const clap_ostream* s, const void* data, std::uint64_t size) {
            auto& out = *static_cast<Sink*> (s->ctx);
            const auto n = out.chunk > 0 ? std::min<std::uint64_t> (size, out.chunk) : size;
            const auto* p = static_cast<const std::uint8_t*> (data);
            out.bytes.insert (out.bytes.end(), p, p + n);
            return static_cast<std::int64_t> (n);
        };
        REQUIRE (state->save (plugin, &stream));
        return sink.bytes;
    }

    void load (const std::vector<std::uint8_t>& bytes, std::size_t chunk = 0) const
    {
        struct Source
        {
            const std::vector<std::uint8_t>& bytes;
            std::size_t at = 0, chunk;
        } source { bytes, 0, chunk };
        clap_istream stream {};
        stream.ctx = &source;
        stream.read = [] (const clap_istream* s, void* data, std::uint64_t size) {
            auto& in = *static_cast<Source*> (s->ctx);
            auto n = std::min<std::uint64_t> (size, in.bytes.size() - in.at);
            if (in.chunk > 0)
                n = std::min<std::uint64_t> (n, in.chunk);
            std::memcpy (data, in.bytes.data() + in.at, n);
            in.at += n;
            return static_cast<std::int64_t> (n);
        };
        REQUIRE (state->load (plugin, &stream));
    }
};

/// Input events from a list, output events dropped.
struct Events
{
    std::vector<clap_event_param_value> list;
    clap_input_events in {};
    clap_output_events out {};

    Events()
    {
        in.ctx = this;
        in.size = [] (const clap_input_events* e) {
            return static_cast<std::uint32_t> (static_cast<const Events*> (e->ctx)->list.size());
        };
        in.get = [] (const clap_input_events* e, std::uint32_t i) {
            return &static_cast<const Events*> (e->ctx)->list[i].header;
        };
        out.ctx = this;
        out.try_push = [] (const clap_output_events*, const clap_event_header*) {
            return true;
        };
    }
};

/// Random values for every writable parameter, as a validator's parameter fuzzer picks them.
Events randomValues (const Instance& instance, std::mt19937& random)
{
    Events events;
    for (const auto& info : instance.infos())
    {
        if ((info.flags & CLAP_PARAM_IS_READONLY) != 0)
            continue;
        std::uniform_real_distribution<double> uniform (info.min_value, info.max_value);
        auto value = uniform (random);
        if ((info.flags & CLAP_PARAM_IS_STEPPED) != 0)
            value = std::round (value);
        clap_event_param_value e {};
        e.header.size = sizeof (e);
        e.header.time = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = info.id;
        e.cookie = info.cookie;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = value;
        events.list.push_back (e);
    }
    return events;
}

/// One block of silence with the events, on the "audio thread", as a host's process call.
void processBlock (Instance& instance, Events& events)
{
    const auto* plugin = instance.plugin;
    REQUIRE (plugin->activate (plugin, 48000.0, 32, 256));

    const auto* ports = static_cast<const clap_plugin_audio_ports*> (
        plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
    REQUIRE (ports != nullptr);
    constexpr std::uint32_t kFrames = 256;
    std::vector<std::vector<float>> channels;
    std::vector<std::vector<float*>> pointers;
    std::vector<clap_audio_buffer> buffers[2];
    for (const bool isInput : { true, false })
        for (std::uint32_t i = 0; i < ports->count (plugin, isInput); ++i)
        {
            clap_audio_port_info info {};
            REQUIRE (ports->get (plugin, i, isInput, &info));
            auto& ptrs = pointers.emplace_back();
            for (std::uint32_t c = 0; c < info.channel_count; ++c)
                ptrs.push_back (channels.emplace_back (kFrames, 0.0f).data());
        }
    // Pointers are taken once every vector stopped growing.
    std::size_t next = 0;
    for (const bool isInput : { true, false })
        for (std::uint32_t i = 0; i < ports->count (plugin, isInput); ++i, ++next)
        {
            clap_audio_buffer b {};
            b.data32 = pointers[next].data();
            b.channel_count = static_cast<std::uint32_t> (pointers[next].size());
            buffers[isInput ? 0 : 1].push_back (b);
        }

    instance.host.inAudio = true;
    REQUIRE (plugin->start_processing (plugin));
    clap_process process {};
    process.steady_time = 0;
    process.frames_count = kFrames;
    process.audio_inputs = buffers[0].data();
    process.audio_inputs_count = static_cast<std::uint32_t> (buffers[0].size());
    process.audio_outputs = buffers[1].data();
    process.audio_outputs_count = static_cast<std::uint32_t> (buffers[1].size());
    process.in_events = &events.in;
    process.out_events = &events.out;
    CHECK (plugin->process (plugin, &process) != CLAP_PROCESS_ERROR);

    plugin->stop_processing (plugin);
    instance.host.inAudio = false;
    plugin->deactivate (plugin);
    instance.poll();
}

std::string describe (const Instance& instance, const std::map<clap_id, double>& a,
                      const std::map<clap_id, double>& b)
{
    std::string out;
    for (const auto& info : instance.infos())
        if (a.at (info.id) != b.at (info.id))
            out += std::string (info.name) + ": " + std::to_string (a.at (info.id)) + " vs " +
                   std::to_string (b.at (info.id)) + "\n";
    return out;
}
} // namespace

TEST_CASE ("The Sedef MIDI CLAP restores its values and asks the host to re-read them",
           "[midi][clap]")
{
    Library library;
    for (const bool throughProcess : { true, false })
        for (const std::size_t chunk : { std::size_t { 0 }, std::size_t { 17 } })
            for (std::uint32_t seed = 1; seed <= 10; ++seed)
            {
                INFO ((throughProcess ? "process" : "flush")
                      << ", chunk " << chunk << ", seed " << seed);
                std::mt19937 random (seed);
                std::map<clap_id, double> expected;
                std::vector<std::uint8_t> saved;
                {
                    Host host;
                    Instance first (library, host);
                    auto events = randomValues (first, random);
                    if (throughProcess)
                        processBlock (first, events);
                    else
                    {
                        first.params->flush (first.plugin, &events.in, &events.out);
                        first.poll();
                    }
                    expected = first.values();
                    saved = first.save (chunk);
                    first.poll();
                }
                Host host;
                Instance second (library, host);
                const auto before = second.values();
                host.rescans = 0;
                second.load (saved, chunk);
                second.poll();
                const auto after = second.values();
                INFO ("changed:\n" << describe (second, before, after));
                if (after != before)
                    CHECK ((host.rescans & CLAP_PARAM_RESCAN_VALUES) != 0);
                INFO ("differs from saved:\n" << describe (second, after, expected));
                CHECK (after == expected);
                CHECK (second.save() == saved);
            }
}
