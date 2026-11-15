// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.
//
// The messages between instances, as JSON text. In the product these would ride
// the relay: live changes as `direct` messages outside the ordered log, and the
// value a gesture ends on as an operation in it (README, "For the product").

#pragma once

#include <JuceHeader.h>
#include "../core/ParamSync.h"

namespace spike
{

/** One parameter as an instance reports it. `id` is the plugin's own stable
    parameter ID (the VST3 ParamID), which is what travels. `index` is only this
    instance's position in its list, and never leaves the machine except here,
    in the hello, so a mismatch can be reported. */
struct ParamInfo
{
    juce::String id, name;
    int index = -1;
    bool discrete = false;
    int steps = 0;
};

/** Which plugin a member has loaded. Never a file path (AGENTS.md). */
struct PluginInfo
{
    juce::String name, vendor, version, format;
    juce::String uid;   // the format's unique id for the plugin class
    bool instrument = false;
};

struct Hello
{
    juce::String member;
    PluginInfo plugin;
    std::vector<ParamInfo> params;
};

struct Batch
{
    juce::String from;   // who sent it; the relay would stamp this
    std::vector<psync::Change> changes;
};

juce::String encodeHello (const Hello&);
juce::String encodeBatch (const Batch&);

/** Returns the message's type ("hello", "batch") and fills the matching struct. Empty if it is not one of ours. */
juce::String decode (const juce::String& json, Hello&, Batch&);

/** Compares two members' parameter lists. Returns one line per difference, or nothing if they match by ID. */
juce::StringArray compareParams (const Hello& mine, const Hello& theirs);

} // namespace spike
