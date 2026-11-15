// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Tests for the mirroring rules in src/core, with no JUCE,
// engine or network: a simulated clock, simulated plugins and a simulated relay.
//
//   param_sync_tests            run everything
//   param_sync_tests --seeds N  run the random convergence test with N seeds per setting

#include "../src/core/ParamSync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
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

bool near (float a, float b, float tol = 1.0e-6f) { return std::abs (a - b) <= tol; }

void test (const char* name, const std::function<void()>& body)
{
    currentTest = name;
    const int before = failures;
    body();
    std::printf ("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

Options unthrottled() { Options o; o.flushIntervalMs = 0.0; return o; }

//==============================================================================
void unitTests()
{
    test ("a local change is sent once, with a stamp from this member", [] {
        ParamSync a ("a", unthrottled());
        a.seed ("cutoff", 0.5f);
        a.localValue ("cutoff", 0.7f, 0.0);
        auto out = a.flush (0.0);
        CHECK (out.size() == 1);
        CHECK (out[0].param == "cutoff");
        CHECK (near (out[0].value, 0.7f));
        CHECK (out[0].stamp.member == "a");
        CHECK (out[0].flags == Flags::none);
        CHECK (a.flush (1.0).empty());
    });

    test ("repeating the known value sends nothing", [] {
        ParamSync a ("a", unthrottled());
        a.seed ("cutoff", 0.5f);
        a.localValue ("cutoff", 0.5f, 0.0);
        CHECK (a.flush (0.0).empty());
        CHECK (a.stats().duplicates == 1);
    });

    test ("the throttle coalesces a fast sweep into one change per interval, ending on the last value", [] {
        Options o; o.flushIntervalMs = 33.0;
        ParamSync a ("a", o);
        a.seed ("cutoff", 0.0f);
        int sent = 0;
        float last = -1.0f;

        // 1 s sweep at 1 kHz, flushing every millisecond as a host timer would.
        for (int ms = 0; ms <= 1000; ++ms)
        {
            a.localValue ("cutoff", (float) ms / 1000.0f, ms);
            for (auto& c : a.flush (ms)) { ++sent; last = c.value; }
        }
        for (int ms = 1001; ms < 1100; ++ms)
            for (auto& c : a.flush (ms)) { ++sent; last = c.value; }

        CHECK (sent >= 30 && sent <= 32);   // 1000 ms / 33 ms, plus the first and the last
        CHECK (near (last, 1.0f));
        CHECK (a.stats().localEvents == 1001);
        CHECK (a.stats().duplicates + a.stats().coalesced + a.stats().sentChanges == 1001);   // the first repeats the seed
    });

    test ("gesture begin and end are sent at once, and the end carries the final value", [] {
        Options o; o.flushIntervalMs = 1000.0;
        ParamSync a ("a", o);
        a.seed ("cutoff", 0.2f);
        a.localGestureBegin ("cutoff", 0.0);
        a.localValue ("cutoff", 0.3f, 0.0);
        auto first = a.flush (0.0);
        CHECK (first.size() == 1 && (first[0].flags & Flags::gestureBegin) != 0);
        CHECK (near (first[0].value, 0.3f));
        a.localValue ("cutoff", 0.4f, 5.0);
        CHECK (a.flush (5.0).empty());                  // held back by the interval
        a.localValue ("cutoff", 0.9f, 6.0);
        a.localGestureEnd ("cutoff", 7.0);
        auto last = a.flush (7.0);                      // the end does not wait
        CHECK (last.size() == 1 && (last[0].flags & Flags::gestureEnd) != 0);
        CHECK (near (last[0].value, 0.9f));
    });

    test ("a remote change is applied once and its echo from the plugin is not sent back", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("cutoff", 0.5f);
        auto applied = b.receive ("a", { { "cutoff", 0.8f, { 3, "a" }, Flags::none } }, 100.0);
        CHECK (applied.size() == 1 && near (applied[0].value, 0.8f));
        b.localValue ("cutoff", 0.8f, 120.0);          // the plugin reports what the host just set
        CHECK (b.flush (130.0).empty());
        CHECK (b.stats().echoes == 1);
    });

    test ("a late echo of an older remote value is not sent, and the newest is set again", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("cutoff", 0.5f);
        b.receive ("a", { { "cutoff", 0.6f, { 3, "a" }, Flags::none } }, 0.0);
        b.receive ("a", { { "cutoff", 0.7f, { 4, "a" }, Flags::none } }, 10.0);
        auto again = b.localValue ("cutoff", 0.6f, 20.0);
        CHECK (again && near (again->value, 0.7f));
        CHECK (b.flush (20.0).empty());
        CHECK (b.stats().echoesStale == 1);
    });

    test ("a rounded echo while someone else holds the control is not sent; their end resets the plugin", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("cutoff", 0.5f);
        b.receive ("a", { { "cutoff", 0.613f, { 3, "a" }, Flags::gestureBegin } }, 0.0);
        CHECK (! b.localValue ("cutoff", 0.61f, 5.0));  // the plugin rounded it
        CHECK (b.flush (5.0).empty());
        CHECK (b.stats().echoesAltered == 1);
        auto applied = b.receive ("a", { { "cutoff", 0.613f, { 4, "a" }, Flags::gestureEnd } }, 50.0);
        CHECK (applied.size() == 1 && near (applied[0].value, 0.613f));  // same value, set again
    });

    test ("a rounded echo with nobody holding the control is sent, so everyone settles on the rounded value", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("cutoff", 0.5f);
        b.receive ("a", { { "cutoff", 0.613f, { 3, "a" }, Flags::none } }, 0.0);
        b.localValue ("cutoff", 0.61f, 5.0);
        auto out = b.flush (5.0);
        CHECK (out.size() == 1 && near (out[0].value, 0.61f));
    });

    test ("after the echo window a plugin change is sent again", [] {
        Options o = unthrottled(); o.echoWindowMs = 250.0;
        ParamSync b ("b", o);
        b.seed ("cutoff", 0.5f);
        b.receive ("a", { { "cutoff", 0.8f, { 3, "a" }, Flags::none } }, 0.0);
        b.localValue ("cutoff", 0.6f, 300.0);
        auto out = b.flush (300.0);
        CHECK (out.size() == 1 && near (out[0].value, 0.6f));
        CHECK (out[0].stamp.counter > 3);               // newer than what it saw
    });

    test ("a change made inside a gesture is never taken for an echo", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("cutoff", 0.5f);
        b.receive ("a", { { "cutoff", 0.8f, { 3, "a" }, Flags::none } }, 0.0);
        b.localGestureBegin ("cutoff", 10.0);
        b.localValue ("cutoff", 0.2f, 11.0);
        auto out = b.flush (11.0);
        CHECK (out.size() == 1 && near (out[0].value, 0.2f));
    });

    test ("an older remote change is ignored", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("cutoff", 0.5f);
        b.receive ("a", { { "cutoff", 0.8f, { 5, "a" }, Flags::none } }, 0.0);
        auto applied = b.receive ("c", { { "cutoff", 0.1f, { 4, "c" }, Flags::none } }, 1.0);
        CHECK (applied.empty());
        CHECK (near (*b.value ("cutoff"), 0.8f));
        CHECK (b.stats().stale == 1);
    });

    test ("equal counters are ordered by member id, the same everywhere", [] {
        ParamSync a ("a", unthrottled()), b ("b", unthrottled());
        a.seed ("x", 0.0f); b.seed ("x", 0.0f);
        a.localValue ("x", 0.3f, 0.0);
        b.localValue ("x", 0.6f, 0.0);
        auto fromA = a.flush (0.0), fromB = b.flush (0.0);
        CHECK (fromA[0].stamp.counter == fromB[0].stamp.counter);
        a.receive ("b", fromB, 1.0);
        b.receive ("a", fromA, 1.0);
        CHECK (near (*a.value ("x"), 0.6f));            // "b" > "a"
        CHECK (near (*b.value ("x"), 0.6f));
    });

    test ("while someone holds a control, remote changes do not move it; the last to let go wins", [] {
        ParamSync a ("a", unthrottled()), b ("b", unthrottled());
        a.seed ("x", 0.0f); b.seed ("x", 0.0f);

        a.localGestureBegin ("x", 0.0); a.localValue ("x", 0.2f, 0.0);
        b.localGestureBegin ("x", 0.0); b.localValue ("x", 0.7f, 0.0);
        auto ab = a.flush (0.0), ba = b.flush (0.0);
        CHECK (a.receive ("b", ba, 1.0).empty());            // a is holding: b's change does not apply
        CHECK (b.receive ("a", ab, 1.0).empty());
        CHECK (a.heldBy ("x") == "b");
        CHECK (b.heldBy ("x") == "a");

        b.localGestureEnd ("x", 2.0);                    // b lets go first
        auto bEnd = b.flush (2.0);
        CHECK (a.receive ("b", bEnd, 3.0).empty());           // a still holds
        CHECK (a.heldBy ("x").empty());

        a.localValue ("x", 0.4f, 4.0);
        a.localGestureEnd ("x", 5.0);                     // a lets go last
        auto aEnd = a.flush (5.0);
        auto applied = b.receive ("a", aEnd, 6.0);
        CHECK (applied.size() == 1 && near (applied[0].value, 0.4f));
        CHECK (near (*a.value ("x"), 0.4f));
        CHECK (near (*b.value ("x"), 0.4f));
    });

    test ("a member who leaves no longer holds anything", [] {
        ParamSync b ("b", unthrottled());
        b.seed ("x", 0.0f);
        b.receive ("a", { { "x", 0.2f, { 1, "a" }, Flags::gestureBegin } }, 0.0);
        CHECK (b.heldBy ("x") == "a");
        CHECK (! b.localValue ("x", 0.3f, 1.0));     // dropped: a holds it ...
        auto reset = b.memberLeft ("a");
        CHECK (b.heldBy ("x").empty());
        CHECK (reset.size() == 1 && near (reset[0].value, 0.2f));   // ... so the plugin goes back
    });

    test ("an unsent local change that has lost to a newer remote one is dropped", [] {
        Options o; o.flushIntervalMs = 100.0;
        ParamSync a ("a", o);
        a.seed ("x", 0.0f);
        a.localValue ("x", 0.1f, 0.0);
        a.flush (0.0);                                  // sent; the next one waits
        a.localValue ("x", 0.2f, 10.0);
        a.receive ("b", { { "x", 0.9f, { 50, "b" }, Flags::none } }, 20.0);
        CHECK (a.flush (200.0).empty());
        CHECK (near (*a.value ("x"), 0.9f));
    });
}

