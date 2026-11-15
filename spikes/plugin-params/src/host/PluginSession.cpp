// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.

#include "PluginSession.h"

// Tracktion Engine compiles this header into its own module and does not export
// it, but its parameters are only reachable through it: the class is defined
// inline, so including it again is safe. The product would ask Tracktion to make
// it public, or listen to the JUCE parameters directly.
#include <tracktion_engine/plugins/external/tracktion_ExternalAutomatableParameter.h>

namespace spike
{

namespace
{
constexpr int flushTimerMs = 5;
}

PluginSession::PluginSession (te::Engine& e, const juce::String& memberId, psync::Options options)
    : engine (e), member (memberId), sync (memberId.toStdString(), options)
{
    edit = te::Edit::createSingleTrackEdit (engine);
    edit->getMasterVolumePlugin()->setVolumeDb (0.0f);
    hello.member = member;
    startTimer (flushTimerMs);
}

PluginSession::~PluginSession()
{
    stopTimer();
    detach();
    plugin = nullptr;
    pluginRef = nullptr;
    edit.reset();
}

juce::AudioPluginInstance* PluginSession::getInstance() const
{
    return plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
}

bool PluginSession::loadPlugin (const juce::File& vst3, juce::String& error)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, vst3.getFullPathName());

    if (types.isEmpty())
    {
        error = "No VST3 plugin found in " + vst3.getFullPathName();
        return false;
    }

    const auto desc = *types.getFirst();
    engine.getPluginManager().knownPluginList.addType (desc);

    auto& track = *te::getAudioTracks (*edit)[0];
    int index = 0;

    if (! desc.isInstrument)
    {
        // An effect needs something to process.
        auto tone = edit->getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
        track.pluginList.insertPlugin (tone, 0, nullptr);

        if (auto* t = dynamic_cast<te::ToneGeneratorPlugin*> (tone.get()))
        {
            t->oscTypeParam->setParameter ((float) te::ToneGeneratorPlugin::OscType::triangle, juce::sendNotificationSync);
            t->frequencyParam->setParameter (110.0f, juce::sendNotificationSync);
            t->levelParam->setParameter (0.25f, juce::sendNotificationSync);
        }

        index = 1;
    }

    pluginRef = track.pluginList.insertPlugin (te::ExternalPlugin::create (engine, desc), index);
    plugin = dynamic_cast<te::ExternalPlugin*> (pluginRef.get());

    if (plugin == nullptr)
    {
        error = "Tracktion Engine could not create the plugin";
        return false;
    }

    plugin->initialiseFully();

    if (getInstance() == nullptr)
    {
        error = "The plugin did not load: " + plugin->getLoadError();
        return false;
    }

    hello.plugin.name = desc.name;
    hello.plugin.vendor = desc.manufacturerName;
    hello.plugin.version = desc.version;
    hello.plugin.format = desc.pluginFormatName;
    hello.plugin.uid = desc.pluginFormatName + ":" + juce::String::toHexString (desc.uniqueId);
    hello.plugin.instrument = desc.isInstrument;

    buildParamList();
    juce::StringArray names;
    for (auto& p : params)
        names.add (p.name);

    info ("Loaded " + desc.name + " " + desc.version + " by " + desc.manufacturerName + ": "
          + juce::String ((int) params.size()) + " parameters synced of "
          + juce::String (getInstance()->getParameters().size()) + " the plugin reports"
          + (params.size() <= 12 ? " (" + names.joinIntoString (", ") + ")" : juce::String()));
    return true;
}

void PluginSession::buildParamList()
{
    detach();
    params.clear();
    byId.clear();
    byAp.clear();
    hello.params.clear();

    auto* instance = getInstance();
    auto& all = instance->getParameters();

    // Count every parameter's raw reports, including ones Tracktion skips.
    for (auto* jp : all)
    {
        jp->addListener (this);
        rawListened.push_back (jp);
    }

    for (auto* ap : plugin->getAutomatableParameters())
    {
        // Only the plugin's own parameters. Tracktion adds dry and wet levels of
        // its own, which in the product are part of the session, not the plugin.
        auto* ext = dynamic_cast<te::ExternalAutomatableParameter*> (ap);

        if (ext == nullptr)
            continue;

        auto* jp = all[ext->getParameterIndex()];

        if (jp == nullptr)
            continue;

        // The stable ID is the plugin's own (the VST3 ParamID). Tracktion keys its
        // parameters by list index for VST3, which changes when a plugin update
        // adds a parameter in the middle: never send that.
        juce::String id;
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (jp))
            id = hosted->getParameterID();
        if (id.isEmpty())
        {
            id = "index:" + juce::String (ext->getParameterIndex());
            warn ("parameter " + jp->getName (64) + " has no stable ID; using its index, which is not safe across versions");
        }

        if (byId.count (id) != 0)
        {
            warn ("two parameters share ID " + id + "; the second is not synced");
            continue;
        }

        const int steps = jp->isDiscrete() ? jp->getNumSteps() : 0;
        params.push_back ({ id, jp->getName (128), ext->getParameterIndex(), ext, jp });
        byId[id] = params.size() - 1;
        byAp[ext] = params.size() - 1;
        hello.params.push_back ({ id, jp->getName (128), ext->getParameterIndex(), steps >= 2, steps });

        sync.seed (id.toStdString(), jp->getValue());
        ext->addListener (this);
    }
}

