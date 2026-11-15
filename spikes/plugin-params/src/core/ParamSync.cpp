// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.

#include "ParamSync.h"

#include <cmath>
#include <utility>

namespace psync
{

ParamSync::ParamSync (std::string id, Options o)
    : memberId (std::move (id)), opts (o)
{
}

void ParamSync::seed (const std::string& name, float value)
{
    auto& p = params[name];

    if (! p.known)
    {
        p.value = value;
        p.known = true;
    }
}

Stamp ParamSync::nextStamp()
{
    return { ++clock, memberId };
}

void ParamSync::queue (Param& p, const std::string& name, std::uint8_t flags)
{
    if (p.pending)
    {
        p.pending->flags = static_cast<std::uint8_t> (p.pending->flags | flags);
        p.pending->value = p.value;
        p.pending->stamp = p.stamp;
        return;
    }

    p.pending = Change { name, p.value, p.stamp, flags };
}

void ParamSync::localGestureBegin (const std::string& name, double)
{
    auto& p = params[name];
    p.heldHere = true;

    // Someone here takes the control: what the plugin holds now is the starting
    // point, even if it was a change that was not sent.
    if (p.pluginDiffers)
    {
        p.value = p.pluginValue;
        p.pluginDiffers = false;
    }

    // Tells the others who holds it. Carries the current value and stamp, which
    // are not newer than what they have, so only the flag has an effect.
    if (p.known)
        queue (p, name, Flags::gestureBegin);
}

std::optional<Apply> ParamSync::localValue (const std::string& name, float value, double nowMs)
{
    ++counters.localEvents;
    auto& p = params[name];

    std::erase_if (p.recent, [nowMs] (const Param::Recent& r) { return r.untilMs < nowMs; });

    if (! p.heldHere && ! p.recent.empty())
    {
        for (const auto& r : p.recent)
        {
            if (std::abs (value - r.value) <= opts.epsilon)
            {
                if (std::abs (value - p.value) <= opts.epsilon)
                {
                    ++counters.echoes;
                    return std::nullopt;
                }

                // A late echo of an older value. The plugin may now hold it, so set the newest again.
                ++counters.echoesStale;
                return Apply { name, p.value };
            }
        }

        if (! p.heldBy.empty())
        {
            // Someone else is moving this control. A different value now is the plugin
            // rounding theirs, or a gesture-less control here fighting theirs. Theirs
            // wins; the plugin gets their next value even if it looks unchanged.
            ++counters.echoesAltered;
            p.pluginDiffers = true;
            p.pluginValue = value;
            return std::nullopt;
        }
    }

    if (p.known && std::abs (value - p.value) <= opts.epsilon)
    {
        ++counters.duplicates;
        return std::nullopt;
    }

    if (p.pending && p.pendingCarriesValue)
        ++counters.coalesced;

    p.value = value;
    p.known = true;
    p.pluginDiffers = false;
    p.stamp = nextStamp();
    queue (p, name, Flags::none);
    p.pendingCarriesValue = true;
    return std::nullopt;
}

void ParamSync::localGestureEnd (const std::string& name, double)
{
    auto& p = params[name];
    p.heldHere = false;

    if (! p.known)
        return;

    // The final value outranks everything this member has seen, including the
    // changes it ignored while holding the control, so it wins everywhere.
    p.stamp = nextStamp();
    queue (p, name, Flags::gestureEnd);
}

std::vector<Apply> ParamSync::receive (const std::string& from, const std::vector<Change>& changes, double nowMs)
{
    std::vector<Apply> out;

    for (const auto& c : changes)
    {
        ++counters.received;

        if (c.stamp.counter > clock)
            clock = c.stamp.counter;

        auto& p = params[c.param];
        bool holderLetGo = false;

        if ((c.flags & Flags::gestureBegin) != 0)
            p.heldBy = from;

        if ((c.flags & Flags::gestureEnd) != 0 && p.heldBy == from)
        {
            p.heldBy.clear();
            holderLetGo = true;
        }

        receiveValue (p, c, nowMs, out);

        // A change here was dropped while they held the control. If their last
        // value did not reset the plugin, set the agreed value now.
        if (holderLetGo && p.pluginDiffers && ! p.heldHere)
        {
            p.pluginDiffers = false;
            out.push_back ({ c.param, p.value });
        }
    }

    return out;
}

void ParamSync::receiveValue (Param& p, const Change& c, double nowMs, std::vector<Apply>& out)
{
    if (p.heldHere)
    {
        ++counters.heldDuringGesture;
        return;
    }

    if (p.known && ! (p.stamp < c.stamp))
    {
        ++counters.stale;

        // Someone tells us what our own plugin held when it saved a state (see
        // adoptState): a value it set without telling our host. A newer value
        // has been agreed since, but our plugin may still hold the old one.
        if (c.stamp.member == memberId && std::abs (c.value - p.value) > opts.epsilon)
            out.push_back ({ c.param, p.value });
        return;
    }

    p.stamp = c.stamp;
    dropOlderPending (p, c.stamp, c.value, counters);

    if (! p.known || p.pluginDiffers || std::abs (c.value - p.value) > opts.epsilon)
    {
        p.value = c.value;
        p.known = true;
        p.pluginDiffers = false;
        rememberApplied (p, c.value, nowMs);
        ++counters.applied;
        out.push_back ({ c.param, c.value });
    }
    else
    {
        ++counters.receivedSame;
    }
}

void ParamSync::dropOlderPending (Param& p, const Stamp& newer, float newValue, Stats& stats)
{
    // A change of ours still waiting to go out has lost; only send it if it
    // carries a gesture flag the others need.
    if (! p.pending || ! (p.pending->stamp < newer))
        return;

    if (p.pending->flags == Flags::none)
    {
        ++stats.coalesced;
        p.pending.reset();
    }
    else
    {
        p.pending->value = newValue;
        p.pending->stamp = newer;
    }

    p.pendingCarriesValue = false;
}

void ParamSync::rememberApplied (Param& p, float value, double nowMs)
{
    if (opts.echoWindowMs <= 0.0)
        return;

    std::erase_if (p.recent, [nowMs] (const Param::Recent& r) { return r.untilMs < nowMs; });
    if (p.recent.size() >= maxRecent)
        p.recent.erase (p.recent.begin());
    p.recent.push_back ({ value, nowMs + opts.echoWindowMs });
}

void ParamSync::observe (const Stamp& s)
{
    if (s.counter > clock)
        clock = s.counter;
}

ParamSync::Snapshot ParamSync::snapshot() const
{
    Snapshot out;

    for (const auto& [name, p] : params)
        if (p.known)
            out[name] = { p.value, p.stamp };

    return out;
}

ParamSync::Adopted ParamSync::adoptState (const Snapshot& sent, const Stamp& stateStamp,
                                         const std::map<std::string, float>& readback, double nowMs)
{
    observe (stateStamp);
    Adopted out;

    for (const auto& [name, value] : readback)
    {
        // What the state says this parameter is, and since when.
        float carried = value;
        Stamp since = stateStamp;
        bool silent = true;

        if (auto it = sent.find (name); it != sent.end() && std::abs (it->second.first - value) <= opts.epsilon)
        {
            carried = it->second.first;
            since = it->second.second;
            silent = false;
        }

        auto& p = params[name];

        if (p.heldHere)
        {
            // Someone here is holding it: their hand wins until they let go.
            if (p.known && std::abs (value - p.value) > opts.epsilon)
                out.apply.push_back ({ name, p.value });
            continue;
        }

        if (p.known && since < p.stamp)
        {
            // A newer change is known here: put it back if the state overwrote it.
            if (std::abs (value - p.value) > opts.epsilon)
            {
                rememberApplied (p, p.value, nowMs);
                out.apply.push_back ({ name, p.value });
            }

            // The sender's plugin holds a value its host does not know about.
            // Everyone else ignores this as older than what they have; the
            // sender learns that its plugin needs the agreed value set again.
            if (silent && stateStamp.member != memberId)
                notices.push_back ({ name, value, stateStamp, Flags::none });
            continue;
        }

        if (p.known && since == p.stamp)
            continue;

        p.value = carried;
        p.known = true;
        p.stamp = since;
        p.pluginDiffers = false;
        dropOlderPending (p, since, carried, counters);

        if (silent)
        {
            const Change c { name, carried, since, Flags::none };
            out.discovered.push_back (c);
            p.pending = c;
            p.pendingCarriesValue = true;
        }
    }

    return out;
}

std::vector<Apply> ParamSync::memberLeft (const std::string& member)
{
    std::vector<Apply> out;

    for (auto& [name, p] : params)
    {
        if (p.heldBy != member)
            continue;

        p.heldBy.clear();

        if (p.pluginDiffers && ! p.heldHere)
        {
            p.pluginDiffers = false;
            out.push_back ({ name, p.value });
        }
    }

    return out;
}

std::vector<Change> ParamSync::flush (double nowMs)
{
    std::vector<Change> out (std::make_move_iterator (notices.begin()), std::make_move_iterator (notices.end()));
    notices.clear();

    for (auto& [name, p] : params)
    {
        if (! p.pending)
            continue;

        const bool urgent = p.pending->flags != Flags::none;

        if (! urgent && nowMs - p.lastSentMs < opts.flushIntervalMs)
            continue;

        out.push_back (std::move (*p.pending));
        p.pending.reset();
        p.pendingCarriesValue = false;
        p.lastSentMs = nowMs;
    }

    if (! out.empty())
    {
        ++counters.sentBatches;
        counters.sentChanges += out.size();
    }

    return out;
}

bool ParamSync::hasPending() const noexcept
{
    if (! notices.empty())
        return true;

    for (const auto& [name, p] : params)
        if (p.pending)
            return true;

    return false;
}

std::optional<float> ParamSync::value (const std::string& name) const
{
    auto it = params.find (name);
    return it != params.end() && it->second.known ? std::optional<float> (it->second.value) : std::nullopt;
}

std::optional<Stamp> ParamSync::stamp (const std::string& name) const
{
    auto it = params.find (name);
    return it != params.end() && it->second.known ? std::optional<Stamp> (it->second.stamp) : std::nullopt;
}

bool ParamSync::isHeldHere (const std::string& name) const
{
    auto it = params.find (name);
    return it != params.end() && it->second.heldHere;
}

std::string ParamSync::heldBy (const std::string& name) const
{
    auto it = params.find (name);
    return it != params.end() ? it->second.heldBy : std::string();
}

} // namespace psync
