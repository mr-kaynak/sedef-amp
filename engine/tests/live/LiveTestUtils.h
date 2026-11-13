// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/live/LiveControl.h"

#include <vector>

namespace sedef::test
{

/// The live layer between a pretend host and a pretend message thread: `store` stands for the
/// host's parameters (what the processor reads into the snapshot), `master` for the message
/// thread's scene bank. Blocks run like SedefProcessor::processBlock does.
struct LiveHarness
{
    LiveHarness()
    {
        store = ParamSnapshot::defaults();
        master = live::SceneBank::withSwitches (store);
        control.setScenes (master);
    }

    /// One audio block: acknowledge, read the parameters, then the live layer. Returns what the
    /// engine would get.
    ParamSnapshot block (const std::vector<live::MidiMessage>& messages = {}, int frames = 64)
    {
        control.acknowledge();
        auto snapshot = store;
        control.process (snapshot, messages.data(), static_cast<int> (messages.size()), frames);
        return snapshot;
    }

    /// The message thread's tick: writes the overrides into the host's parameters and takes in
    /// the reports, as the plug-in does (docs/live.md §7.2).
    void tick()
    {
        control.syncParameters ([this] (ParamId id, float v) { store[id] = v; });
        live::LiveEvent e;
        bool tookReports = false;
        while (control.popEvent (e))
        {
            events.push_back (e);
            if (e.type != live::LiveEvent::Type::sceneSwitched)
                continue;
            const auto& r = e.scene;
            // A report made on a bank switched on screen since is passed over (§7.2).
            if (r.basedOn == master.switchCount)
            {
                for (int i = 0; i < r.numParams; ++i)
                {
                    const int at =
                        master.find (static_cast<ParamId> (r.params[static_cast<size_t> (i)]));
                    if (at >= 0)
                        master.values[r.from][static_cast<size_t> (at)] =
                            r.values[static_cast<size_t> (i)];
                }
                if (!master.isMade (r.to))
                    master.values[r.to] = master.values[r.from];
                master.made = static_cast<std::uint8_t> (master.made | 1u << r.from | 1u << r.to);
                master.active = r.to;
            }
            ++master.reportsSeen;
            tookReports = true;
        }
        if (tookReports)
        {
            master.applySwitch = false;
            control.setScenes (master);
        }
    }

    /// A switch made on screen: the parameters are written here, the audio thread applies the
    /// same values in one block.
    void screenSwitch (int to)
    {
        tick();
        for (int i = 0; i < master.numParams; ++i)
            master.values[master.active][static_cast<size_t> (i)] =
                store.values[master.params[static_cast<size_t> (i)]];
        if (!master.isMade (to))
            master.values[static_cast<size_t> (to)] = master.values[master.active];
        master.made = static_cast<std::uint8_t> (master.made | 1u << to);
        master.active = static_cast<std::uint8_t> (to);
        ++master.switchCount;
        master.applySwitch = true;
        control.setScenes (master);
        for (int i = 0; i < master.numParams; ++i)
            store.values[master.params[static_cast<size_t> (i)]] =
                master.values[static_cast<size_t> (to)][static_cast<size_t> (i)];
        store[ParamId::scene] = static_cast<float> (to);
    }

    void bind (const live::Binding& b)
    {
        map.add (b);
        control.setMap (map);
    }

    live::LiveControl control;
    live::MidiMap map;
    live::SceneBank master;
    ParamSnapshot store;
    std::vector<live::LiveEvent> events;
};

inline live::Binding bindParam (ParamId id, int controller, int channel = 0,
                                live::SwitchMode mode = live::SwitchMode::toggle)
{
    live::Binding b;
    b.channel = static_cast<std::uint8_t> (channel);
    b.controller = static_cast<std::uint8_t> (controller);
    b.action = live::Action::parameter;
    b.param = static_cast<std::uint16_t> (index (id));
    b.mode = mode;
    return b;
}

inline live::Binding bindAction (live::Action action, int controller, int scene = 0,
                                 live::SwitchMode mode = live::SwitchMode::toggle)
{
    live::Binding b;
    b.controller = static_cast<std::uint8_t> (controller);
    b.action = action;
    b.scene = static_cast<std::uint8_t> (scene);
    b.mode = mode;
    return b;
}

inline live::MidiMessage cc (int controller, int value, int channel = 0)
{
    return live::MidiMessage::controlChange (channel, controller, value);
}

} // namespace sedef::test
