// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.
//
// One member's plugin state: reads and hashes the plugin's saved state when the
// rules (src/core/StateSync) ask, listens for the plugin's hints, sends and
// receives announcements and chunks, applies others' states and reports what
// happened. PluginSession owns one and passes it the messages and the clock.
// Everything runs on the message thread: VST3 requires state calls there, and
// JUCE's host takes the message manager lock for them.

#pragma once

#include <JuceHeader.h>
#include <deque>
#include <functional>

#include "../core/StateSync.h"
#include "Wire.h"

namespace spike
{

/** A plugin's saved state as the host reads it. */
struct SavedState
{
    juce::MemoryBlock bytes;      // what getStateInformation returns: for a VST3, JUCE's XML holding the component and controller states
    std::string hash;             // SHA-256 of bytes
    double readMs = 0.0;
    std::size_t componentBytes = 0, controllerBytes = 0;
};

/** How the host looks for changes to the hidden part. */
enum class Detect
{
    hints,        ///< only when the plugin or the person hints: setDirty, a restart, a burst of unchanged parameter reports, the mouse let go in its window
    pollWindow,   ///< hints, and polling while the plugin's window is open
    pollAlways    ///< hints, and polling all the time
};

struct StateHostStats
{
    std::uint64_t hintSetDirty = 0;        ///< VST3 IComponentHandler2::setDirty, as JUCE's nonParameterStateChanged
    std::uint64_t hintRestart = 0;         ///< VST3 restartComponent, as JUCE's programChanged / parameterInfoChanged
    std::uint64_t hintSameValues = 0;      ///< bursts of parameter reports that changed nothing
    std::uint64_t hintWindow = 0;          ///< the mouse let go in the plugin's window
    std::uint64_t hintManual = 0;
    std::uint64_t announcesSent = 0, announcesReceived = 0;
    std::uint64_t dataBytesSent = 0, dataBytesReceived = 0, chunksSent = 0, chunksReceived = 0;
    std::uint64_t wantsSent = 0, wantsServed = 0, wantsMissing = 0, hashMismatches = 0;
    std::uint64_t applies = 0;
    double applyMsTotal = 0.0, applyMsMax = 0.0;
    double lastTransferMs = 0.0;           ///< from asking for bytes to having all of them
    std::uint64_t lastRawSize = 0, lastCompressedSize = 0;
    std::uint64_t pathsMissingHere = 0;
    std::map<juce::String, std::uint64_t> outcomes;   ///< apply outcomes by name
};

class StateHost : private juce::AudioProcessorListener
{
public:
    StateHost (psync::ParamSync&, const juce::String& memberId);
    ~StateHost() override;

    /** Starts watching a loaded plugin. `readParams` gives what it holds, by parameter ID. */
    void attach (juce::AudioPluginInstance&, std::function<std::map<std::string, float>()> readParams);
    void detach();
    bool isAttached() const noexcept { return instance != nullptr; }

    /** Reads the plugin's saved state now and hashes it. */
    SavedState read() const;
    static SavedState read (juce::AudioPluginInstance&);
    /** The component and controller parts of a JUCE VST3 state blob, decoded. Empty if it is not one. */
    static std::pair<juce::MemoryBlock, juce::MemoryBlock> splitVst3State (const juce::MemoryBlock&);
    /** The reverse of splitVst3State: a blob JUCE's VST3 host loads. */
    static juce::MemoryBlock joinVst3State (const juce::MemoryBlock& component, const juce::MemoryBlock& controller);
    /** Path-like strings in a saved state, from inside its component and controller parts. */
    static std::vector<std::string> pathsIn (const juce::MemoryBlock&);

    //==========================================================================
    // From PluginSession

    std::function<void (const juce::String&)> send;
    std::function<void (const juce::String&)> log;     ///< "WARNING ..." for warnings
    /** Called with values to set on the plugin, as remote parameter values are. */
    std::function<void (const std::vector<psync::Apply>&)> applyParams;
    /** True while anyone here holds a control. */
    std::function<bool()> gestureHeld;

    /** Called every few milliseconds on the message thread. */
    void tick (double nowMs);
    void receive (const StateMessage&, double nowMs);
    void peerJoined (double nowMs);
    void peerLeft (const juce::String& member);

    /** A parameter report from the plugin that repeated the known value. */
    void sameValueReport (double nowMs);
    /** The person or the plugin changed a parameter here. */
    void localParamChange (double nowMs)          { sync.localActivity (nowMs); }
    /** Parameter values from someone else were set on the plugin. */
    void remoteParamChange (double nowMs)         { sync.remoteActivity (nowMs); }
    /** The app's view of the plugin's window: whether a mouse button is down inside it. */
    void windowMouse (bool downInside, double nowMs);

    //==========================================================================
    // Settings and actions for the window and the tests

    bool enabled = true;
    Detect detect = Detect::hints;
    bool windowOpen = false;
    /** Chunks are this big before base64. The product would use the relay's binary frames. */
    int chunkBytes = 256 * 1024;
    /** At most this many chunk bytes go out per tick, so parameter messages are not stuck behind a big state. */
    int bytesPerTick = 512 * 1024;

    psync::StateSync& getSync()                    { return sync; }
    const StateHostStats& getStats() const         { return stats; }
    /** Reads now and announces the state, whatever the rules say. */
    void sendNow (double nowMs);
    /** Asks for a read soon, as a hint would. */
    void checkNow (double nowMs);
    /** True while bytes are being sent or received. */
    bool isTransferring() const;
    bool isSending() const                         { return ! outgoing.empty(); }
    const SavedState& lastRead() const             { return last; }
    bool determinismChecked() const noexcept       { return determinism.has_value(); }
    bool isDeterministic() const noexcept          { return determinism.value_or (true); }
    /** One line per apply outcome and transfer, for the report. */
    const juce::StringArray& history() const       { return events; }

private:
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;

    void readAndObserve (double nowMs);
    void announce (const psync::StateOffer&, const SavedState&, double nowMs);
    void serve (const juce::String& to, const juce::String& hash);
    void apply (const std::string& hash, double nowMs);
    void remember (const std::string& hash, const juce::MemoryBlock& bytes, const psync::ParamSync::Snapshot&);
    void info (const juce::String&);
    void warn (const juce::String&);
    void note (const juce::String&);

    psync::ParamSync& params;
    juce::String member;
    psync::StateSync sync;
    juce::AudioPluginInstance* instance = nullptr;
    std::function<std::map<std::string, float>()> readParams;

    SavedState last;
    std::optional<bool> determinism;

    struct Cached { juce::MemoryBlock bytes; psync::ParamSync::Snapshot params; };
    std::map<std::string, Cached> cache;     // states this member can send, by hash
    std::deque<std::string> cacheOrder;

    struct Outgoing { juce::String to, hash; juce::MemoryBlock compressed; psync::ParamSync::Snapshot params; int next = 0, count = 0; };
    std::deque<Outgoing> outgoing;

    struct Incoming { juce::String hash, from; juce::MemoryBlock compressed; int expected = 0, received = 0; psync::ParamSync::Snapshot params; double startedMs = 0.0; };
    std::optional<Incoming> incoming;
    std::map<std::string, Cached> arrivedBytes;     // waiting to be applied

    double lastSameValueHintMs = -1.0e12;
    bool mouseWasDown = false;
    StateHostStats stats;
    juce::StringArray events;

    JUCE_DECLARE_WEAK_REFERENCEABLE (StateHost)
    JUCE_DECLARE_NON_COPYABLE (StateHost)
};

} // namespace spike
