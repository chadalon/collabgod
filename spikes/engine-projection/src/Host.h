// Spike for issue #11. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>
#include <atomic>

#include "ClickDetector.h"

namespace projection
{
namespace te = tracktion;

/**
    Owns a Tracktion Engine, an edit, the probe track and the click detector.

    The edit starts with one track, the probe: Tracktion's Tone Generator at 440 Hz,
    panned hard left, which no command touches. Tracks the model asks for are added
    after it and panned hard right. The click detector sits last on the master bus.

    Known engine problems that are not this spike's (#101) are handled the way the
    web view spike (#8) handles them, and counted separately so they are not mistaken
    for the projection's:
    - play waits until the engine has settled after start-up (playback context created,
      first MIDI device scan reported, then 300 ms of quiet);
    - a transport that stops without being asked (a MIDI device list change) is
      pressed again at the same place, and counted as an unexpected stop.

    Everything here runs on the message thread except the simulated sound card's
    thread, which only calls processBlock on preallocated buffers.
*/
class Host : private te::TransportControl::Listener,
             private juce::ChangeListener
{
public:
    enum class Audio
    {
        device,     ///< the system's default output, or the device type asked for
        simulated   ///< a simulated sound card driven in real time by a thread of ours
    };

    /** audioCpus: how many CPUs Tracktion may use for audio, the audio thread included;
        0 keeps Tracktion's default, every CPU the machine has.
        deviceType: a JUCE device type name, such as "Windows Audio (Exclusive Mode)"; empty
        keeps the default. bufferSize: samples per block; 0 keeps the device's default.
        Throws std::runtime_error if the type does not exist or the device will not open. */
    explicit Host (Audio, int audioCpus = 0, const juce::String& deviceType = {}, int bufferSize = 0);
    ~Host() override;

    /** Call regularly on the message thread. Completes settling, applies a waiting
        play request, and resumes after an unexpected stop. */
    void tick();

    bool isSettled() const noexcept      { return settled; }
    double getSettleMs() const noexcept  { return settleMs; }

    void setPlaying (bool);
    void setPosition (double seconds);
    void setLoop (double startSeconds, double endSeconds);  ///< end <= start turns looping off

    /** Where the audio thread is playing, in edit seconds. */
    double getAudibleSeconds();

    te::Engine& getEngine()                 { return *engine; }
    te::Edit& getEdit()                     { return *edit; }
    te::AudioTrack& getProbeTrack()         { return *probeTrack; }
    ClickDetectorPlugin& getDetector()      { return *detector; }

    struct AudioInfo
    {
        bool simulated = false;
        juce::String type, name;
        double sampleRate = 0.0;
        int blockSize = 0;
    };

    AudioInfo getAudioInfo() const          { return audioInfo; }
    int getUnexpectedStops() const noexcept { return unexpectedStops; }
    int getAudioCpus() const;
    int getXRuns() const;                   ///< device dropout counter, -1 if unknown or simulated

    /** For the simulated sound card: the slowest block so far, and how many took longer than real time. */
    double getMaxBlockMs() const;
    int getLateBlocks() const;
    /** The audio thread's own CPU time in that slowest block; the rest was waiting or preempted. -1 on Windows. */
    double getMaxBlockCpuMs() const;
    void resetBlockTimes();

    static constexpr double probeHz = 440.0;
    static constexpr float probeLevel = 0.2f;
    /** Gain on every model track. Hard panning adds 6 dB in Tracktion, and many clips overlap:
        at 0 dB the dropout check's session peaked 14 to 18 dB over full scale (README finding 11). */
    static constexpr float modelTrackGainDb = -24.0f;
    static constexpr double simulatedSampleRate = 44100.0;
    static constexpr int simulatedBlockSize = 441;  // what Chad's Windows Audio device used

    /** Marks a track as belonging to the model: holds the model's id. The probe has none. */
    static const juce::Identifier modelIdProperty;
    /** Holds the sample hash on a clip. */
    static const juce::Identifier sampleHashProperty;

private:
    void playbackContextChanged() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void createEngine (bool simulated, int audioCpus);
    void chooseDevice (const juce::String& deviceType, int bufferSize);
    void buildEdit();
    void resumeIfStoppedUnexpectedly (double now);

    class SimulatedSoundCard;

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<SimulatedSoundCard> soundCard;
    te::AudioTrack::Ptr probeTrack;
    juce::ReferenceCountedObjectPtr<ClickDetectorPlugin> detector;
    AudioInfo audioInfo;

    // Settling (engine-toolchain finding 8, web view finding 8)
    double settleStart = 0.0, lastContextChange = 0.0, lastDeviceChange = 0.0;
    double quietSince = -1.0, firstTick = -1.0, settleMs = 0.0;
    int contextChanges = 0, deviceChanges = 0;
    bool settled = false;

    // Playing and resuming (#101)
    bool wantPlaying = false;
    double stoppedSince = -1.0, lastPlayingPosition = 0.0;
    int unexpectedStops = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Host)
};

/** Command-line options shared by serve, selftest and bench. */
struct Options
{
    int audioCpus = 0;           ///< CPUs Tracktion may use for audio; 0 for its default, all of them
    bool realTimeNodes = true;   ///< play clips without proxies, with WaveNodeRealTime (README finding 4)
    juce::String deviceType;     ///< audio device type to open; empty for the default
    int bufferSize = 0;          ///< samples per block to ask the device for; 0 for its default
};

/** The three commands (Main.cpp dispatches to them). */
int serve (bool simulated, Options);
int selfTest (const juce::File& outFolder, Options);
int bench (const juce::File& outFolder, bool deviceAudio, Options);

/** Writes a line to stderr, prefixed with seconds since start. Message thread only. */
void log (const juce::String&);

} // namespace projection