//==============================================================================
// Random convergence: members edit the same parameters at once through a relay
// that delivers each member's messages to every other member in order, with
// random delays. Plugins may round values and may echo values set by the host,
// sometimes after the echo window. After everyone lets go and the network goes
// quiet, every member must hold the same value for every parameter, and the
// traffic must stop.

struct SimPlugin
{
    float step = 0.0f;              // rounding; 0 means none
    double echoChance = 0.0;        // chance the plugin reports a host-set value back
    std::map<std::string, float> values;

    float round (float v) const
    {
        return step > 0.0f ? std::round (v / step) * step : v;
    }
};

struct Delivery
{
    double at;
    std::vector<Change> changes;
};

struct Echo
{
    double at;
    int member;
    std::string param;
};

struct SimResult
{
    bool converged = true;
    bool quiesced = true;
    std::uint64_t messages = 0;
    std::string detail;
};

bool traceOn = false;
#define TRACE(...) do { if (traceOn) std::printf (__VA_ARGS__); } while (false)

SimResult simulate (unsigned seed, int members, const Options& opts, float step, double echoChance)
{
    std::mt19937 rng (seed);
    auto uniform = [&] (double lo, double hi) { return std::uniform_real_distribution<double> (lo, hi) (rng); };
    auto chance = [&] (double p) { return uniform (0.0, 1.0) < p; };

    const std::vector<std::string> names { "cutoff", "res", "level" };
    std::vector<ParamSync> sync;
    std::vector<SimPlugin> plugins (members);

    for (int m = 0; m < members; ++m)
    {
        sync.emplace_back ("m" + std::to_string (m), opts);
        plugins[m].step = step;
        plugins[m].echoChance = echoChance;

        for (auto& n : names)
        {
            sync[m].seed (n, 0.5f);
            plugins[m].values[n] = 0.5f;
        }
    }

    // Links from each member to each other member, in order.
    std::vector<std::vector<std::deque<Delivery>>> links (members, std::vector<std::deque<Delivery>> (members));
    std::vector<Echo> echoes;
    std::vector<std::map<std::string, bool>> holding (members);

    const double activeMs = 1500.0;
    const double endMs = 20000.0;
    SimResult result;
    double lastActivity = 0.0;

    for (double now = 0.0; now <= endMs; now += 1.0)
    {
        // People act.
        if (now < activeMs)
        {
            for (int m = 0; m < members; ++m)
            {
                if (! chance (0.08))
                    continue;

                const auto& n = names[(std::size_t) (rng() % names.size())];
                auto& held = holding[m][n];
                const double r = uniform (0.0, 1.0);

                if (! held && r < 0.25)
                {
                    TRACE ("%6.0f m%d %s begin\n", now, m, n.c_str());
                    sync[m].localGestureBegin (n, now);
                    held = true;
                }
                else if (held && r < 0.35)
                {
                    TRACE ("%6.0f m%d %s end\n", now, m, n.c_str());
                    sync[m].localGestureEnd (n, now);
                    held = false;
                }
                else
                {
                    // A move in the plugin window or a host control (in a gesture),
                    // or a change with no gesture, such as a MIDI-learned knob.
                    const float v = plugins[m].round ((float) uniform (0.0, 1.0));
                    plugins[m].values[n] = v;
                    TRACE ("%6.0f m%d %s local %.4f held=%d heldBy=%s\n", now, m, n.c_str(), v, (int) held, sync[m].heldBy (n).c_str());
                    if (auto again = sync[m].localValue (n, v, now))
                        plugins[m].values[again->param] = plugins[m].round (again->value);
                }
            }
        }
        else if (now == activeMs)
        {
            for (int m = 0; m < members; ++m)
                for (auto& [n, held] : holding[m])
                    if (held)
                    {
                        sync[m].localGestureEnd (n, now);
                        held = false;
                    }
        }

        // Plugins echo what the host set.
        for (auto it = echoes.begin(); it != echoes.end();)
        {
            if (it->at <= now)
            {
                auto& plugin = plugins[it->member];
                TRACE ("%6.0f m%d %s echo %.4f\n", now, it->member, it->param.c_str(), plugin.values[it->param]);
                if (auto again = sync[it->member].localValue (it->param, plugin.values[it->param], now))
                    plugin.values[again->param] = plugin.round (again->value);
                it = echoes.erase (it);
                lastActivity = now;
            }
            else
            {
                ++it;
            }
        }

        // The network delivers.
        for (int from = 0; from < members; ++from)
        {
            for (int to = 0; to < members; ++to)
            {
                auto& q = links[from][to];

                while (! q.empty() && q.front().at <= now)
                {
                    for (auto& c : q.front().changes)
                        TRACE ("%6.0f m%d <- m%d %s %.4f (%llu,%s) f%d\n", now, to, from, c.param.c_str(), c.value, (unsigned long long) c.stamp.counter, c.stamp.member.c_str(), c.flags);
                    for (auto& a : sync[to].receive (sync[from].member(), q.front().changes, now))
                    {
                        TRACE ("%6.0f m%d apply %s %.4f\n", now, to, a.param.c_str(), a.value);
                        plugins[to].values[a.param] = plugins[to].round (a.value);

                        if (chance (plugins[to].echoChance))
                            echoes.push_back ({ now + uniform (0.0, 500.0), to, a.param });
                    }

                    q.pop_front();
                    lastActivity = now;
                }
            }
        }

        // Hosts flush.
        for (int from = 0; from < members; ++from)
        {
            auto out = sync[from].flush (now);

            if (out.empty())
                continue;

            ++result.messages;
            lastActivity = now;

            for (int to = 0; to < members; ++to)
            {
                if (to == from)
                    continue;

                auto& q = links[from][to];
                const double earliest = q.empty() ? now : q.back().at;
                q.push_back ({ std::max (earliest, now + uniform (5.0, 120.0)), out });
            }
        }

        if (now > activeMs + 2000.0 && now - lastActivity > 2000.0)
            break;
    }

    // Quiet?
    for (int m = 0; m < members; ++m)
    {
        if (sync[m].hasPending())
            result.quiesced = false;

        for (int to = 0; to < members; ++to)
            if (! links[m][to].empty())
                result.quiesced = false;
    }

    if (! echoes.empty())
        result.quiesced = false;

    // Same everywhere?
    for (auto& n : names)
    {
        const float agreed = *sync[0].value (n);

        for (int m = 1; m < members; ++m)
        {
            if (! near (*sync[m].value (n), agreed))
            {
                result.converged = false;
                result.detail += n + ": m0=" + std::to_string (agreed) + " m" + std::to_string (m) + "="
                               + std::to_string (*sync[m].value (n)) + "; ";
            }
        }

        // Each plugin is at the agreed value, give or take its own rounding.
        for (int m = 0; m < members; ++m)
        {
            if (std::abs (plugins[m].values[n] - agreed) > step * 0.5f + 1.0e-5f)
            {
                result.converged = false;
                result.detail += n + " plugin m" + std::to_string (m) + "=" + std::to_string (plugins[m].values[n])
                               + " agreed=" + std::to_string (agreed) + "; ";
            }
        }
    }

    return result;
}

