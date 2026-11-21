// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issues #12 and #13. Throwaway code: product code never includes this.
//
// One member's side: an edit with one VST3 hosted in Tracktion Engine, the
// listeners that hear its parameters change, and the sync rules (src/core) in
// between. Its StateHost syncs the plugin's saved state (#13). It sends and receives text messages; who carries them (Link, or a
// test) is up to the caller. Everything runs on the message thread except the
// raw counters, which a plugin may bump from its audio thread.

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <map>

#include "../core/ParamSync.h"
#include "StateHost.h"
#include "Wire.h"

namespace spike
{
namespace te = tracktion;

/** Counters the spike reports. Totals since the plugin was loaded. */
struct SessionStats
{
    // What the plugin reported to the host, straight from JUCE, on any thread.
    std::uint64_t rawValueEvents = 0;
    std::uint64_t rawValueEventsOffMessageThread = 0;
    std::uint64_t rawGestureEvents = 0;
    // What reached the sync from Tracktion Engine's listener.
    std::uint64_t trackedEvents = 0;
    std::uint64_t fromHostControls = 0;    ///< changes made by this host's own controls
    std::uint64_t fromPlugin = 0;          ///< everything else: the plugin's window, MIDI learn, its own modulation
    std::uint64_t automationIgnored = 0;   ///< changes while automation plays the parameter; each side plays its own copy
    std::uint64_t remoteSetEchoes = 0;     ///< callbacks caused by setting a remote value, swallowed at once
    std::uint64_t unknownRemoteParams = 0; ///< remote changes to a parameter ID this plugin does not have
    std::uint64_t sameValueReports = 0;    ///< reports from the plugin that repeated the known value
    std::uint64_t foreignPluginBatches = 0;///< batches from a member with a different plugin, ignored
};

class PluginSession : private te::AutomatableParameter::Listener,
                      private juce::AudioProcessorParameter::Listener,
                      private juce::Timer
{
public:
    PluginSession (te::Engine&, const juce::String& memberId, psync::Options = {});
    ~PluginSession() override;

    /** Loads the first plugin in a .vst3 file onto the edit's only track. An
        effect gets Tracktion's tone generator in front of it so it has input. */
    bool loadPlugin (const juce::File& vst3, juce::String& error);

    /** For instruments: a four-bar MIDI clip holding a chord, looped, so
        parameter changes can be heard while the transport plays. */
    void addHeldChord();

    /** Puts a curve on a parameter that goes up and down over `seconds`. Both
        members add the same curve; automation is never sent, each plays its own. */
    bool addSweepAutomation (const juce::String& paramId, double seconds);

    te::Edit& getEdit()                             { return *edit; }
    te::ExternalPlugin* getPlugin() const           { return plugin; }
    juce::AudioPluginInstance* getInstance() const;
    const juce::String& getMember() const           { return member; }

    //==========================================================================
    struct Param
    {
        juce::String id, name;
        int index = -1;
        te::AutomatableParameter* ap = nullptr;   // the plugin's parameter as Tracktion Engine wraps it
        juce::AudioProcessorParameter* jp = nullptr;
    };

    const std::vector<Param>& getParams() const     { return params; }
    const Param* findParam (const juce::String& id) const;
    /** Any of the plugin's parameters, synced or not, by name. For the test plugin's controls. */
    juce::AudioProcessorParameter* findParameterByName (const juce::String& name) const;

    // The host's own controls, as a slider in this app would use them.
    void hostGestureBegin (const juce::String& id);
    void hostSet (const juce::String& id, float normalised);
    void hostGestureEnd (const juce::String& id);

    //==========================================================================
    // Messages

    /** Called with each message to send to the other members. */
    std::function<void (const juce::String&)> send;
    /** Called with lines for the log. Warnings start with "WARNING". */
    std::function<void (const juce::String&)> log;

    void receive (const juce::String& message);
    void sendHello();
    /** Call when connections drop: nobody who left can hold a control. */
    void peersLeft();

    const Hello& getHello() const                   { return hello; }
    const std::map<juce::String, Hello>& getPeers() const { return peers; }

    /** Where this member's hands are: the plugin window open, a button pressed in it.
        Sent to the others when it changes. */
    void setPresence (bool windowOpen, bool pressing);
    const std::map<juce::String, Presence>& getPresence() const { return presence; }
    /** One line per other member: which plugin they have up, whether its window is
        open, and what they are touching in it. */
    juce::StringArray describeOthers() const;
    /** The differences found with each peer's parameter list, by member. */
    const std::map<juce::String, juce::StringArray>& getMismatches() const { return mismatches; }

    psync::ParamSync& getSync()                     { return sync; }
    StateHost& getState()                           { return state; }
    /** What the plugin holds now for every synced parameter, by ID. */
    std::map<std::string, float> paramValues() const;
    SessionStats getStats() const;
    std::uint64_t bytesSent() const noexcept        { return sentBytes; }

    /** Writes every event to a CSV file, or stops if the file is empty. */
    void setEventLog (const juce::File&);

    /** Sends whatever the sync holds back now, ignoring the interval. For tests. */
    void flushNow (bool ignoreInterval);

    static double nowMs()                           { return juce::Time::getMillisecondCounterHiRes(); }

private:
    // te::AutomatableParameter::Listener (message thread)
    void curveHasChanged (te::AutomatableParameter&) override {}
    void parameterChanged (te::AutomatableParameter&, float newValue) override;
    void parameterChangeGestureBegin (te::AutomatableParameter&) override;
    void parameterChangeGestureEnd (te::AutomatableParameter&) override;

    // juce::AudioProcessorParameter::Listener (any thread): counting only
    void parameterValueChanged (int, float) override;
    void parameterGestureChanged (int, bool) override;

    void timerCallback() override;

    void buildParamList();
    void detach();
    void applyRemote (const psync::Apply&);
    const Param* paramFor (te::AutomatableParameter&) const;
    void logEvent (const char* kind, const juce::String& id, float value, const juce::String& extra = {});
    void warn (const juce::String&);
    void info (const juce::String&);

    te::Engine& engine;
    juce::String member;
    std::unique_ptr<te::Edit> edit;
    te::ExternalPlugin* plugin = nullptr;
    te::Plugin::Ptr pluginRef;
    std::vector<Param> params;
    std::map<juce::String, std::size_t> byId;
    std::map<te::AutomatableParameter*, std::size_t> byAp;
    std::vector<juce::AudioProcessorParameter*> rawListened;

    psync::ParamSync sync;
    StateHost state { sync, member };
    double lastRemoteApplyMs = -1.0e12;
    Hello hello;
    std::map<juce::String, Hello> peers;
    std::map<juce::String, Presence> presence;
    Presence ownPresence;
    void sendPresence();
    std::map<juce::String, juce::StringArray> mismatches;
    std::set<juce::String> unknownIdsLogged;

    bool applyingRemote = false;
    bool hostChanging = false;
    SessionStats stats;
    std::atomic<std::uint64_t> rawValues { 0 }, rawValuesOff { 0 }, rawGestures { 0 };
    std::uint64_t sentBytes = 0;

    std::unique_ptr<juce::FileOutputStream> eventLog;
    double startMs = nowMs();

    JUCE_DECLARE_NON_COPYABLE (PluginSession)
};

} // namespace spike
