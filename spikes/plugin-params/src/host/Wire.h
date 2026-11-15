// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.
//
// The messages between instances, as JSON text. In the product these would ride
// the relay: live changes as `direct` messages outside the ordered log, and the
// value a gesture ends on as an operation in it (README, "For the product").
// A plugin's saved state travels as an announcement of its hash, then, only to
// whoever asks, its bytes in chunks (README-STATE.md). The product would send
// the bytes as the relay's binary frames, like samples (spikes/sample-transfer).

#pragma once

#include <JuceHeader.h>
#include "../core/ParamSync.h"
#include "../core/StateSync.h"

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

/** A state message. `type` is "state" (an announcement), "stateWant" (a request
    for the bytes of `hash`), "stateData" (a chunk of them) or "stateMissing". */
struct StateMessage
{
    juce::String type, from, to;
    psync::StateRef ref;              // state: the announcement
    juce::String hash;                // stateWant, stateData, stateMissing
    int chunk = 0, chunks = 0;        // stateData
    std::uint64_t compressedSize = 0; // stateData: the size of all chunks together
    juce::MemoryBlock data;           // stateData: this chunk of the zlib-compressed bytes
    psync::ParamSync::Snapshot params;// stateData, first chunk: the sender's parameters when it read the state
};

juce::String encodeHello (const Hello&);
juce::String encodeBatch (const Batch&);

juce::String encodeState (const StateMessage&);

/** Returns the message's type ("hello", "batch", or a state type) and fills the
    matching struct. Empty if it is not one of ours. */
juce::String decode (const juce::String& json, Hello&, Batch&, StateMessage&);

/** Compares two members' parameter lists. Returns one line per difference, or nothing if they match by ID. */
juce::StringArray compareParams (const Hello& mine, const Hello& theirs);

} // namespace spike
