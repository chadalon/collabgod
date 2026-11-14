// Spike for issue #11. Throwaway code: product code never includes this.

#include "Commands.h"

#include <algorithm>

namespace projection
{

namespace
{
juce::Result fail (const juce::String& message) { return juce::Result::fail (message); }

bool isNumber (const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

bool readSeconds (const juce::var& command, const char* key, double& out, bool positive)
{
    const auto v = command[key];
    if (! isNumber (v))
        return false;
    out = static_cast<double> (v);
    return std::isfinite (out) && (positive ? out > 0.0 : out >= 0.0);
}
} // namespace

Projector::Projector (Host& h, bool rt) : host (h), edit (h.getEdit()), realTimeNodes (rt) {}

te::AudioTrack* Projector::findTrack (const juce::String& id) const
{
    const auto it = tracks.find (id);
    if (it == tracks.end())
        return nullptr;
    return dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, it->second));
}

te::WaveAudioClip* Projector::findClip (const juce::String& id) const
{
    const auto it = clips.find (id);
    if (it == clips.end())
        return nullptr;
    return dynamic_cast<te::WaveAudioClip*> (te::findClipForID (edit, it->second));
}

juce::Result Projector::trackAfter (const juce::var& after, te::Track*& preceding) const
{
    // null: first of the model's tracks, which is straight after the probe.
    if (after.isVoid() || after.isUndefined())
    {
        preceding = &host.getProbeTrack();
        return juce::Result::ok();
    }

    if (! after.isString())
        return fail ("'after' must be a track id or null");

    preceding = findTrack (after.toString());
    return preceding != nullptr ? juce::Result::ok() : fail ("no track " + after.toString() + " to go after");
}

juce::Result Projector::addTrack (const juce::String& id, const juce::var& after)
{
    if (id.isEmpty())
        return fail ("track id missing");
    if (tracks.count (id) > 0)
        return fail ("track " + id + " already exists");

    te::Track* preceding = nullptr;
    if (auto r = trackAfter (after, preceding); r.failed())
        return r;

    auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr, true);
    if (track == nullptr)
        return fail ("Tracktion did not create track " + id);

    track->state.setProperty (Host::modelIdProperty, id, nullptr);
    track->setName (id);
    if (auto vol = track->getVolumePlugin())
        vol->setPan (1.0f);  // the model's audio on the right; the probe is on the left

    tracks[id] = track->itemID;
    return juce::Result::ok();
}

juce::Result Projector::moveTrack (const juce::String& id, const juce::var& after)
{
    auto track = findTrack (id);
    if (track == nullptr)
        return fail ("no track " + id);

    te::Track* preceding = nullptr;
    if (auto r = trackAfter (after, preceding); r.failed())
        return r;
    if (preceding == track)
        return fail ("track " + id + " cannot go after itself");

    edit.moveTrack (track, te::TrackInsertPoint (nullptr, preceding));
    return juce::Result::ok();
}

juce::Result Projector::removeTrack (const juce::String& id)
{
    auto track = findTrack (id);
    if (track == nullptr)
        return fail ("no track " + id);

    // Its clips go with it.
    for (auto clip : track->getClips())
        clips.erase (clip->state[Host::modelIdProperty].toString());

    tracks.erase (id);
    edit.deleteTrack (track);
    return juce::Result::ok();
}

juce::Result Projector::addClip (const juce::var& command)
{
    const auto id = command["id"].toString();
    const auto trackId = command["trackId"].toString();
    const auto hash = command["sampleHash"].toString();
    const auto path = command["file"].toString();
    double start = 0.0, length = 0.0;

    if (id.isEmpty() || hash.isEmpty() || path.isEmpty())
        return fail ("clip.add needs id, sampleHash and file");
    if (! readSeconds (command, "start", start, false) || ! readSeconds (command, "length", length, true))
        return fail ("clip " + id + ": start must be >= 0 and length > 0, in seconds");
    if (clips.count (id) > 0)
        return fail ("clip " + id + " already exists");

    auto track = findTrack (trackId);
    if (track == nullptr)
        return fail ("clip " + id + ": no track " + trackId);

    const juce::File file (path);
    if (! juce::File::isAbsolutePath (path) || ! te::AudioFile (edit.engine, file).isValid())
        return fail ("clip " + id + ": cannot read audio file " + path);

    const te::ClipPosition position { { te::TimePosition::fromSeconds (start), te::TimeDuration::fromSeconds (length) },
                                      te::TimeDuration() };
    auto clip = track->insertWaveClip (id, file, position, false);
    if (clip == nullptr)
        return fail ("Tracktion did not create clip " + id);

    if (realTimeNodes)
        clip->setUsesProxy (false);

    clip->state.setProperty (Host::modelIdProperty, id, nullptr);
    clip->state.setProperty (Host::sampleHashProperty, hash, nullptr);
    clips[id] = clip->itemID;
    return juce::Result::ok();
}

