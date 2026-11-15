// Spike for issue #11. Throwaway code: product code never includes this.

#include "Host.h"

#include <iostream>
#include <stdexcept>

#if JUCE_LINUX || JUCE_MAC
 #include <time.h>
#endif

namespace projection
{

const juce::Identifier Host::modelIdProperty ("collabgodId");
const juce::Identifier Host::sampleHashProperty ("collabgodSampleHash");

namespace
{
constexpr double settleQuietMs = 300.0;
constexpr double firstDeviceScanMaxMs = 1500.0;
constexpr double settleMaxMs = 3000.0;
constexpr double settleObserveMs = 1000.0;
constexpr double resumeAfterMs = 150.0;

const double startMs = juce::Time::getMillisecondCounterHiRes();

/** CPU time this thread has used, in ms, or -1 where we do not measure it (Windows).
    Wall time minus CPU time in a block is time the thread was waiting or preempted. */
double threadCpuMs() noexcept
{
   #if JUCE_LINUX || JUCE_MAC
    timespec ts {};
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return static_cast<double> (ts.tv_sec) * 1000.0 + static_cast<double> (ts.tv_nsec) / 1.0e6;
   #endif
    return -1.0;
}

struct SpikeBehaviour : te::EngineBehaviour
{
    SpikeBehaviour (bool h, int c) : headless (h), cpus (c) {}
    bool autoInitialiseDeviceManager() override { return ! headless; }
    int getNumberOfCPUsToUseForAudio() override
    {
        return cpus > 0 ? cpus : te::EngineBehaviour::getNumberOfCPUsToUseForAudio();
    }
    const bool headless;
    const int cpus;
};
} // namespace

void log (const juce::String& message)
{
    std::cerr << juce::String ((juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0, 3)
              << " s: " << message << std::endl;
}

/** Stands in for a sound card: calls the engine's hosted device one block at a time,
    paced to real time. This thread is the audio thread, so its loop does nothing but
    render into preallocated buffers and update atomics. */
class Host::SimulatedSoundCard : public juce::Thread
{
public:
    explicit SimulatedSoundCard (te::HostedAudioDeviceInterface& h)
        : juce::Thread ("Simulated sound card"), hosted (h), buffer (2, simulatedBlockSize)
    {
        midi.ensureSize (1024);
    }

    ~SimulatedSoundCard() override { stopThread (2000); }

    void run() override
    {
        const double blockMs = 1000.0 * simulatedBlockSize / simulatedSampleRate;
        double next = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            const double before = juce::Time::getMillisecondCounterHiRes();
            const double cpuBefore = threadCpuMs();
            buffer.clear();
            midi.clear();
            hosted.processBlock (buffer, midi);
            const double took = juce::Time::getMillisecondCounterHiRes() - before;
            const double cpu = cpuBefore >= 0.0 ? threadCpuMs() - cpuBefore : -1.0;

            if (took > maxBlockMs.load())
            {
                maxBlockMs.store (took);
                maxBlockCpuMs.store (cpu);
            }
            if (took > blockMs)
                lateBlocks.fetch_add (1);

            next += blockMs;
            const double wait = next - juce::Time::getMillisecondCounterHiRes();

            if (wait >= 1.0)
                juce::Thread::sleep (static_cast<int> (wait));
            else if (wait < -100.0)
                next = juce::Time::getMillisecondCounterHiRes();  // fell far behind: do not try to catch up
        }
    }

    std::atomic<double> maxBlockMs { 0.0 }, maxBlockCpuMs { 0.0 };
    std::atomic<int> lateBlocks { 0 };

private:
    te::HostedAudioDeviceInterface& hosted;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};

Host::Host (Audio audio, int audioCpus, const juce::String& deviceType, int bufferSize)
{
    if (audio == Audio::device)
    {
        createEngine (false, audioCpus);
        if (deviceType.isNotEmpty() || bufferSize > 0)
            chooseDevice (deviceType, bufferSize);

        if (auto* device = engine->getDeviceManager().deviceManager.getCurrentAudioDevice())
        {
            audioInfo = { false, device->getTypeName(), device->getName(),
                          device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples() };
        }
        else
        {
            log ("No audio output device opened; using a simulated sound card instead");
            engine.reset();
            audio = Audio::simulated;
        }
    }

    if (audio == Audio::simulated)
    {
        createEngine (true, audioCpus);
        audioInfo = { true, "simulated", "real-time thread", simulatedSampleRate, simulatedBlockSize };
    }

    buildEdit();
    edit->getTransport().addListener (this);
    engine->getDeviceManager().addChangeListener (this);

    // Scan MIDI devices now, before play: the engine's own first scan comes 4 s after
    // start and stops a playing transport if it finds devices (#101).
    settleStart = juce::Time::getMillisecondCounterHiRes();
    engine->getDeviceManager().rescanMidiDeviceList();
    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();

    if (audio == Audio::simulated)
    {
        soundCard = std::make_unique<SimulatedSoundCard> (engine->getDeviceManager().getHostedAudioDeviceInterface());
        soundCard->startThread (juce::Thread::Priority::highest);
    }

    log ("Audio: " + audioInfo.type + " / " + audioInfo.name + ", " + juce::String (audioInfo.sampleRate)
         + " Hz, " + juce::String (audioInfo.blockSize) + " samples per block, " + juce::String (getAudioCpus())
         + " CPUs for audio of " + juce::String (juce::SystemStats::getNumCpus()));
}

int Host::getAudioCpus() const
{
    return engine->getEngineBehaviour().getNumberOfCPUsToUseForAudio();
}

Host::~Host()
{
    soundCard.reset();
    engine->getDeviceManager().removeChangeListener (this);
    edit->getTransport().removeListener (this);
    edit->getTransport().stop (false, false);
    detector = nullptr;
    probeTrack = nullptr;
    edit.reset();
    engine.reset();
}

void Host::createEngine (bool simulated, int audioCpus)
{
    if (simulated)
    {
        engine = std::make_unique<te::Engine> ("CollabGodEngineProjectionSpike", nullptr, std::make_unique<SpikeBehaviour> (true, audioCpus));

        te::HostedAudioDeviceInterface::Parameters params;
        params.sampleRate = simulatedSampleRate;
        params.blockSize = simulatedBlockSize;
        params.inputChannels = 0;
        params.outputChannels = 2;

        auto& hosted = engine->getDeviceManager().getHostedAudioDeviceInterface();
        hosted.initialise (params);
        hosted.prepareToPlay (simulatedSampleRate, simulatedBlockSize);
        engine->getDeviceManager().dispatchPendingUpdates();
    }
    else
    {
        engine = std::make_unique<te::Engine> ("CollabGodEngineProjectionSpike", nullptr, std::make_unique<SpikeBehaviour> (false, audioCpus));
    }

    // Neither is registered by default (engine-toolchain finding 5).
    engine->getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
    engine->getPluginManager().createBuiltInType<ClickDetectorPlugin>();
}

void Host::chooseDevice (const juce::String& deviceType, int bufferSize)
{
    auto& dm = engine->getDeviceManager().deviceManager;

    if (deviceType.isNotEmpty())
    {
        juce::StringArray types;
        for (auto* type : dm.getAvailableDeviceTypes())
            types.add (type->getTypeName());

        if (! types.contains (deviceType))
            throw std::runtime_error (("no audio device type \"" + deviceType + "\"; this machine has: \""
                                       + types.joinIntoString ("\", \"") + "\"").toStdString());

        dm.setCurrentAudioDeviceType (deviceType, true);
    }

    if (bufferSize > 0)
    {
        auto setup = dm.getAudioDeviceSetup();
        setup.bufferSize = bufferSize;
        const auto error = dm.setAudioDeviceSetup (setup, true);
        if (error.isNotEmpty())
            throw std::runtime_error (("could not set " + juce::String (bufferSize) + " samples per block: " + error).toStdString());
    }

    auto* device = dm.getCurrentAudioDevice();
    if (device == nullptr)
        throw std::runtime_error (("no device opened for \"" + dm.getCurrentAudioDeviceType() + "\"").toStdString());

    if (bufferSize > 0 && device->getCurrentBufferSizeSamples() != bufferSize)
    {
        juce::StringArray sizes;
        for (auto size : device->getAvailableBufferSizes())
            sizes.add (juce::String (size));
        log ("Asked for " + juce::String (bufferSize) + " samples per block, the device gave "
             + juce::String (device->getCurrentBufferSizeSamples()) + ". It offers: " + sizes.joinIntoString (", "));
    }
}

void Host::buildEdit()
{
    edit = te::Edit::createSingleTrackEdit (*engine, te::Edit::forEditing);
    edit->getMasterVolumePlugin()->setVolumeDb (0.0f);

    probeTrack = te::getAudioTracks (*edit)[0];
    probeTrack->setName ("Probe");

    auto plugin = edit->getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
    auto tone = dynamic_cast<te::ToneGeneratorPlugin*> (plugin.get());
    jassert (tone != nullptr);
    probeTrack->pluginList.insertPlugin (plugin, 0, nullptr);

    // Through the parameters: assigning the CachedValues does not change what plays
    // (engine-toolchain finding 5).
    tone->oscTypeParam->setParameter (static_cast<float> (te::ToneGeneratorPlugin::OscType::sin), juce::sendNotificationSync);
    tone->frequencyParam->setParameter (static_cast<float> (probeHz), juce::sendNotificationSync);
    tone->levelParam->setParameter (probeLevel, juce::sendNotificationSync);

    if (auto vol = probeTrack->getVolumePlugin())
        vol->setPan (-1.0f);

    auto detectorPlugin = edit->getPluginCache().createNewPlugin (ClickDetectorPlugin::xmlTypeName, {});
    detector = dynamic_cast<ClickDetectorPlugin*> (detectorPlugin.get());
    jassert (detector != nullptr);
    edit->getMasterPluginList().insertPlugin (detectorPlugin, -1, nullptr);
}

void Host::playbackContextChanged()
{
    ++contextChanges;
    lastContextChange = juce::Time::getMillisecondCounterHiRes();
}

void Host::changeListenerCallback (juce::ChangeBroadcaster*)
{
    ++deviceChanges;
    lastDeviceChange = juce::Time::getMillisecondCounterHiRes();

    auto& dm = engine->getDeviceManager();
    log ("Device lists changed: " + juce::String (dm.getNumMidiInDevices()) + " MIDI in, "
         + juce::String (dm.getNumMidiOutDevices()) + " MIDI out" + (settled ? "" : " (while settling)"));
}

void Host::tick()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    auto& transport = edit->getTransport();