void PluginSession::detach()
{
    for (auto& p : params)
        if (p.ap != nullptr)
            p.ap->removeListener (this);

    for (auto* jp : rawListened)
        jp->removeListener (this);

    rawListened.clear();
}

void PluginSession::addHeldChord()
{
    auto& track = *te::getAudioTracks (*edit)[0];
    const auto length = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (16.0));
    auto clip = track.insertMIDIClip ({ te::TimePosition(), length }, nullptr);

    if (clip == nullptr)
        return;

    for (int note : { 48, 55, 60, 63, 67 })
        clip->getSequence().addNote (note, te::BeatPosition(), te::BeatDuration::fromBeats (16.0), 100, 0, nullptr);

    auto& transport = edit->getTransport();
    transport.setLoopRange ({ te::TimePosition(), length });
    transport.looping = true;
}

bool PluginSession::addSweepAutomation (const juce::String& id, double seconds)
{
    auto* p = findParam (id);

    if (p == nullptr)
        return false;

    auto& curve = p->ap->getCurve();
    curve.clear (nullptr);
    curve.addPoint (te::TimePosition(), 0.1f, 0.0f, nullptr);
    curve.addPoint (te::TimePosition::fromSeconds (seconds * 0.5), 0.9f, 0.0f, nullptr);
    curve.addPoint (te::TimePosition::fromSeconds (seconds), 0.1f, 0.0f, nullptr);
    return true;
}

const PluginSession::Param* PluginSession::findParam (const juce::String& id) const
{
    auto it = byId.find (id);
    return it != byId.end() ? &params[it->second] : nullptr;
}

juce::AudioProcessorParameter* PluginSession::findParameterByName (const juce::String& name) const
{
    if (auto* instance = getInstance())
        for (auto* jp : instance->getParameters())
            if (jp->getName (128) == name)
                return jp;

    return nullptr;
}

const PluginSession::Param* PluginSession::paramFor (te::AutomatableParameter& ap) const
{
    auto it = byAp.find (&ap);
    return it != byAp.end() ? &params[it->second] : nullptr;
}

//==============================================================================
void PluginSession::hostGestureBegin (const juce::String& id)
{
    if (auto* p = findParam (id))
        p->ap->parameterChangeGestureBegin();
}

void PluginSession::hostSet (const juce::String& id, float normalised)
{
    if (auto* p = findParam (id))
    {
        const juce::ScopedValueSetter<bool> svs (hostChanging, true);
        p->ap->setParameter (normalised, juce::sendNotification);
    }
}

void PluginSession::hostGestureEnd (const juce::String& id)
{
    if (auto* p = findParam (id))
        p->ap->parameterChangeGestureEnd();
}

//==============================================================================
void PluginSession::parameterChanged (te::AutomatableParameter& ap, float newValue)
{
    if (applyingRemote)
    {
        // Setting a remote value calls this back at once. Not a change made here.
        ++stats.remoteSetEchoes;
        return;
    }

    auto* p = paramFor (ap);

    if (p == nullptr)
        return;

    ++stats.trackedEvents;
    const auto id = p->id.toStdString();

    // Automation playing this parameter: both members play the same curve, so
    // its values are never sent. Tracktion does not call this for automation
    // playback itself; this catches plugins that report automated values back.
    if (! sync.isHeldHere (id) && ap.isAutomationActive() && edit->getTransport().isPlaying())
    {
        ++stats.automationIgnored;
        logEvent ("automation-ignored", p->id, newValue);
        return;
    }

    if (hostChanging)
        ++stats.fromHostControls;
    else
        ++stats.fromPlugin;

    logEvent (hostChanging ? "host" : "plugin", p->id, newValue, sync.isHeldHere (id) ? "held" : "");

    if (auto again = sync.localValue (id, newValue, nowMs()))
        applyRemote (*again);
}

void PluginSession::parameterChangeGestureBegin (te::AutomatableParameter& ap)
{
    if (auto* p = paramFor (ap))
    {
        logEvent ("gesture-begin", p->id, p->jp->getValue());
        sync.localGestureBegin (p->id.toStdString(), nowMs());
    }
}

