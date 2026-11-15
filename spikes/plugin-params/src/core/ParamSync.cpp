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
        return;
    }

    p.stamp = c.stamp;

    // A change of ours still waiting to go out has lost; only send it if it
    // carries a gesture flag the others need.
    if (p.pending && p.pending->stamp < c.stamp)
    {
        if (p.pending->flags == Flags::none)
        {
            ++counters.coalesced;
            p.pending.reset();
            p.pendingCarriesValue = false;
        }
        else
        {
            p.pending->value = c.value;
            p.pending->stamp = c.stamp;
            p.pendingCarriesValue = false;
        }
    }

    if (! p.known || p.pluginDiffers || std::abs (c.value - p.value) > opts.epsilon)
    {
        p.value = c.value;
        p.known = true;
        p.pluginDiffers = false;

        if (opts.echoWindowMs > 0.0)
        {
            std::erase_if (p.recent, [nowMs] (const Param::Recent& r) { return r.untilMs < nowMs; });
            if (p.recent.size() >= maxRecent)
                p.recent.erase (p.recent.begin());
            p.recent.push_back ({ c.value, nowMs + opts.echoWindowMs });
        }

        ++counters.applied;
        out.push_back ({ c.param, c.value });
    }
    else
    {
        ++counters.receivedSame;
    }
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
    std::vector<Change> out;

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
