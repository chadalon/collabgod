// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Tests for the state sync rules in src/core, with no JUCE,
// engine or network: a simulated clock, simulated plugins and a simulated relay.
//
//   state_sync_tests            run everything
//   state_sync_tests --seeds N  run the random convergence test with N seeds per setting
//   state_sync_tests --trace <seed> <members> <setting>   print one run

#include "../src/core/StateSync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace psync;

namespace
{
int failures = 0;
int checks = 0;
const char* currentTest = "";

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++checks;                                                                         \
        if (! (cond)) {                                                                   \
            ++failures;                                                                   \
            std::printf ("FAIL %s: %s (line %d)\n", currentTest, #cond, __LINE__);        \
        }                                                                                 \
    } while (false)

void test (const char* name, const std::function<void()>& body)
{
    currentTest = name;
    const int before = failures;
    body();
    std::printf ("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

std::string sha (const std::string& s) { return sha256Hex (s.data(), s.size()); }

//==============================================================================
void helperTests()
{
    test ("SHA-256 matches the FIPS 180-4 examples", [] {
        CHECK (sha ("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK (sha ("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK (sha ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
               == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        CHECK (sha (std::string (1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        // Lengths around the padding boundary.
        CHECK (sha (std::string (55, 'x')) != sha (std::string (56, 'x')));
        CHECK (sha (std::string (64, 'x')).size() == 64);
    });

    test ("paths are found in 8-bit and UTF-16 text, and nothing in noise", [] {
        using namespace std::string_literals;
        std::string blob = "junk\x01\x02<sample path=\"C:\\Users\\chad\\Samples\\Kick 01.wav\"/>\x00\x05"s
                           "/Users/chad/Music/Serum Presets/Tables/Growl.wav\x00more"s
                           "name=Bass.SerumPreset;\x00\x00"s;
        const std::string wide = "D:\\Libraries\\Strings\\Violin.nki";
        for (char c : wide) { blob.push_back (c); blob.push_back ('\0'); }
        blob += "\0\0"s;

        blob += "x=/var/folders/ab/T/collabgod-test-synth/sample-1f.wav\x00"s;

        auto found = findPathLikeStrings (blob.data(), blob.size());
        auto has = [&] (const std::string& s) { return std::find (found.begin(), found.end(), s) != found.end(); };
        CHECK (has ("C:\\Users\\chad\\Samples\\Kick 01.wav"));
        CHECK (has ("/Users/chad/Music/Serum Presets/Tables/Growl.wav"));
        CHECK (has ("Bass.SerumPreset"));
        CHECK (has ("D:\\Libraries\\Strings\\Violin.nki"));
        CHECK (has ("/var/folders/ab/T/collabgod-test-synth/sample-1f.wav"));
        CHECK (found.size() == 5);

        std::mt19937 rng (5);
        std::string noise (1 << 16, '\0');
        for (auto& c : noise) c = (char) (rng() & 0xff);
        CHECK (findPathLikeStrings (noise.data(), noise.size()).empty());
    });

    test ("a home folder is replaced with ~ in reports", [] {
        CHECK (redactHome ("C:\\Users\\chad\\Samples\\Kick.wav") == "~\\Samples\\Kick.wav");
        CHECK (redactHome ("c:/users/chad/x.wav") == "~/x.wav");
        CHECK (redactHome ("/Users/chad/Music/a.wav") == "~/Music/a.wav");
        CHECK (redactHome ("/home/someone/a.wav") == "~/a.wav");
        CHECK (redactHome ("D:\\Libraries\\a.nki") == "D:\\Libraries\\a.nki");
    });

    test ("an apply is classified from the hashes and the parameters", [] {
        std::map<std::string, float> sent { { "1", 0.5f }, { "2", 0.25f } };
        CHECK (classifyApply ("h", "old", "h", sent, sent).outcome == ApplyOutcome::exact);
        CHECK (classifyApply ("h", "old", "old", sent, { { "1", 0.1f } }).outcome == ApplyOutcome::unchanged);
        CHECK (classifyApply ("h", "old", "other", sent, sent).outcome == ApplyOutcome::resaved);
        auto r = classifyApply ("h", "old", "other", sent, { { "1", 0.5f }, { "2", 0.9f }, { "3", 0.0f } });
        CHECK (r.outcome == ApplyOutcome::paramsDiffer);
        CHECK (r.differing.size() == 1 && r.differing[0] == "2");
        CHECK (r.compared == 2);
    });
}

//==============================================================================
// One member's rules with a plugin held in a map, for the unit tests.
struct Unit
{
    explicit Unit (const std::string& id, StateOptions o = {})
        : params (id, quick()), state (params, [this] { return values; }, o)
    {
        for (auto& [k, v] : values)
            params.seed (k, v);
        state.loaded ("h0", 10);
    }

    static Options quick() { Options o; o.flushIntervalMs = 0.0; return o; }

    std::map<std::string, float> values { { "cutoff", 0.5f }, { "res", 0.5f } };
    ParamSync params;
    StateSync state;

    /** Runs reads and flushes from `from` to `to` ms with hash `h`; returns the first announcement. */
    std::optional<StateOffer> run (double from, double to, const std::string& h, bool poll = false, bool held = false)
    {
        for (double t = from; t <= to; t += 1.0)
        {
            if (state.readDue (t, poll))
                state.observed (h, 10, 0.1, t);
            if (auto a = state.flush (t, held))
                return a;
        }
        return std::nullopt;
    }
};

void ruleTests()
{
    test ("a hinted change is announced once the state has been read unchanged for the quiet period", [] {
        Unit a ("a");
        a.state.hint (0.0);
        auto out = a.run (0.0, 250.0, "h1");
        CHECK (! out);   // seen at 30 ms, not yet quiet
        out = a.run (251.0, 400.0, "h1");
        CHECK (out && out->ref.hash == "h1" && out->ref.stamp.member == "a" && out->ref.stamp.counter > 0);
        CHECK (! a.run (401.0, 2000.0, "h1"));
        CHECK (a.state.stats().announced == 1);
        CHECK (a.state.baseline() == "h1");
    });

    test ("a change the plugin does not hint at is found only by polling", [] {
        StateOptions o; o.pollMs = 100.0;
        Unit quiet ("a"), polled ("b", o);
        CHECK (! quiet.run (0.0, 3000.0, "h1"));
        polled.state.localActivity (0.0);
        auto out = polled.run (0.0, 3000.0, "h1", true);
        CHECK (out && out->ref.hash == "h1");
    });

    test ("a state that keeps changing is announced when it stops, and not while a control is held", [] {
        Unit a ("a");
        for (int i = 0; i < 10; ++i)
        {
            a.state.hint (i * 100.0);
            CHECK (! a.run (i * 100.0, i * 100.0 + 99.0, "h" + std::to_string (i + 1)));
        }
        CHECK (! a.run (1000.0, 3000.0, "h10", false, true));
        auto out = a.run (3001.0, 3100.0, "h10");
        CHECK (out && out->ref.hash == "h10");
        CHECK (a.state.stats().announced == 1);
    });

    test ("a member whose plugin already saves the announced hash fetches nothing", [] {
        Unit a ("a"), b ("b");
        StateRef ref { "h1", 10, a.params.issueStamp() };
        auto step = b.state.receive ("a", ref, "h1", 0.0);
        CHECK (step.need == Need::nothing);
        CHECK (b.state.stats().matched == 1);
        CHECK (b.state.baseline() == "h1");
        CHECK (b.state.agreedStamp() == ref.stamp);
    });

    test ("a member with a different state reads again, then fetches; or matches on the second read", [] {
        Unit b ("b"), c ("c");
        StateRef ref { "h1", 10, Stamp { 5, "a" } };
        auto step = b.state.receive ("a", ref, "h0", 0.0);
        CHECK (step.need == Need::recheck);
        CHECK (! b.state.recheckDue (100.0));
        CHECK (b.state.recheckDue (150.0));
        step = b.state.recheck ("h0", 150.0);
        CHECK (step.need == Need::fetch && step.hash == "h1" && step.from == "a");

        c.state.receive ("a", ref, "h0", 0.0);
        step = c.state.recheck ("h1", 150.0);
        CHECK (step.need == Need::nothing);
        CHECK (c.state.stats().matched == 1);
    });

    test ("an older announcement is ignored", [] {
        Unit b ("b");
        b.state.receive ("a", { "h2", 10, Stamp { 9, "a" } }, "h2", 0.0);
        auto step = b.state.receive ("c", { "h1", 10, Stamp { 4, "c" } }, "h2", 1.0);
        CHECK (step.need == Need::nothing);
        CHECK (b.state.stats().stale == 1);
        CHECK (b.state.agreedHash() == "h2");
    });

    test ("bytes are applied once nobody here holds a control, and the plugin settling is not announced", [] {
        Unit b ("b");
        b.state.receive ("a", { "h1", 10, Stamp { 5, "a" } }, "h0", 0.0);
        b.state.recheck ("h0", 150.0);
        CHECK (b.state.arrived ("h1", {}));
        CHECK (! b.state.applyDue (true));
        CHECK (b.state.stats().appliedDeferred == 1);
        auto h = b.state.applyDue (false);
        CHECK (h && *h == "h1");
        b.state.applied ("h1", 10, 200.0);
        CHECK (b.state.baseline() == "h1");

        // The plugin rewrites its state as it settles (a background load), and says so.
        b.state.hint (300.0);
        CHECK (! b.run (300.0, 1000.0, "h1-settled"));
        CHECK (b.state.stats().absorbedSettling == 1);
        CHECK (b.state.baseline() == "h1-settled");

        // A person editing straight after is not the plugin settling.
        b.state.localActivity (1100.0);
        b.state.hint (1100.0);
        auto out = b.run (1100.0, 1600.0, "h1-edited");
        CHECK (out && out->ref.hash == "h1-edited");
    });

    test ("bytes that arrive after a newer announcement are dropped", [] {
        Unit b ("b");
        b.state.receive ("a", { "h1", 10, Stamp { 5, "a" } }, "h0", 0.0);
        b.state.recheck ("h0", 150.0);
        b.state.receive ("c", { "h2", 10, Stamp { 6, "c" } }, "h0", 160.0);
        CHECK (! b.state.arrived ("h1", {}));
        CHECK (b.state.stats().superseded == 1);
    });

    test ("a change right after parameters from someone else, with nothing done here, is not announced", [] {
        StateOptions o; o.pollMs = 100.0;
        Unit b ("b", o);
        b.run (0.0, 150.0, "h0", true);
        b.state.remoteActivity (160.0);
        CHECK (! b.run (151.0, 3000.0, "h0+remote-knob", true));
        CHECK (b.state.stats().absorbedRemoteParams == 1);

        // The same with a hint from the plugin: that is a change made here.
        b.state.remoteActivity (3010.0);
        b.state.hint (3020.0);
        auto out = b.run (3001.0, 4000.0, "h0+lfo", true);
        CHECK (out && out->ref.hash == "h0+lfo");
    });

    test ("a state that changes on every read with nothing done here is marked noisy, and polling stops", [] {
        StateOptions o; o.pollMs = 50.0; o.noisyAfter = 4; o.promptMs = 100.0;
        Unit a ("a", o);
        int reads = 0;
        for (double t = 1000.0; t < 3000.0; t += 1.0)
        {
            if (a.state.readDue (t, true))
                a.state.observed ("n" + std::to_string (++reads), 10, 0.1, t);
            a.state.flush (t, false);
        }
        CHECK (a.state.isNoisy());
        CHECK (reads < 10);
        CHECK (a.state.stats().announced == 0);
    });

    test ("slow reads stretch the polling interval to the budget", [] {
        StateOptions o; o.pollMs = 100.0; o.pollBudget = 0.02;
        Unit a ("a", o);
        a.state.observed ("h0", 10, 20.0, 0.0);   // a 20 ms read: at 2 %, once a second
        CHECK (std::abs (a.state.pollIntervalMs() - 1000.0) < 1.0e-6);
        CHECK (! a.state.readDue (500.0, true));
        CHECK (a.state.readDue (1000.0, true));
    });

    test ("if someone else's state arrives during a change here, ours goes out at once with a newer stamp", [] {
        Unit a ("a"), b ("b");
        a.state.hint (0.0);
        a.run (0.0, 50.0, "a1");
        CHECK (a.state.hasLocalChange());
        StateRef theirs { "b1", 10, Stamp { 7, "b" } };
        CHECK (a.state.receive ("b", theirs, "a1", 60.0).need == Need::nothing);
        auto out = a.state.flush (61.0, false);
        CHECK (out && out->ref.hash == "a1");
        CHECK (theirs.stamp < out->ref.stamp);
        CHECK (a.state.stats().forced == 1);
    });

    test ("a parameter the plugin changed without telling the host is found by whoever applies the state, and sent on", [] {
        Unit a ("a"), b ("b");
        // A preset load in A's plugin moved cutoff to 0.1 without a report: A's host still thinks 0.5.
        a.state.hint (0.0);
        auto offer = a.run (0.0, 500.0, "preset");
        CHECK (offer.has_value());
        if (! offer)
            return;
        CHECK (offer->params.at ("cutoff").first == 0.5f);

        b.state.receive ("a", offer->ref, "h0", 600.0);
        b.state.recheck ("h0", 750.0);
        CHECK (b.state.arrived ("preset", offer->params));
        CHECK (b.state.applyDue (false).has_value());
        b.values["cutoff"] = 0.1f;   // what B's plugin holds once it loaded the state
        auto again = b.state.applied ("preset", 10, 800.0);
        CHECK (again.empty());
        CHECK (b.state.stats().paramsDiscovered == 1);
        CHECK (b.params.value ("cutoff") && std::abs (*b.params.value ("cutoff") - 0.1f) < 1e-6f);
        CHECK (b.params.stamp ("cutoff") && *b.params.stamp ("cutoff") == offer->ref.stamp);

        // B sends it on as a parameter change, and A's host learns it.
        auto sent = b.params.flush (900.0);
        CHECK (sent.size() == 1 && sent[0].param == "cutoff" && sent[0].stamp == offer->ref.stamp);
        auto applies = a.params.receive ("b", sent, 1000.0);
        CHECK (applies.size() == 1 && std::abs (applies[0].value - 0.1f) < 1e-6f);
    });

    test ("a state that carries an older value of a parameter has it put back", [] {
        Unit b ("b");
        b.params.receive ("c", { { "cutoff", 0.8f, Stamp { 20, "c" }, Flags::none } }, 0.0);
        b.values["cutoff"] = 0.8f;
        const StateRef ref { "h1", 10, Stamp { 10, "a" } };
        ParamSync::Snapshot sent { { "cutoff", { 0.4f, Stamp { 5, "a" } } }, { "res", { 0.3f, Stamp { 9, "a" } } } };
        b.state.receive ("a", ref, "h0", 0.0);
        b.state.recheck ("h0", 150.0);
        b.state.arrived ("h1", sent);
        b.state.applyDue (false);
        b.values["cutoff"] = 0.4f;   // the state set cutoff back to what A had when it saved
        b.values["res"] = 0.3f;
        auto again = b.state.applied ("h1", 10, 200.0);
        CHECK (again.size() == 1 && again[0].param == "cutoff" && std::abs (again[0].value - 0.8f) < 1e-6f);
        CHECK (b.params.value ("res") && std::abs (*b.params.value ("res") - 0.3f) < 1e-6f);
        CHECK (b.params.stamp ("res") && *b.params.stamp ("res") == (Stamp { 9, "a" }));
        CHECK (b.state.stats().paramsDiscovered == 0);
        CHECK (b.params.flush (300.0).empty());

        // The plugin reports the value it loaded, late: an echo, and the newest is set again.
        auto late = b.params.localValue ("cutoff", 0.4f, 700.0);
        CHECK (late && std::abs (late->value - 0.8f) < 1e-6f);
        CHECK (b.params.flush (800.0).empty());
    });

    test ("an untouched state loses to any edited one, and two untouched ones agree by member id", [] {
        Unit a ("a"), b ("b");
        CHECK (a.state.current() && a.state.current()->stamp.counter == 0);
        auto step = a.state.receive ("b", *b.state.current(), "h0", 0.0);
        CHECK (step.need == Need::nothing);              // same default state
        CHECK (a.state.stats().matched == 1);
        CHECK (b.state.receive ("a", *a.state.current(), "h0", 0.0).need == Need::nothing);
        CHECK (b.state.stats().stale == 1);              // "b" outranks "a" at counter 0
    });
}

//==============================================================================
// Random convergence. Each member hosts a simulated plugin with parameters and
// a hidden part (a wavetable, an LFO shape). People move knobs, edit the hidden
// part and load presets, all at once, through a relay with random delays. The
// plugins can save the same thing differently (another version), take a while
// to reflect a parameter in their saved state, rewrite their state after a load,
// change parameters on a preset load without telling the host, and hint or not.
// After everyone stops, every member must hold the same hidden part and the
// same parameters, and the traffic must stop.

struct Setting
{
    const char* name;
    bool setDirty = true;         // the plugin calls setDirty when its hidden part changes
    bool window = true;           // the host sees the mouse let go in the plugin's window
    bool poll = false;            // the host polls
    bool reportsPresetParams = true;
    bool versionsDiffer = false;  // each member saves the same content differently
    double processLagMs = 0.0;    // a parameter set from outside reaches the saved state this much later
    double settleMs = 0.0;        // after a load, the plugin rewrites its state this much later
    double recheckMs = 150.0;
    bool measureOnly = false;     // a setting expected to lose some edits: report how often, do not fail
};

struct SimPlugin
{
    std::map<std::string, float> truth;           // what the plugin really holds
    std::map<std::string, float> cache;           // what the host thinks it holds (JUCE's cached values)
    std::map<std::string, float> processed;       // what the processor saves; lags parameters set from outside
    double processedAtMs = 0.0;
    int hidden = 0;
    int version = 0;
    bool cached = true;                           // rewritten some time after a load
    double cacheAtMs = 0.0;

    std::string save() const
    {
        std::string s = "v" + std::to_string (version) + "|" + std::to_string (hidden) + "|" + (cached ? "c" : "-");
        for (auto& [k, v] : processed)
            s += "|" + k + "=" + std::to_string (v);
        return s;
    }
};

struct Blob
{
    int hidden = 0;
    std::map<std::string, float> params;
};

struct Msg
{
    double at;
    enum Kind { params, announce, want, bytes } kind;
    std::vector<Change> changes;
    StateRef ref;
    std::string hash;
};

struct RunResult
{
    bool converged = true, quiesced = true;
    std::uint64_t messages = 0, applied = 0, matched = 0, discovered = 0;
    std::string detail;
};

bool traceOn = false;
#define TRACE(...) do { if (traceOn) std::printf (__VA_ARGS__); } while (false)

RunResult simulate (unsigned seed, int members, const Setting& setting)
{
    std::mt19937 rng (seed);
    auto uniform = [&] (double lo, double hi) { return std::uniform_real_distribution<double> (lo, hi) (rng); };
    auto chance = [&] (double p) { return uniform (0.0, 1.0) < p; };
    const auto M = (std::size_t) members;

    const std::vector<std::string> names { "cutoff", "res", "level" };
    std::vector<SimPlugin> plugins (M);
    std::vector<std::unique_ptr<ParamSync>> ps;
    std::vector<std::unique_ptr<StateSync>> ss;
    std::map<std::string, Blob> bytes;                     // what each hash's bytes hold
    std::map<std::string, ParamSync::Snapshot> offers;     // the parameters sent with them

    Options po; po.flushIntervalMs = 33.0;
    StateOptions so;
    so.pollMs = setting.poll ? 100.0 : 0.0;
    so.recheckMs = setting.recheckMs;

    for (std::size_t m = 0; m < M; ++m)
    {
        auto& pl = plugins[m];
        pl.version = setting.versionsDiffer ? (int) m : 0;
        for (auto& n : names)
            pl.truth[n] = pl.cache[n] = pl.processed[n] = 0.5f;

        ps.push_back (std::make_unique<ParamSync> ("m" + std::to_string (m), po));
        for (auto& n : names)
            ps.back()->seed (n, 0.5f);
        ss.push_back (std::make_unique<StateSync> (*ps.back(), [&pl] { return pl.cache; }, so));
        const auto saved = pl.save();
        ss.back()->loaded (sha (saved), saved.size());
        bytes[sha (saved)] = { pl.hidden, pl.processed };
    }

    std::vector<std::vector<std::deque<Msg>>> links (M, std::vector<std::deque<Msg>> (M));
    std::vector<std::map<std::string, bool>> holding (M);
    std::vector<std::map<std::string, double>> holdUntil (M);
    RunResult result;
    double lastActivity = 0.0;
    const double activeMs = 8000.0, endMs = 90000.0;

    auto send = [&] (std::size_t from, std::size_t to, Msg msg, double now)
    {
        auto& q = links[from][to];
        const double earliest = q.empty() ? now : q.back().at;
        msg.at = std::max (earliest, now + uniform (5.0, 120.0));
        q.push_back (std::move (msg));
        ++result.messages;
        lastActivity = now;
    };

    auto broadcast = [&] (std::size_t from, const Msg& msg, double now)
    {
        for (std::size_t to = 0; to < M; ++to)
            if (to != from)
                send (from, to, msg, now);
    };

    // The host sets a parameter: the plugin takes it, its saved state follows after the lag.
    auto setParam = [&] (std::size_t m, const std::string& n, float v, double now)
    {
        auto& pl = plugins[m];
        pl.truth[n] = pl.cache[n] = v;
        pl.processedAtMs = now + setting.processLagMs;
    };

    auto anyHeld = [&] (std::size_t m)
    {
        for (auto& [n, h] : holding[m])
            if (h)
                return true;
        return false;
    };

    // The host reads the state, and keeps the bytes of what it read.
    auto readState = [&] (std::size_t m)
    {
        const auto saved = plugins[m].save();
        const auto h = sha (saved);
        bytes[h] = { plugins[m].hidden, plugins[m].processed };
        return h;
    };

    for (double now = 0.0; now <= endMs; now += 1.0)
    {
        for (std::size_t m = 0; m < M; ++m)
        {
            auto& pl = plugins[m];
            auto& p = *ps[m];
            auto& s = *ss[m];

            // The plugin processes audio: its saved state catches up with its parameters.
            if (now >= pl.processedAtMs)
                pl.processed = pl.truth;
            if (! pl.cached && now >= pl.cacheAtMs)
                pl.cached = true;

            // People act: each member is busy every other second, the members out of step.
            const bool busy = ((int) (now / 1000.0) + (int) m) % 2 == 0;

            if (now < activeMs && chance (busy ? 0.02 : 0.0005))
            {
                const double r = uniform (0.0, 1.0);
                const auto& n = names[(std::size_t) (rng() % names.size())];
                auto& held = holding[m][n];

                if (r < 0.2 && ! held)
                {
                    p.localGestureBegin (n, now);
                    held = true;
                    holdUntil[m][n] = now + uniform (50.0, 800.0);
                }
                else if (r < 0.6)
                {
                    const float v = (float) uniform (0.0, 1.0);
                    setParam (m, n, v, now);
                    pl.processedAtMs = now;
                    s.localActivity (now);
                    TRACE ("%6.0f m%zu knob %s %.3f\n", now, m, n.c_str(), v);
                    if (auto again = p.localValue (n, v, now))
                        setParam (m, again->param, again->value, now);
                }
                else if (r < 0.85)
                {
                    pl.hidden = (int) (rng() % 100000);
                    TRACE ("%6.0f m%zu hidden %d\n", now, m, pl.hidden);
                    if (setting.window)
                    {
                        s.localActivity (now);
                        s.hint (now);
                    }
                    if (setting.setDirty)
                        s.hint (now);
                }
                else
                {
                    // A preset: the hidden part and every parameter not held here.
                    pl.hidden = (int) (rng() % 100000);
                    TRACE ("%6.0f m%zu preset %d%s\n", now, m, pl.hidden, setting.reportsPresetParams ? "" : " (parameters not reported)");
                    if (setting.window)
                        s.localActivity (now);
                    for (auto& k : names)
                    {
                        if (holding[m][k])
                            continue;
                        const float v = (float) uniform (0.0, 1.0);
                        pl.truth[k] = v;
                        if (setting.reportsPresetParams)
                        {
                            pl.cache[k] = v;
                            if (auto again = p.localValue (k, v, now))
                                setParam (m, again->param, again->value, now);
                        }
                    }
                    pl.processed = pl.truth;
                    if (setting.window || setting.setDirty)
                        s.hint (now);
                }
            }

            // Letting go, as the host does it: the plugin's final value, then the end of the gesture.
            for (auto& [n, held] : holding[m])
            {
                if (held && (now >= holdUntil[m][n] || now >= activeMs))
                {
                    if (auto again = p.localValue (n, pl.cache[n], now))
                        setParam (m, again->param, again->value, now);
                    p.localGestureEnd (n, now);
                    held = false;
                }
            }

            if (s.readDue (now, setting.poll))
                s.observed (readState (m), 10, 0.2, now);

            if (s.recheckDue (now))
            {
                auto step = s.recheck (readState (m), now);
                if (step.need == Need::fetch)
                {
                    TRACE ("%6.0f m%zu want %s from %s\n", now, m, step.hash.substr (0, 6).c_str(), step.from.c_str());
                    send (m, (std::size_t) std::stoi (step.from.substr (1)), { 0.0, Msg::want, {}, {}, step.hash }, now);
                }
            }

            if (auto h = s.applyDue (anyHeld (m)))
            {
                // setStateInformation: the plugin takes the bytes, and the host re-reads every parameter.
                const auto& blob = bytes.at (*h);
                pl.hidden = blob.hidden;
                pl.truth = pl.cache = pl.processed = blob.params;
                pl.processedAtMs = now;
                if (setting.settleMs > 0.0)
                {
                    pl.cached = false;
                    pl.cacheAtMs = now + setting.settleMs;
                }
                ++result.applied;
                TRACE ("%6.0f m%zu apply %s hidden %d\n", now, m, h->substr (0, 6).c_str(), pl.hidden);
                const auto before = s.stats().paramsDiscovered;
                for (auto& a : s.applied (readState (m), 10, now))
                {
                    TRACE ("%6.0f m%zu put back %s %.3f\n", now, m, a.param.c_str(), a.value);
                    setParam (m, a.param, a.value, now);
                }
                result.discovered += s.stats().paramsDiscovered - before;
                lastActivity = now;
            }

            if (auto offer = s.flush (now, anyHeld (m)))
            {
                offers[offer->ref.hash] = offer->params;
                TRACE ("%6.0f m%zu announce %s (%llu,%s) hidden %d\n", now, m, offer->ref.hash.substr (0, 6).c_str(),
                       (unsigned long long) offer->ref.stamp.counter, offer->ref.stamp.member.c_str(), bytes.at (offer->ref.hash).hidden);
                broadcast (m, { 0.0, Msg::announce, {}, offer->ref, {} }, now);
            }

            auto out = p.flush (now);
            if (! out.empty())
                broadcast (m, { 0.0, Msg::params, out, {}, {} }, now);
        }

        // The network delivers.
        for (std::size_t from = 0; from < M; ++from)
        {
            for (std::size_t to = 0; to < M; ++to)
            {
                auto& q = links[from][to];

                while (! q.empty() && q.front().at <= now)
                {
                    auto msg = std::move (q.front());
                    q.pop_front();
                    lastActivity = now;
                    auto& p = *ps[to];
                    auto& s = *ss[to];
                    const auto sender = "m" + std::to_string (from);

                    switch (msg.kind)
                    {
                        case Msg::params:
                        {
                            for (auto& c : msg.changes)
                                TRACE ("%6.0f m%zu <- %s %s %.3f (%llu,%s) f%d\n", now, to, sender.c_str(), c.param.c_str(), c.value,
                                       (unsigned long long) c.stamp.counter, c.stamp.member.c_str(), c.flags);
                            auto applies = p.receive (sender, msg.changes, now);
                            for (auto& a : applies)
                                setParam (to, a.param, a.value, now);
                            if (! applies.empty())
                                s.remoteActivity (now);
                            break;
                        }
                        case Msg::announce:
                        {
                            const auto matchedBefore = s.stats().matched;
                            auto step = s.receive (sender, msg.ref, readState (to), now);
                            if (step.need == Need::fetch)
                                send (to, from, { 0.0, Msg::want, {}, {}, step.hash }, now);
                            result.matched += s.stats().matched - matchedBefore;
                            break;
                        }
                        case Msg::want:
                            send (to, from, { 0.0, Msg::bytes, {}, {}, msg.hash }, now);
                            break;
                        case Msg::bytes:
                            s.arrived (msg.hash, offers.at (msg.hash));
                            break;
                    }
                }
            }
        }

        bool busy = false;
        for (std::size_t m = 0; m < M; ++m)
            busy = busy || ss[m]->hasLocalChange() || ss[m]->isWaiting() || ps[m]->hasPending();

        if (now > activeMs + 3000.0 && now - lastActivity > 3000.0 && ! busy)
            break;
    }

    for (std::size_t m = 0; m < M; ++m)
    {
        if (ss[m]->hasLocalChange() || ss[m]->isWaiting() || ps[m]->hasPending())
            result.quiesced = false;
        for (std::size_t to = 0; to < M; ++to)
            if (! links[m][to].empty())
                result.quiesced = false;
    }

    // Same everywhere: the hidden part, what each plugin really holds, what each host knows, and the agreed stamp.
    for (std::size_t m = 0; m < M; ++m)
    {
        const auto& a = plugins[0];
        const auto& b = plugins[m];
        const auto tag = "m" + std::to_string (m);

        if (a.hidden != b.hidden)
        {
            result.converged = false;
            result.detail += "hidden m0=" + std::to_string (a.hidden) + " " + tag + "=" + std::to_string (b.hidden) + "; ";
        }

        if (! (ss[0]->agreedStamp() == ss[m]->agreedStamp()))
        {
            result.converged = false;
            result.detail += "agreed stamp differs for " + tag + "; ";
        }

        for (auto& n : names)
        {
            const float known = *ps[m]->value (n);

            if (std::abs (a.truth.at (n) - b.truth.at (n)) > 1.0e-6f || std::abs (known - b.truth.at (n)) > 1.0e-6f
                || std::abs (b.cache.at (n) - b.truth.at (n)) > 1.0e-6f)
            {
                result.converged = false;
                result.detail += n + ": " + tag + " plugin=" + std::to_string (b.truth.at (n)) + " host=" + std::to_string (b.cache.at (n))
                               + " known=" + std::to_string (known) + ", m0 plugin=" + std::to_string (a.truth.at (n)) + "; ";
            }
        }
    }

    return result;
}

const Setting settings[] {
    //  name                                                setDirty window poll  reports versions lag   settle recheck measure
    { "setDirty and the mouse in the window" },
    { "the mouse in the window only, polling",               false,   true,  true },
    { "presets change parameters without telling the host",   true,    true,  true,  false },
    { "versions save differently",                           true,    true,  false, true,   true },
    { "parameters reach the saved state 40 ms late",         true,    true,  true,  true,   false,   40.0 },
    { "plugins rewrite their state 400 ms after a load",     true,    true,  true,  true,   false,   0.0,  400.0 },
    { "everything at once",                                  true,    true,  true,  false,  true,    40.0, 400.0 },
    { "no recheck before fetching",                          true,    true,  true,  true,   false,   40.0, 0.0,   0.0 },
    // Measured, not required: with only setDirty, or no signal at all, an edit
    // in the 1.5 s after a state arrives looks like the plugin settling.
    { "setDirty only",                                       true,    false, false, true,   false,   0.0,  0.0,   150.0, true },
    { "no signal at all, polling",                           false,   false, true,  true,   false,   0.0,  0.0,   150.0, true },
};

void convergenceTests (int seeds)
{
    for (const auto& s : settings)
    {
        for (int members = 2; members <= 4; ++members)
        {
            std::string name = std::string ("random convergence, ") + std::to_string (members) + " members, " + s.name;
            test (name.c_str(), [&] {
                std::uint64_t messages = 0, applied = 0, matched = 0, discovered = 0;
                int diverged = 0;

                for (int seed = 1; seed <= seeds; ++seed)
                {
                    auto r = simulate ((unsigned) (seed * 7919 + members), members, s);
                    messages += r.messages;
                    applied += r.applied;
                    matched += r.matched;
                    discovered += r.discovered;

                    if (! r.quiesced || (! r.converged && ! s.measureOnly))
                    {
                        std::printf ("  seed %d: converged=%d quiesced=%d %s\n", seed, r.converged, r.quiesced, r.detail.c_str());
                        CHECK (r.converged && r.quiesced);
                        return;
                    }

                    if (! r.converged)
                        ++diverged;
                }

                if (s.measureOnly)
                    std::printf ("  measured: %d of %d runs ended with an edit lost\n", diverged, seeds);

                std::printf ("  %d seeds; per run on average %.1f messages, %.1f states applied, %.1f already matched, %.1f silent parameters found\n",
                             seeds, (double) messages / seeds, (double) applied / seeds, (double) matched / seeds, (double) discovered / seeds);
            });
        }
    }
}
} // namespace

int main (int argc, char** argv)
{
    int seeds = 100;

    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp (argv[i], "--seeds") == 0)
            seeds = std::max (1, std::atoi (argv[i + 1]));

    if (argc >= 5 && std::strcmp (argv[1], "--trace") == 0)
    {
        traceOn = true;
        const int index = std::atoi (argv[4]);
        auto r = simulate ((unsigned) std::atoi (argv[2]), std::atoi (argv[3]), settings[index]);
        std::printf ("converged=%d quiesced=%d %s\n", r.converged, r.quiesced, r.detail.c_str());
        return 0;
    }

    helperTests();
    ruleTests();
    convergenceTests (seeds);

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