    if (! settled)
    {
        // The same rule as the web view spike's EngineHost (its finding 8): quiet means
        // quiet across ticks we actually ran, and the time-out counts only ticked time.
        const bool ready = contextChanges > 0 && transport.isPlayContextActive()
                           && (deviceChanges > 0 || now - settleStart >= firstDeviceScanMaxMs);

        if (! ready || quietSince < 0.0 || juce::jmax (lastContextChange, lastDeviceChange) > quietSince)
            quietSince = ready ? now : -1.0;

        const bool quiet = ready && now - quietSince >= settleQuietMs;

        if (firstTick < 0.0)
            firstTick = now;
        const bool tooLong = now - settleStart >= settleMaxMs && now - firstTick >= settleObserveMs;

        if (quiet || tooLong)
        {
            settled = true;
            settleMs = now - settleStart;
            log ("Engine settled after " + juce::String (settleMs, 0) + " ms, " + juce::String (contextChanges)
                 + " context changes and " + juce::String (deviceChanges) + " device list changes"
                 + (quiet ? "" : " (timed out waiting for quiet)"));

            if (wantPlaying)
            {
                transport.ensureContextAllocated();
                transport.play (false);
            }
        }

        return;
    }

    resumeIfStoppedUnexpectedly (now);
}

void Host::resumeIfStoppedUnexpectedly (double now)
{
    auto& transport = edit->getTransport();

    if (transport.isPlaying())
    {
        stoppedSince = -1.0;
        lastPlayingPosition = getAudibleSeconds();
        return;
    }

    if (! wantPlaying)
        return;

    if (stoppedSince < 0.0)
    {
        stoppedSince = now;
        return;
    }

    if (now - stoppedSince < resumeAfterMs)
        return;

    ++unexpectedStops;
    stoppedSince = now;
    log ("Transport stopped without being asked at " + juce::String (lastPlayingPosition, 3)
         + " s; pressing play again (" + juce::String (unexpectedStops) + " so far, see #101)");
    transport.ensureContextAllocated();
    transport.setPosition (te::TimePosition::fromSeconds (lastPlayingPosition));
    transport.play (false);
}

