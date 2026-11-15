// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.

#include "StateHost.h"

namespace spike
{

namespace
{
constexpr std::size_t cacheSize = 6;
constexpr double sameValueHintGapMs = 100.0;   // one hint per burst of unchanged reports

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

juce::MemoryBlock zip (const juce::MemoryBlock& in)
{
    juce::MemoryOutputStream out;
    {
        juce::GZIPCompressorOutputStream z (out, 6, juce::GZIPCompressorOutputStream::windowBitsRaw);
        z.write (in.getData(), in.getSize());
    }
    return out.getMemoryBlock();
}

juce::MemoryBlock unzip (const juce::MemoryBlock& in)
{
    juce::MemoryInputStream source (in, false);
    juce::GZIPDecompressorInputStream z (&source, false, juce::GZIPDecompressorInputStream::deflateFormat);
    juce::MemoryOutputStream out;
    out.writeFromInputStream (z, -1);
    return out.getMemoryBlock();
}

std::string hashOf (const juce::MemoryBlock& b) { return psync::sha256Hex (b.getData(), b.getSize()); }
juce::String shortHash (const std::string& h) { return juce::String (h.substr (0, 8)); }

juce::String sizeText (std::uint64_t bytes)
{
    if (bytes >= 1024 * 1024)
        return juce::String ((double) bytes / (1024.0 * 1024.0), 2) + " MiB";
    if (bytes >= 1024)
        return juce::String ((double) bytes / 1024.0, 1) + " KiB";
    return juce::String ((juce::int64) bytes) + " bytes";
}
} // namespace

StateHost::StateHost (psync::ParamSync& p, const juce::String& memberId)
    : params (p), member (memberId),
      sync (p, [this] { return readParams ? readParams() : std::map<std::string, float> {}; })
{
    // Polling, when the window allows it, starts at twice a second and slows
    // down for plugins whose state is slow to read (StateOptions::pollBudget).
    auto o = sync.options();
    o.pollMs = 500.0;
    sync.setOptions (o);
}

StateHost::~StateHost()
{
    detach();
}

void StateHost::attach (juce::AudioPluginInstance& i, std::function<std::map<std::string, float>()> rp)
{
    detach();
    instance = &i;
    readParams = std::move (rp);
    instance->addListener (this);

    // Two reads straight after each other: a state that differs already changes
    // by itself (a counter or a time stamp in it), and reading it finds nothing.
    last = read();
    const auto again = read();
    determinism = last.hash == again.hash;
    sync.loaded (last.hash, last.bytes.getSize());
    sync.setNoisy (! *determinism);
    remember (last.hash, last.bytes, params.snapshot());

    info ("state is " + sizeText (last.bytes.getSize()) + " (component " + sizeText (last.componentBytes) + ", controller "
          + sizeText (last.controllerBytes) + "), read in " + juce::String (last.readMs, 2) + " ms"
          + (*determinism ? "" : "; it changes between two reads with nothing done, so it is not polled"));
}

void StateHost::detach()
{
    if (instance != nullptr)
        instance->removeListener (this);

    instance = nullptr;
}

//==============================================================================
SavedState StateHost::read() const
{
    return instance != nullptr ? read (*instance) : SavedState {};
}

SavedState StateHost::read (juce::AudioPluginInstance& i)
{
    SavedState s;
    const auto t0 = nowMs();
    i.getStateInformation (s.bytes);
    s.readMs = nowMs() - t0;
    s.hash = hashOf (s.bytes);
    const auto [component, controller] = splitVst3State (s.bytes);
    s.componentBytes = component.getSize();
    s.controllerBytes = controller.getSize();
    return s;
}

std::pair<juce::MemoryBlock, juce::MemoryBlock> StateHost::splitVst3State (const juce::MemoryBlock& b)
{
    std::pair<juce::MemoryBlock, juce::MemoryBlock> out;

    if (auto xml = juce::AudioProcessor::getXmlFromBinary (b.getData(), (int) b.getSize()); xml != nullptr && xml->hasTagName ("VST3PluginState"))
    {
        if (auto* c = xml->getChildByName ("IComponent"))
            out.first.fromBase64Encoding (c->getAllSubText());
        if (auto* c = xml->getChildByName ("IEditController"))
            out.second.fromBase64Encoding (c->getAllSubText());
    }

    return out;
}

std::vector<std::string> StateHost::pathsIn (const juce::MemoryBlock& b)
{
    auto [component, controller] = splitVst3State (b);

    if (component.isEmpty() && controller.isEmpty())
        return psync::findPathLikeStrings (b.getData(), b.getSize());

    auto paths = psync::findPathLikeStrings (component.getData(), component.getSize());
    for (auto& p : psync::findPathLikeStrings (controller.getData(), controller.getSize()))
        if (std::find (paths.begin(), paths.end(), p) == paths.end())
            paths.push_back (p);
    return paths;
}

//==============================================================================
void StateHost::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d)
{
    // JUCE may call this on any thread; the counters and the hint are taken on the message thread.
    const bool dirty = d.nonParameterStateChanged;
    const bool restart = d.programChanged || d.parameterInfoChanged;

    if (! dirty && ! restart)
        return;

    juce::MessageManager::callAsync ([weak = juce::WeakReference<StateHost> (this), dirty, restart]
    {
        if (auto* self = weak.get())
        {
            if (dirty)
                ++self->stats.hintSetDirty;
            if (restart)
                ++self->stats.hintRestart;
            self->sync.hint (nowMs());
        }
    });
}