void convergenceTests (int seeds)
{
    struct Setting { const char* name; double interval, echoWindow; float step; double echoChance; };
    const Setting settings[] {
        { "unthrottled, exact plugins, no echoes",       0.0,   250.0, 0.0f,  0.0 },
        { "throttled 33 ms, exact plugins, echoes",     33.0,   250.0, 0.0f,  0.7 },
        { "throttled 33 ms, rounding plugins, echoes",  33.0,   250.0, 0.01f, 0.7 },
        { "echo window off, rounding plugins, echoes",  33.0,     0.0, 0.01f, 0.7 },
        { "unthrottled, rounding plugins, echoes",       0.0,   250.0, 0.01f, 1.0 },
    };

    for (const auto& s : settings)
    {
        for (int members = 2; members <= 4; ++members)
        {
            std::string name = std::string ("random convergence, ") + std::to_string (members) + " members, " + s.name;
            test (name.c_str(), [&] {
                std::uint64_t messages = 0;

                for (int seed = 1; seed <= seeds; ++seed)
                {
                    Options o;
                    o.flushIntervalMs = s.interval;
                    o.echoWindowMs = s.echoWindow;
                    auto r = simulate ((unsigned) (seed * 7919 + members), members, o, s.step, s.echoChance);
                    messages += r.messages;

                    if (! r.converged || ! r.quiesced)
                    {
                        std::printf ("  seed %d: converged=%d quiesced=%d %s\n", seed, r.converged, r.quiesced, r.detail.c_str());
                        CHECK (r.converged && r.quiesced);
                        return;
                    }
                }

                std::printf ("  %d seeds, %.1f messages per run on average\n", seeds, (double) messages / seeds);
            });
        }
    }
}
} // namespace

int main (int argc, char** argv)
{
    int seeds = 200;

    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp (argv[i], "--seeds") == 0)
            seeds = std::max (1, std::atoi (argv[i + 1]));

    // --trace <seed> <members> <interval ms> <echo window ms> <rounding step> <echo chance>
    if (argc >= 8 && std::strcmp (argv[1], "--trace") == 0)
    {
        traceOn = true;
        Options o; o.flushIntervalMs = std::atof (argv[4]); o.echoWindowMs = std::atof (argv[5]);
        auto r = simulate ((unsigned) std::atoi (argv[2]), std::atoi (argv[3]), o, (float) std::atof (argv[6]), std::atof (argv[7]));
        std::printf ("%s\n", r.detail.c_str());
        return 0;
    }

    unitTests();
    convergenceTests (seeds);

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