void Host::setPlaying (bool shouldPlay)
{
    wantPlaying = shouldPlay;
    stoppedSince = -1.0;
    auto& transport = edit->getTransport();

    if (! shouldPlay)
    {
        transport.stop (false, false);
        return;
    }

    if (settled && ! transport.isPlaying())
    {
        transport.ensureContextAllocated();
        transport.play (false);
    }
}

void Host::setPosition (double seconds)
{
    edit->getTransport().setPosition (te::TimePosition::fromSeconds (juce::jmax (0.0, seconds)));
    lastPlayingPosition = seconds;
}

void Host::setLoop (double startSeconds, double endSeconds)
{
    auto& transport = edit->getTransport();

    if (endSeconds > startSeconds)
    {
        transport.setLoopRange ({ te::TimePosition::fromSeconds (startSeconds), te::TimePosition::fromSeconds (endSeconds) });
        transport.looping = true;
    }
    else
    {
        transport.looping = false;
    }
}

double Host::getAudibleSeconds()
{
    auto& transport = edit->getTransport();
    // TransportControl::getPosition is only refreshed on a timer; ask the playback context.
    if (auto* ctx = transport.getCurrentPlaybackContext())
        return ctx->getAudibleTimelineTime().inSeconds();
    return transport.getPosition().inSeconds();
}

int Host::getXRuns() const
{
    if (audioInfo.simulated)
        return -1;
    // The manager's count is the device's own (where it has one) plus the callbacks JUCE
    // timed as longer than their block. CoreAudio has no counter of its own, so without
    // the second part a late callback on a Mac was invisible to the check.
    auto& manager = engine->getDeviceManager().deviceManager;
    if (manager.getCurrentAudioDevice() != nullptr)
        return manager.getXRunCount();
    return -1;
}

double Host::getMaxBlockMs() const  { return soundCard != nullptr ? soundCard->maxBlockMs.load() : -1.0; }
double Host::getMaxBlockCpuMs() const { return soundCard != nullptr ? soundCard->maxBlockCpuMs.load() : -1.0; }
int Host::getLateBlocks() const     { return soundCard != nullptr ? soundCard->lateBlocks.load() : -1; }

void Host::resetBlockTimes()
{
    if (soundCard != nullptr)
    {
        soundCard->maxBlockMs.store (0.0);
        soundCard->maxBlockCpuMs.store (0.0);
        soundCard->lateBlocks.store (0);
    }
}

} // namespace projection