void StateHost::sameValueReport (double now)
{
    // Serum 2 floods the host with unchanged parameter values while its LFOs are
    // edited (#12 finding 14): no change to send, but a sign the state moved.
    if (now - lastSameValueHintMs < sameValueHintGapMs)
        return;

    lastSameValueHintMs = now;
    ++stats.hintSameValues;
    sync.hint (now);
}

void StateHost::windowMouse (bool downInside, double now)
{
    if (mouseWasDown && ! downInside)
    {
        // The person let go of the mouse in the plugin's window: whatever they
        // did there (a preset, an LFO point, a wavetable) is in the state now.
        ++stats.hintWindow;
        sync.localActivity (now);
        sync.hint (now);
    }

    mouseWasDown = downInside;
}

void StateHost::checkNow (double now)
{
    ++stats.hintManual;
    sync.localActivity (now);
    sync.hint (now);
}

void StateHost::sendNow (double now)
{
    if (instance == nullptr)
        return;

    const auto s = read();
    last = s;
    announce (sync.announceNow (s.hash, s.bytes.getSize()), s, now);
}

bool StateHost::isTransferring() const
{
    return ! outgoing.empty() || incoming.has_value();
}

//==============================================================================
void StateHost::tick (double now)
{
    if (instance == nullptr || ! enabled)
        return;

    const bool held = gestureHeld && gestureHeld();
    const bool pollAllowed = detect == Detect::pollAlways || (detect == Detect::pollWindow && windowOpen);

    if (sync.readDue (now, pollAllowed))
        readAndObserve (now);

    if (sync.recheckDue (now))
    {
        const auto s = read();
        auto step = sync.recheck (s.hash, now);

        if (step.need == psync::Need::fetch)
        {
            StateMessage want;
            want.type = "stateWant";
            want.from = member;
            want.to = step.from;
            want.hash = step.hash;
            ++stats.wantsSent;
            incoming = Incoming { want.hash, want.to, {}, 0, 0, {}, now };
            if (send)
                send (encodeState (want));
        }
    }

    if (auto h = sync.applyDue (held))
        apply (*h, now);

    if (auto offer = sync.flush (now, held))
    {
        // The bytes are the last ones read with this hash.
        if (offer->ref.hash == last.hash)
            announce (*offer, last, now);
        else
        {
            const auto s = read();
            announce (*offer, s, now);
        }
    }

    // Send chunks, a limited amount per tick.
    int budget = bytesPerTick;

    while (! outgoing.empty() && budget > 0)
    {
        auto& o = outgoing.front();
        const auto offset = (std::size_t) o.next * (std::size_t) chunkBytes;
        const auto n = std::min ((std::size_t) chunkBytes, o.compressed.getSize() - offset);

        StateMessage m;
        m.type = "stateData";
        m.from = member;
        m.to = o.to;
        m.hash = o.hash;
        m.chunk = o.next;
        m.chunks = o.count;
        m.compressedSize = o.compressed.getSize();
        m.data = juce::MemoryBlock (static_cast<const char*> (o.compressed.getData()) + offset, n);
        if (o.next == 0)
            m.params = o.params;

        const auto text = encodeState (m);
        stats.dataBytesSent += (std::uint64_t) text.getNumBytesAsUTF8();
        ++stats.chunksSent;
        budget -= (int) n;

        if (send)
            send (text);

        if (++o.next >= o.count)
            outgoing.pop_front();
    }
}