juce::Result Projector::setClip (const juce::var& command)
{
    const auto id = command["id"].toString();
    auto clip = findClip (id);
    if (clip == nullptr)
        return fail ("no clip " + id);

    // Check everything before changing anything, so a failed command changes nothing.
    te::AudioTrack* destination = nullptr;
    double start = 0.0, length = 0.0;
    const bool hasTrack = command.hasProperty ("trackId");
    const bool hasStart = command.hasProperty ("start");
    const bool hasLength = command.hasProperty ("length");

    if (! hasTrack && ! hasStart && ! hasLength)
        return fail ("clip.set " + id + " changes nothing");
    if (hasTrack && (destination = findTrack (command["trackId"].toString())) == nullptr)
        return fail ("clip " + id + ": no track " + command["trackId"].toString());
    if (hasStart && ! readSeconds (command, "start", start, false))
        return fail ("clip " + id + ": start must be >= 0");
    if (hasLength && ! readSeconds (command, "length", length, true))
        return fail ("clip " + id + ": length must be > 0");

    if (destination != nullptr && clip->getClipTrack() != destination)
        if (! clip->moveTo (*destination))
            return fail ("Tracktion would not move clip " + id + " to track " + command["trackId"].toString());

    // Write the whole position in absolute terms. The content moves with the clip and its
    // offset into the file stays as it is. Not setStart/setLength: their preserveSync flags
    // mean opposite things (setEnd with preserveSync false moves the offset), and the
    // contract example 02 caught a shortened clip playing a different part of its file.
    if (hasStart || hasLength)
    {
        auto position = clip->getPosition();
        const auto newStart = hasStart ? te::TimePosition::fromSeconds (start) : position.getStart();
        const auto newLength = hasLength ? te::TimeDuration::fromSeconds (length) : position.getLength();
        position.time = { newStart, newLength };
        clip->setPosition (position);
    }

    return juce::Result::ok();
}

juce::Result Projector::removeClip (const juce::String& id)
{
    auto clip = findClip (id);
    if (clip == nullptr)
        return fail ("no clip " + id);

    clips.erase (id);
    clip->removeFromParent();
    return juce::Result::ok();
}

juce::Result Projector::reset()
{
    for (auto track : te::getAudioTracks (edit))
        if (track->state.hasProperty (Host::modelIdProperty))
            edit.deleteTrack (track);

    tracks.clear();
    clips.clear();
    return juce::Result::ok();
}

juce::Result Projector::applyOne (const juce::var& command)
{
    const auto op = command["op"].toString();

    if (op == "track.add")      return addTrack (command["id"].toString(), command["after"]);
    if (op == "track.move")     return moveTrack (command["id"].toString(), command["after"]);
    if (op == "track.remove")   return removeTrack (command["id"].toString());
    if (op == "clip.add")       return addClip (command);
    if (op == "clip.set")       return setClip (command);
    if (op == "clip.remove")    return removeClip (command["id"].toString());
    if (op == "reset")          return reset();

    return fail ("unknown command '" + op + "'");
}

Projector::ApplyResult Projector::apply (const juce::var& commands)
{
    ApplyResult result;

    if (! commands.isArray())
    {
        result.error = "commands must be an array";
        result.failedAt = 0;
        return result;
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();

    for (int i = 0; i < commands.size(); ++i)
    {
        const auto r = applyOne (commands[i]);
        if (r.failed())
        {
            result.failedAt = i;
            result.error = r.getErrorMessage();
            break;
        }
        ++result.applied;
    }

    const auto applied = juce::Time::getMillisecondCounterHiRes();
    edit.dispatchPendingUpdatesSynchronously();
    const auto rebuilt = juce::Time::getMillisecondCounterHiRes();

    result.applyMs = applied - started;
    result.rebuildMs = rebuilt - applied;
    return result;
}

juce::var Projector::readback() const
{
    juce::Array<juce::var> trackList;

    for (auto track : te::getAudioTracks (edit))
    {
        if (! track->state.hasProperty (Host::modelIdProperty))
            continue;  // the probe

        juce::Array<te::Clip*> trackClips (track->getClips());
        std::sort (trackClips.begin(), trackClips.end(), [] (te::Clip* a, te::Clip* b)
        {
            return a->state[Host::modelIdProperty].toString().compare (b->state[Host::modelIdProperty].toString()) < 0;
        });

        juce::Array<juce::var> clipList;

        for (auto clip : trackClips)
        {
            auto o = std::make_unique<juce::DynamicObject>();
            const auto position = clip->getPosition();
            o->setProperty ("id", clip->state[Host::modelIdProperty]);
            o->setProperty ("start", position.getStart().inSeconds());
            o->setProperty ("length", position.getLength().inSeconds());
            o->setProperty ("offset", position.getOffset().inSeconds());
            o->setProperty ("sampleHash", clip->state[Host::sampleHashProperty]);

            if (auto wave = dynamic_cast<te::WaveAudioClip*> (clip))
                o->setProperty ("file", wave->getOriginalFile().getFullPathName());

            clipList.add (juce::var (o.release()));
        }

        auto t = std::make_unique<juce::DynamicObject>();
        t->setProperty ("id", track->state[Host::modelIdProperty]);
        t->setProperty ("clips", clipList);
        trackList.add (juce::var (t.release()));
    }

    auto doc = std::make_unique<juce::DynamicObject>();
    doc->setProperty ("tracks", trackList);
    return juce::var (doc.release());
}

} // namespace projection
