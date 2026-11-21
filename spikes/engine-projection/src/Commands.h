// Spike for issue #11. Throwaway code: product code never includes this.

#pragma once

#include <JuceHeader.h>
#include <map>

#include "Host.h"

namespace projection
{

/**
    The engine side of the projection: applies the commands the TypeScript side sends,
    and reads back what the edit actually holds.

    This side has no logic of its own about the session. It does not diff, order or
    decide anything: it carries out primitive commands on Tracktion's edit and fails
    loudly when one cannot be carried out. All decisions are in TypeScript
    (ADR-0002: the engine is a projection of the model). The commands and the readback
    are specified in contract/README.md.

    Message thread only.
*/
class Projector
{
public:
    /** realTimeNodes: turn off proxy files on every clip, which makes Tracktion play it with
        WaveNodeRealTime (reads the file cache without waiting) instead of WaveNode (waits up to
        3 ms per read on the audio thread). See README finding 4. */
    Projector (Host&, bool realTimeNodes);

    struct ApplyResult
    {
        int applied = 0;          ///< commands carried out
        int failedAt = -1;        ///< index of the command that failed, or -1
        juce::String error;
        double applyMs = 0.0;     ///< time to change Tracktion's edit
        double rebuildMs = 0.0;   ///< time for Tracktion to rebuild its playback graph afterwards
    };

    /** Carries out the commands in order. Stops at the first that fails and reports it.
        Then asks Tracktion to rebuild its playback graph now, rather than on its next
        timer, so the cost of the rebuild is measured here. */
    ApplyResult apply (const juce::var& commands);

    /** What the edit holds, read from Tracktion's own objects, in the contract's form. */
    juce::var readback() const;

    int getNumClips() const     { return static_cast<int> (clips.size()); }

private:
    juce::Result applyOne (const juce::var& command);
    juce::Result addTrack (const juce::String& id, const juce::var& after);
    juce::Result moveTrack (const juce::String& id, const juce::var& after);
    juce::Result removeTrack (const juce::String& id);
    juce::Result addClip (const juce::var& command);
    juce::Result setClip (const juce::var& command);
    juce::Result removeClip (const juce::String& id);
    juce::Result reset();

    te::AudioTrack* findTrack (const juce::String& id) const;
    te::WaveAudioClip* findClip (const juce::String& id) const;
    juce::Result trackAfter (const juce::var& after, te::Track*& preceding) const;

    Host& host;
    te::Edit& edit;
    const bool realTimeNodes;
    // The model's ids to Tracktion's. Readback never uses these: it walks the edit.
    std::map<juce::String, te::EditItemID> tracks, clips;
};

} // namespace projection