void PluginSession::parameterChangeGestureEnd (te::AutomatableParameter& ap)
{
    if (auto* p = paramFor (ap))
    {
        // Tracktion reports gestures at once but values asynchronously, so the
        // end can arrive before the last value. Read the final value now, so the
        // end carries it.
        const auto id = p->id.toStdString();
        const float finalValue = p->jp->getValue();

        if (auto again = sync.localValue (id, finalValue, nowMs()))
            applyRemote (*again);

        logEvent ("gesture-end", p->id, finalValue);
        sync.localGestureEnd (id, nowMs());
    }
}

void PluginSession::parameterValueChanged (int, float)
{
    ++rawValues;

    if (! juce::MessageManager::existsAndIsCurrentThread())
        ++rawValuesOff;
}

void PluginSession::parameterGestureChanged (int, bool)
{
    ++rawGestures;
}

//==============================================================================
void PluginSession::applyRemote (const psync::Apply& a)
{
    const auto id = juce::String (a.param);
    auto* p = findParam (id);

    if (p == nullptr)
    {
        ++stats.unknownRemoteParams;

        if (unknownIdsLogged.insert (id).second)
            warn ("a member changed parameter " + id + ", which this plugin does not have; ignored");
        return;
    }

    logEvent ("apply", id, a.value);
    const juce::ScopedValueSetter<bool> svs (applyingRemote, true);
    p->ap->setParameter (a.value, juce::sendNotification);
}

void PluginSession::timerCallback()
{
    flushNow (false);
}

void PluginSession::flushNow (bool ignoreInterval)
{
    auto out = sync.flush (ignoreInterval ? 1.0e15 : nowMs());

    if (out.empty() || ! send)
        return;

    for (const auto& c : out)
        logEvent ("send", c.param, c.value, juce::String (c.flags));

    const auto message = encodeBatch ({ member, std::move (out) });
    sentBytes += (std::uint64_t) message.getNumBytesAsUTF8();
    send (message);
}

void PluginSession::sendHello()
{
    if (send)
        send (encodeHello (hello));
}

void PluginSession::receive (const juce::String& message)
{
    Hello h;
    Batch b;
    const auto type = decode (message, h, b);

    if (type == "hello")
    {
        if (h.member == member)
            return;

        const bool isNew = peers.count (h.member) == 0;
        peers[h.member] = h;
        auto differences = compareParams (hello, h);
        const bool changed = isNew || mismatches[h.member] != differences;
        mismatches[h.member] = differences;

        // Hellos are repeated as members arrive; report each member once, and again only if it changed.
        if (changed)
        {
            info (h.member + " joined with " + h.plugin.name + " " + h.plugin.version + ", "
                  + juce::String ((int) h.params.size()) + " parameters");

            for (auto& line : differences)
                warn ("parameter lists differ from " + h.member + ": " + line);
        }

        // A late joiner has not seen ours yet.
        if (isNew)
            sendHello();

        return;
    }

    if (type == "batch")
    {
        if (b.from == member)
            return;

        auto peer = peers.find (b.from);

        if (peer != peers.end() && peer->second.plugin.uid != hello.plugin.uid)
        {
            ++stats.foreignPluginBatches;
            return;
        }

        for (const auto& c : b.changes)
            logEvent ("receive", c.param, c.value, b.from + " " + juce::String (c.flags));

        for (const auto& a : sync.receive (b.from.toStdString(), b.changes, nowMs()))
            applyRemote (a);
    }
}

void PluginSession::peersLeft()
{
    for (auto& [id, h] : peers)
        for (const auto& a : sync.memberLeft (id.toStdString()))
            applyRemote (a);

    peers.clear();
    mismatches.clear();
}

SessionStats PluginSession::getStats() const
{
    auto s = stats;
    s.rawValueEvents = rawValues.load();
    s.rawValueEventsOffMessageThread = rawValuesOff.load();
    s.rawGestureEvents = rawGestures.load();
    return s;
}

void PluginSession::setEventLog (const juce::File& file)
{
    eventLog.reset();

    if (file == juce::File())
        return;

    file.deleteFile();
    eventLog = std::make_unique<juce::FileOutputStream> (file);

    if (eventLog->failedToOpen())
    {
        eventLog.reset();
        warn ("could not write the event log to " + file.getFullPathName());
        return;
    }

    *eventLog << "ms,member,event,param,value,extra\n";
}

void PluginSession::logEvent (const char* kind, const juce::String& id, float value, const juce::String& extra)
{
    if (eventLog != nullptr)
        *eventLog << juce::String (nowMs() - startMs, 2) << "," << member << "," << kind << "," << id << ","
                  << juce::String (value, 6) << "," << extra << "\n";
}

void PluginSession::warn (const juce::String& s)
{
    if (log)
        log ("WARNING " + member + ": " + s);
}

void PluginSession::info (const juce::String& s)
{
    if (log)
        log (member + ": " + s);
}

} // namespace spike