void StateHost::readAndObserve (double now)
{
    last = read();
    sync.observed (last.hash, last.bytes.getSize(), last.readMs, now);

    // Keep the bytes of a change, so they can be sent if it is announced. The
    // parameters that go with them are the ones the rules took when they
    // confirmed it (StateOffer), set when it is announced.
    if (sync.hasLocalChange() && cache.count (last.hash) == 0)
        remember (last.hash, last.bytes, {});
}

void StateHost::announce (const psync::StateOffer& offer, const SavedState& s, double)
{
    remember (offer.ref.hash, s.bytes, offer.params);
    stats.lastRawSize = s.bytes.getSize();

    StateMessage m;
    m.type = "state";
    m.from = member;
    m.ref = offer.ref;
    ++stats.announcesSent;
    note ("announced state " + shortHash (offer.ref.hash) + " (" + sizeText (s.bytes.getSize()) + ") with stamp "
          + juce::String ((juce::int64) offer.ref.stamp.counter));

    if (send)
        send (encodeState (m));
}

void StateHost::remember (const std::string& hash, const juce::MemoryBlock& bytes, const psync::ParamSync::Snapshot& snap)
{
    if (cache.count (hash) == 0)
    {
        cacheOrder.push_back (hash);

        while (cacheOrder.size() > cacheSize)
        {
            cache.erase (cacheOrder.front());
            cacheOrder.pop_front();
        }
    }

    cache[hash] = { bytes, snap };
}

void StateHost::serve (const juce::String& to, const juce::String& hash)
{
    auto it = cache.find (hash.toStdString());

    if (it == cache.end())
    {
        ++stats.wantsMissing;
        StateMessage m;
        m.type = "stateMissing";
        m.from = member;
        m.to = to;
        m.hash = hash;
        if (send)
            send (encodeState (m));
        return;
    }

    ++stats.wantsServed;
    Outgoing o;
    o.to = to;
    o.hash = hash;
    o.compressed = zip (it->second.bytes);
    o.params = it->second.params;
    o.count = std::max (1, (int) ((o.compressed.getSize() + (std::size_t) chunkBytes - 1) / (std::size_t) chunkBytes));
    stats.lastRawSize = it->second.bytes.getSize();
    stats.lastCompressedSize = o.compressed.getSize();
    outgoing.push_back (std::move (o));
}

//==============================================================================
void StateHost::receive (const StateMessage& m, double now)
{
    if (instance == nullptr || m.from == member || (m.to.isNotEmpty() && m.to != member))
        return;

    if (m.type == "state")
    {
        ++stats.announcesReceived;
        const auto s = read();
        last = s;
        auto step = sync.receive (m.from.toStdString(), m.ref, s.hash, now);

        if (step.need == psync::Need::fetch)
        {
            StateMessage want;
            want.type = "stateWant";
            want.from = member;
            want.to = m.from;
            want.hash = step.hash;
            ++stats.wantsSent;
            incoming = Incoming { want.hash, want.to, {}, 0, 0, {}, now };
            if (send)
                send (encodeState (want));
        }
        return;
    }

    if (m.type == "stateWant")
    {
        serve (m.from, m.hash);
        return;
    }

    if (m.type == "stateMissing")
    {
        warn (m.from + " no longer has state " + m.hash.substring (0, 8) + "; this plugin may differ until the next change");
        if (incoming && incoming->hash == m.hash)
            incoming.reset();
        sync.memberLeft (m.from.toStdString());
        return;
    }

    if (m.type == "stateData")
    {
        stats.dataBytesReceived += (std::uint64_t) m.data.getSize();
        ++stats.chunksReceived;

        if (! incoming || incoming->hash != m.hash)
            incoming = Incoming { m.hash, m.from, {}, m.chunks, 0, {}, now };

        auto& in = *incoming;

        if (m.chunk != in.received)
        {
            warn ("state chunk " + juce::String (m.chunk) + " arrived out of order; dropped");
            incoming.reset();
            return;
        }

        if (m.chunk == 0)
        {
            in.params = m.params;
            in.expected = m.chunks;
        }

        in.compressed.append (m.data.getData(), m.data.getSize());
        ++in.received;

        if (in.received < in.expected)
            return;

        auto bytes = unzip (in.compressed);
        const auto hash = hashOf (bytes);
        stats.lastTransferMs = now - in.startedMs;
        stats.lastCompressedSize = in.compressed.getSize();
        stats.lastRawSize = bytes.getSize();
        auto snap = std::move (in.params);
        incoming.reset();

        if (hash != m.hash.toStdString())
        {
            ++stats.hashMismatches;
            warn ("state " + m.hash.substring (0, 8) + " arrived with the wrong hash; dropped");
            return;
        }

        note ("received state " + shortHash (hash) + ": " + sizeText (bytes.getSize()) + ", " + sizeText (stats.lastCompressedSize)
              + " compressed, in " + juce::String (stats.lastTransferMs, 0) + " ms from asking");

        if (sync.arrived (hash, snap))
        {
            arrivedBytes[hash] = { std::move (bytes), std::move (snap) };
            if (auto h = sync.applyDue (gestureHeld && gestureHeld()))
                apply (*h, now);
        }
    }
}

