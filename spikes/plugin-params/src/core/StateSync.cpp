// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.

#include "StateSync.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace psync
{

StateSync::StateSync (ParamSync& p, ReadParams rp, StateOptions o)
    : params (p), readParams (std::move (rp)), opts (o)
{
}

void StateSync::loaded (const std::string& hash, std::uint64_t size)
{
    agreed = { hash, size, Stamp { 0, params.member() } };
    base = hash;
    lastHash = hash;
    pending.reset();
    want = {};
    pendingApply.reset();
}

//==============================================================================
void StateSync::hint (double nowMs)
{
    ++counters.hints;

    // A plugin hints as it takes a loaded state (a restart, setDirty), and on a
    // busy machine those hints can come seconds later. While it settles, a hint
    // still schedules a read, but it is not taken as a sign of a change here.
    if (nowMs >= settleUntilMs)
        lastHintMs = nowMs;
    const double at = nowMs + opts.hintDelayMs;
    readAtMs = readAtMs ? std::min (*readAtMs, at) : at;
}

void StateSync::localActivity (double nowMs)  { lastLocalMs = nowMs; }
void StateSync::remoteActivity (double nowMs, bool heldElsewhere)
{
    // Values set back after a load, or its late echoes set right, are part of
    // the load: the plugin keeps settling for a while after the last of them.
    // Someone else's held control is a person playing, not the load: it does
    // not extend it, and nothing extends it past 10 s after the load.
    if (nowMs < settleUntilMs && ! heldElsewhere)
        settleUntilMs = std::min (std::max (settleUntilMs, nowMs + opts.settleMs), appliedAtMs + 10000.0);

    lastRemoteMs = nowMs;
    if (! heldElsewhere)
        lastRemoteLooseMs = nowMs;
}

double StateSync::pollIntervalMs() const
{
    if (opts.pollMs <= 0.0)
        return std::numeric_limits<double>::infinity();

    const double average = counters.reads > 0 ? counters.readMsTotal / (double) counters.reads : 0.0;
    const double budget = opts.pollBudget > 0.0 ? average / opts.pollBudget : 0.0;
    return std::max (opts.pollMs, budget);
}

bool StateSync::readDue (double nowMs, bool pollAllowed) const
{
    if (readAtMs && nowMs >= *readAtMs)
        return true;

    // A change waits to be seen unchanged for the quiet period before it is announced.
    if (pending && ! pending->confirmed && nowMs >= confirmAtMs() && lastReadMs < confirmAtMs())
        return true;

    return pollAllowed && ! noisy && nowMs - lastReadMs >= pollIntervalMs();
}

bool StateSync::localEvidenceSince (double t) const
{
    // A hint within a moment of values set from elsewhere is the plugin answering
    // them (on a busy machine, the late reports of a load), not a change made here.
    const bool hintHere = lastHintMs > t && std::abs (lastHintMs - lastRemoteMs) >= 50.0;
    return lastLocalMs > t || hintHere;
}

double StateSync::confirmAtMs() const
{
    // Someone else's held control can keep moving for as long as they hold it;
    // waiting for it would hold back a change here (a preset) until they let go,
    // and then race their own. A held parameter is left to parameter sync on
    // every copy that loads this state (ParamSync::adoptState), so a value the
    // bytes are a block behind on does no harm. Loose changes from elsewhere
    // still count.
    const double lastParam = std::max (lastLocalMs, lastRemoteLooseMs);
    return std::max (pending ? pending->changedMs + opts.quietMs : 0.0, lastParam + opts.paramQuietMs);
}

bool StateSync::prompted (double nowMs) const
{
    return nowMs - lastHintMs <= opts.promptMs || nowMs - lastLocalMs <= opts.promptMs;
}

void StateSync::observed (const std::string& hash, std::uint64_t size, double readMs, double nowMs)
{
    ++counters.reads;
    counters.readMsTotal += readMs;
    counters.readMsMax = std::max (counters.readMsMax, readMs);
    prevReadMs = lastReadMs;
    lastReadMs = nowMs;
    readAtMs.reset();

    // A state that differs on every read while nothing is done here changes by
    // itself (a counter, a time stamp, a random seed): reading it finds nothing.
    const bool changedSinceLastRead = hash != lastHash;
    lastHash = hash;

    // A change since parameters arrived from someone else is theirs, not the
    // plugin's own: counting it made a busy session look noisy (finding 18).
    const bool remoteSinceLastRead = lastRemoteMs > prevReadMs;

    if (! changedSinceLastRead)
        unpromptedStreak = 0;
    else if (! prompted (nowMs) && nowMs >= settleUntilMs && ! remoteSinceLastRead)
    {
        ++counters.unprompted;
        if (++unpromptedStreak >= opts.noisyAfter && ! noisy)
        {
            // Those changes were the plugin's own: drop the one waiting to go out.
            noisy = true;
            pending.reset();
            base = hash;
            return;
        }
    }

    if (pending && noisy)
    {
        // A noisy state never reads the same twice, so stillness cannot be seen
        // in its hash. The first read once nothing has been done here for the
        // quiet period is announced, bytes and parameters taken together:
        // never bytes read earlier, in the middle of an edit (finding 18).
        if (nowMs >= confirmAtMs() && lastLocalMs <= pending->changedMs && lastHintMs <= pending->changedMs)
        {
            pending->hash = hash;
            pending->size = size;
            pending->confirmed = true;
            pending->params = params.snapshot();
            return;
        }

        const double lastTouch = std::max (lastLocalMs, lastHintMs);
        pending = Pending { hash, size, std::max (pending->changedMs, std::min (lastTouch, nowMs)), false, {} };

        // Its hash will differ again, so read again when the quiet period has passed.
        readAtMs = std::max (confirmAtMs(), nowMs + opts.hintDelayMs);
        return;
    }

    if (noisy && changedSinceLastRead && ! prompted (nowMs))
    {
        base = hash;
        pending.reset();
        return;
    }

    if (pending && hash != pending->hash && hash != base && lastRemoteMs > prevReadMs
        && ! localEvidenceSince (prevReadMs))
    {
        // The change here is still, but parameters from someone else keep moving
        // its state (a knob they hold while this copy loaded a preset). Follow
        // them without restarting the quiet period.
        pending->hash = hash;
        pending->size = size;

        if (nowMs >= confirmAtMs())
        {
            pending->confirmed = true;
            pending->params = params.snapshot();
        }
        return;
    }

    if (pending)
    {
        if (hash == pending->hash)
        {
            ++counters.unchangedReads;

            if (nowMs >= confirmAtMs() && ! pending->confirmed)
            {
                // Unchanged for the quiet period, and no parameter has moved for
                // long enough to have reached the saved state: what is known now
                // is what the bytes hold.
                pending->confirmed = true;
                pending->params = params.snapshot();
            }
            return;
        }

        if (hash == base)
        {
            // Changed and changed back before anyone was told.
            ++counters.unchangedReads;
            pending.reset();
            return;
        }

        pending = Pending { hash, size, nowMs, false, {} };
        return;
    }

    if (hash == base)
    {
        ++counters.unchangedReads;
        return;
    }

    // Only something a person did ends settling early. A plugin often calls
    // setDirty, or restarts, as it finishes loading: that is not a new change.
    const bool touchedSinceApply = lastLocalMs > appliedAtMs;

    if (nowMs < settleUntilMs && ! touchedSinceApply)
    {
        // The plugin is still settling after a state from someone else.
        ++counters.absorbedSettling;
        base = hash;
        return;
    }

    if (lastRemoteMs > prevReadMs && ! localEvidenceSince (prevReadMs))
    {
        // Parameters arrived from someone else since the last read, and nothing
        // was done here: the state changed because of them, and parameter sync
        // has carried them already.
        ++counters.absorbedRemoteParams;
        base = hash;
        return;
    }

    ++counters.localChanges;
    pending = Pending { hash, size, nowMs, false, {} };
}

std::optional<StateOffer> StateSync::flush (double, bool gestureHeld)
{
    if (! pending || gestureHeld)
        return std::nullopt;

    // A newer state from someone else is on its way or waiting to load: it
    // replaces the hidden part here, and what was changed here in parameters is
    // set back on top of it once it is loaded. Announcing this copy's whole
    // state now would replace their preset with ours (decision on #13).
    if (isWaiting())
        return std::nullopt;

    // Only a confirmed change goes out: its bytes and its parameter snapshot were
    // read together, with nothing moving. Bytes from earlier with parameters from
    // now would make whoever loads them take stale values as silent changes and
    // undo the last edit everywhere (finding 18).
    if (! pending->confirmed)
        return std::nullopt;

    auto p = std::move (*pending);
    pending.reset();
    return announce (p.hash, p.size, std::move (p.params));
}

StateOffer StateSync::announceNow (const std::string& hash, std::uint64_t size)
{
    pending.reset();
    return announce (hash, size, params.snapshot());
}

StateOffer StateSync::announce (const std::string& hash, std::uint64_t size, ParamSync::Snapshot snap)
{
    agreed = { hash, size, params.issueStamp() };
    base = hash;
    want = {};
    pendingApply.reset();
    ++counters.announced;
    return { agreed, std::move (snap) };
}

std::optional<StateRef> StateSync::current() const
{
    if (agreed.hash.empty())
        return std::nullopt;
    return agreed;
}

//==============================================================================
Step StateSync::receive (const std::string& from, const StateRef& ref, const std::string& currentHash, double nowMs)
{
    ++counters.received;
    params.observe (ref.stamp);

    if (! (agreed.stamp < ref.stamp))
    {
        ++counters.stale;
        return {};
    }

    agreed = ref;

    // A change here that has not gone out yet does not compete with it. If it
    // was parameters (a knob turned, perhaps still held), they are set back on
    // top once the newcomer is loaded, which waits until nobody here holds a
    // control. If it was a hidden edit (an LFO shape), it is lost to the
    // newcomer: there is no merging inside a state (decision on #13).
    if (pending)
        ++counters.replacedHere;

    if (currentHash == ref.hash)
        return matchedHere();

    pendingApply.reset();
    want = { ref.hash, from, std::nullopt };

    if (opts.recheckMs > 0.0)
    {
        ++counters.rechecks;
        want.recheckAtMs = nowMs + opts.recheckMs;
        return { Need::recheck, ref.hash, from, {} };
    }

    ++counters.fetches;
    return { Need::fetch, ref.hash, from, {} };
}

bool StateSync::recheckDue (double nowMs) const
{
    return want.recheckAtMs && nowMs >= *want.recheckAtMs;
}

Step StateSync::recheck (const std::string& currentHash, double)
{
    if (want.hash.empty())
        return {};

    want.recheckAtMs.reset();

    if (currentHash == want.hash)
        return matchedHere();

    ++counters.fetches;
    return { Need::fetch, want.hash, want.from, {} };
}

Step StateSync::matchedHere()
{
    // The plugin already saves exactly these bytes. Any parameter they changed
    // silently arrives as a parameter change from whoever applied them.
    ++counters.matched;
    base = agreed.hash;
    pending.reset();
    want = {};
    pendingApply.reset();
    return {};
}

bool StateSync::arrived (const std::string& hash, ParamSync::Snapshot sent)
{
    if (want.hash != hash)
    {
        ++counters.superseded;
        return false;
    }

    pendingApply = hash;
    sentParams = std::move (sent);
    want = {};
    return true;
}

std::optional<std::string> StateSync::applyDue (bool gestureHeld)
{
    if (! pendingApply)
        return std::nullopt;

    if (gestureHeld)
    {
        ++counters.appliedDeferred;
        return std::nullopt;
    }

    auto h = std::move (*pendingApply);
    pendingApply.reset();
    return h;
}

std::vector<Apply> StateSync::applied (const std::string& readbackHash, std::uint64_t, double nowMs)
{
    ++counters.applied;

    if (readbackHash != agreed.hash)
        ++counters.readbackDiffers;

    base = readbackHash;
    pending.reset();
    // Long enough for at least two polls, so a rewrite after loading is seen inside it.
    const double poll = pollIntervalMs();
    settleUntilMs = nowMs + std::max (opts.settleMs, std::isfinite (poll) ? 2.0 * poll : 0.0);
    appliedAtMs = nowMs;
    lastRemoteMs = nowMs;

    auto adopted = params.adoptState (sentParams, agreed.stamp, readParams ? readParams() : std::map<std::string, float> {}, nowMs);
    sentParams.clear();
    counters.paramsPutBack += adopted.apply.size();
    counters.paramsDiscovered += adopted.discovered.size();
    return std::move (adopted.apply);
}

void StateSync::memberLeft (const std::string& member)
{
    if (want.from == member)
        want = {};
}

//==============================================================================
// SHA-256 (FIPS 180-4), small and dependency-free so the rules stay pure.

namespace
{
constexpr std::array<std::uint32_t, 64> k {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constexpr std::uint32_t rotr (std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress (std::array<std::uint32_t, 8>& h, const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> w {};

    for (int i = 0; i < 16; ++i)
        w[(std::size_t) i] = (std::uint32_t) block[i * 4] << 24 | (std::uint32_t) block[i * 4 + 1] << 16
                           | (std::uint32_t) block[i * 4 + 2] << 8 | (std::uint32_t) block[i * 4 + 3];

    for (std::size_t i = 16; i < 64; ++i)
    {
        const auto s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

    for (std::size_t i = 0; i < 64; ++i)
    {
        const auto t1 = hh + (rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const auto t2 = (rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
} // namespace

std::string sha256Hex (const void* data, std::size_t size)
{
    std::array<std::uint32_t, 8> h { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    const auto* bytes = static_cast<const std::uint8_t*> (data);
    std::size_t i = 0;

    for (; i + 64 <= size; i += 64)
        compress (h, bytes + i);

    std::array<std::uint8_t, 128> tail {};
    const std::size_t rest = size - i;
    if (rest > 0)
        std::memcpy (tail.data(), bytes + i, rest);
    tail[rest] = 0x80;
    const std::size_t tailBlocks = rest + 1 + 8 <= 64 ? 1 : 2;
    const std::uint64_t bits = (std::uint64_t) size * 8u;

    for (int b = 0; b < 8; ++b)
        tail[tailBlocks * 64 - 1 - (std::size_t) b] = (std::uint8_t) (bits >> (8 * b));

    for (std::size_t b = 0; b < tailBlocks; ++b)
        compress (h, tail.data() + b * 64);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve (64);

    for (auto word : h)
        for (int shift = 28; shift >= 0; shift -= 4)
            out.push_back (hex[(word >> shift) & 0xf]);

    return out;
}

//==============================================================================
namespace
{
std::string lower (std::string s)
{
    for (auto& c : s)
        c = (char) std::tolower ((unsigned char) c);
    return s;
}

bool startsWithAt (const std::string& s, std::size_t at, const char* prefix)
{
    const auto n = std::strlen (prefix);
    return at + n <= s.size() && lower (s.substr (at, n)) == prefix;
}

bool isTerminator (char c)
{
    return c == '"' || c == '<' || c == '>' || c == '|' || c == '\'' || (unsigned char) c < 0x20;
}

const char* const extensions[] {
    ".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".wv", ".rex", ".rx2", ".caf",
    ".nki", ".nkm", ".nkc", ".nkx", ".ncw", ".sfz", ".sf2", ".exs", ".wt", ".vital", ".fxp", ".fxb",
    ".vstpreset", ".h2p", ".serumpreset", ".serumpack", ".mid", ".midi"
};

/** Absolute-path starts at `i` in `s`. */
bool pathStartsAt (const std::string& s, std::size_t i)
{
    if (i + 3 <= s.size() && std::isalpha ((unsigned char) s[i]) && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/')
        && (i == 0 || ! std::isalnum ((unsigned char) s[i - 1])))
        return true;

    if (startsWithAt (s, i, "\\\\") && i + 2 < s.size() && std::isalnum ((unsigned char) s[i + 2]))
        return true;

    if (startsWithAt (s, i, "file://"))
        return true;

    for (auto* root : { "/users/", "/home/", "/volumes/", "/library/", "/applications/", "/private/", "/var/", "/tmp/", "/opt/", "/mnt/" })
        if (startsWithAt (s, i, root) && (i == 0 || isTerminator (s[i - 1]) || s[i - 1] == ' ' || s[i - 1] == '=' || s[i - 1] == ':'))
            return true;

    return false;
}

bool endsWithExtension (const std::string& s)
{
    const auto low = lower (s);
    for (auto* ext : extensions)
    {
        const auto n = std::strlen (ext);
        if (low.size() > n && low.compare (low.size() - n, n, ext) == 0)
            return true;
    }
    return false;
}

bool isNameChar (char c)
{
    return std::isalnum ((unsigned char) c) || std::strchr (" -_.()&#+,[]!~@", c) != nullptr;
}

/** Filters out the paths that bytes from compressed or random data make by chance. */
bool plausiblePath (const std::string& s)
{
    int separators = 0, letters = 0;
    for (char c : s)
    {
        if (c == '\\' || c == '/')
            ++separators;
        else if (std::isalpha ((unsigned char) c))
            ++letters;
        else if (! isNameChar (c) && c != ':')
            return false;
    }
    return s.size() >= 8 && letters >= 4 && (separators >= 2 || endsWithExtension (s));
}

void scanRun (const std::string& run, std::vector<std::string>& out, std::set<std::string>& seen, std::size_t maxResults)
{
    auto add = [&] (std::string candidate, bool isPath)
    {
        while (! candidate.empty() && (candidate.back() == ' ' || candidate.back() == '.' || candidate.back() == ','))
            candidate.pop_back();

        if (isPath ? ! plausiblePath (candidate) : candidate.size() < 6)
            return;

        if (out.size() < maxResults && seen.insert (candidate).second)
            out.push_back (std::move (candidate));
    };

    std::size_t i = 0;

    while (i < run.size())
    {
        if (pathStartsAt (run, i))
        {
            std::size_t j = i;
            while (j < run.size() && ! isTerminator (run[j]))
                ++j;
            add (run.substr (i, j - i), true);
            i = j;
            continue;
        }
        ++i;
    }

    // Names with a known extension, wherever they are, if not already part of a path found above.
    const auto low = lower (run);

    for (auto* ext : extensions)
    {
        const auto n = std::strlen (ext);

        for (auto at = low.find (ext); at != std::string::npos; at = low.find (ext, at + n))
        {
            const auto end = at + n;
            if (end < low.size() && std::isalnum ((unsigned char) low[end]))
                continue;

            std::size_t start = at;
            while (start > 0 && isNameChar (run[start - 1]) && run[start - 1] != '=')
                --start;
            while (start < at && run[start] == ' ')
                ++start;

            const auto candidate = run.substr (start, end - start);
            bool inside = false;
            for (auto& found : out)
                if (found.find (candidate) != std::string::npos)
                    inside = true;

            int letters = 0;
            for (std::size_t c = start; c < at; ++c)
                letters += std::isalpha ((unsigned char) run[c]) ? 1 : 0;

            if (! inside && letters >= 2)
                add (candidate, false);
        }
    }
}
} // namespace

std::vector<std::string> findPathLikeStrings (const void* data, std::size_t size, std::size_t maxResults)
{
    const auto* b = static_cast<const std::uint8_t*> (data);
    std::vector<std::string> out;
    std::set<std::string> seen;

    // 8-bit text: printable ASCII and UTF-8 continuation bytes.
    std::string run;
    for (std::size_t i = 0; i <= size; ++i)
    {
        const bool text = i < size && (b[i] >= 0x20 || b[i] == '\t');
        if (text)
        {
            run.push_back ((char) b[i]);
            continue;
        }
        if (run.size() >= 5)
            scanRun (run, out, seen, maxResults);
        run.clear();
    }

    // UTF-16 little-endian text (Windows plugins often save paths this way), at both alignments.
    for (std::size_t align = 0; align < 2; ++align)
    {
        run.clear();
        for (std::size_t i = align; i + 1 <= size; i += 2)
        {
            const bool text = i + 1 < size && b[i + 1] == 0 && b[i] >= 0x20 && b[i] < 0x7f;
            if (text)
            {
                run.push_back ((char) b[i]);
                continue;
            }
            if (run.size() >= 5)
                scanRun (run, out, seen, maxResults);
            run.clear();
        }
    }

    return out;
}

std::string redactHome (const std::string& path)
{
    auto cutAfterName = [&path] (std::size_t nameStart) -> std::string
    {
        auto end = path.find_first_of ("\\/", nameStart);
        return "~" + (end == std::string::npos ? std::string() : path.substr (end));
    };

    const auto low = lower (path);

    if (low.size() > 9 && std::isalpha ((unsigned char) low[0]) && low[1] == ':' && (low.compare (2, 7, "\\users\\") == 0 || low.compare (2, 7, "/users/") == 0))
        return cutAfterName (9);
    if (low.rfind ("file:///users/", 0) == 0)
        return cutAfterName (14);
    if (low.rfind ("/users/", 0) == 0)
        return cutAfterName (7);
    if (low.rfind ("/home/", 0) == 0)
        return cutAfterName (6);

    return path;
}

//==============================================================================
ApplyReport classifyApply (const std::string& receivedHash, const std::string& hashBefore, const std::string& readbackHash,
                           const std::map<std::string, float>& sent, const std::map<std::string, float>& here, float tolerance)
{
    ApplyReport r;

    for (const auto& [id, value] : sent)
    {
        auto it = here.find (id);
        if (it == here.end())
            continue;

        ++r.compared;

        if (std::abs (it->second - value) > tolerance && r.differing.size() < 20)
            r.differing.push_back (id);
    }

    if (readbackHash == receivedHash)
        r.outcome = ApplyOutcome::exact;
    else if (readbackHash == hashBefore && receivedHash != hashBefore)
        r.outcome = r.differing.empty() && r.compared > 0 ? ApplyOutcome::alreadyHeld : ApplyOutcome::unchanged;
    else if (r.differing.empty())
        r.outcome = ApplyOutcome::resaved;
    else
        r.outcome = ApplyOutcome::paramsDiffer;

    return r;
}

const char* toString (ApplyOutcome o)
{
    switch (o)
    {
        case ApplyOutcome::exact:        return "loaded exactly";
        case ApplyOutcome::resaved:      return "loaded, saved back differently, parameters match";
        case ApplyOutcome::paramsDiffer: return "loaded with different parameters (a silent difference)";
        case ApplyOutcome::unchanged:    return "not loaded: the plugin's state did not change";
        case ApplyOutcome::alreadyHeld:  return "nothing changed, parameters match the sender's: already held, or a hidden part refused";
    }
    return "?";
}

} // namespace psync