void StateHost::apply (const std::string& hash, double)
{
    auto it = arrivedBytes.find (hash);

    if (it == arrivedBytes.end())
        return;

    const auto cached = std::move (it->second);
    arrivedBytes.clear();

    const auto before = read();
    const auto t0 = nowMs();
    instance->setStateInformation (cached.bytes.getData(), (int) cached.bytes.getSize());
    const double ms = nowMs() - t0;
    ++stats.applies;
    stats.applyMsTotal += ms;
    stats.applyMsMax = std::max (stats.applyMsMax, ms);

    const auto after = read();
    last = after;
    const auto here = readParams ? readParams() : std::map<std::string, float> {};

    std::map<std::string, float> sentValues;
    for (const auto& [id, vs] : cached.params)
        sentValues[id] = vs.first;

    const auto report = psync::classifyApply (hash, before.hash, after.hash, sentValues, here);
    ++stats.outcomes[psync::toString (report.outcome)];

    // Reading and applying a large state takes a while: the rules' clock starts when it is done.
    auto again = sync.applied (after.hash, after.bytes.getSize(), nowMs());
    remember (after.hash, after.bytes, params.snapshot());

    juce::String line = "applied state " + shortHash (hash) + " in " + juce::String (ms, 1) + " ms: " + psync::toString (report.outcome);

    if (! report.differing.empty())
        line << " (" << (int) report.differing.size() << " of " << (int) report.compared << " parameters differ from the sender's)";

    if (report.outcome == psync::ApplyOutcome::unchanged || report.outcome == psync::ApplyOutcome::paramsDiffer)
        warn (line);
    else
        note (line);

    // Files the state names that this machine does not have.
    juce::StringArray missing;
    for (const auto& p : pathsIn (cached.bytes))
    {
        const juce::String path (p);
        const bool absolute = juce::File::isAbsolutePath (path) || path.startsWith ("file://");
        if (absolute && ! juce::File (path.startsWith ("file://") ? juce::URL (path).getLocalFile().getFullPathName() : path).exists())
            missing.add (juce::String (psync::redactHome (p)));
    }

    if (! missing.isEmpty())
    {
        stats.pathsMissingHere += (std::uint64_t) missing.size();
        warn (juce::String (missing.size()) + " file(s) the state refers to are not on this machine: " + missing.joinIntoString (", "));
    }

    if (! again.empty() && applyParams)
        applyParams (again);
}

void StateHost::peerJoined (double now)
{
    // Tell a newcomer the agreed state. Each member offers what its own plugin
    // saves for it, under the agreed stamp; the newcomer takes the first.
    if (instance == nullptr || ! enabled)
        return;

    auto current = sync.current();
    if (! current)
        return;

    const auto s = read();
    last = s;

    if (s.hash != sync.baseline())
        return;   // a change here is on its way; it will be announced

    remember (s.hash, s.bytes, params.snapshot());
    StateMessage m;
    m.type = "state";
    m.from = member;
    m.ref = { s.hash, s.bytes.getSize(), current->stamp };
    ++stats.announcesSent;
    juce::ignoreUnused (now);

    if (send)
        send (encodeState (m));
}

void StateHost::peerLeft (const juce::String& m)
{
    sync.memberLeft (m.toStdString());

    if (incoming && incoming->from == m)
        incoming.reset();

    std::erase_if (outgoing, [&m] (const Outgoing& o) { return o.to == m; });
}

//==============================================================================
void StateHost::note (const juce::String& s)
{
    events.add (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + s);
    while (events.size() > 200)
        events.remove (0);
    info (s);
}

void StateHost::info (const juce::String& s)
{
    if (log)
        log (member + ": " + s);
}

void StateHost::warn (const juce::String& s)
{
    events.add (juce::Time::getCurrentTime().toString (false, true, true, true) + "  WARNING " + s);
    if (log)
        log ("WARNING " + member + ": " + s);
}

} // namespace spike
