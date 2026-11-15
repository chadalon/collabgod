// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.
//
// The rules for mirroring plugin parameter changes between members, with no
// dependency on JUCE, Tracktion Engine or a network. The host feeds it what the
// local plugin reported and what arrived from others; it says what to send and
// what to apply to the local plugin. Everything is deterministic given the calls
// and the times passed in, so the tests drive it with a simulated clock.
//
// The rules, in short (README "How the mirroring works"):
// - Last writer wins, per parameter, ordered by a Lamport stamp (counter, member).
// - A change that only repeats the value already known is never sent.
// - After remote values are applied, a change the plugin reports without a
//   gesture, within the echo window, with one of those values, is the plugin
//   repeating it and is not sent. If it repeats an older one, the newest is set
//   again. A different value is a real change and is sent, unless someone else
//   holds the control: then it is dropped, and when they let go the plugin is
//   set to the agreed value.
// - While a person holds a control (a gesture), remote changes to that parameter
//   are not applied on their machine; when they let go, their final value gets a
//   stamp newer than everything they have seen, so it wins everywhere. The last
//   person to let go wins.
// - Outgoing changes are coalesced per parameter and sent at most once per flush
//   interval. A gesture's start and end are sent at once.
//
// Nothing here assumes two people: stamps carry the member id, and any number of
// members converge on the change with the highest stamp.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace psync
{

/** Orders changes. Higher counter wins; equal counters are broken by member id. */
struct Stamp
{
    std::uint64_t counter = 0;
    std::string member;

    friend bool operator< (const Stamp& a, const Stamp& b)
    {
        return a.counter != b.counter ? a.counter < b.counter : a.member < b.member;
    }
    friend bool operator== (const Stamp& a, const Stamp& b) { return a.counter == b.counter && a.member == b.member; }
};

/** Bits in Change::flags. */
enum Flags : std::uint8_t
{
    none         = 0,
    gestureBegin = 1, ///< the sender started holding this control
    gestureEnd   = 2  ///< the sender let go; the value is their final one
};

/** One parameter's new value as it travels. `value` is normalised, 0 to 1. */
struct Change
{
    std::string param;
    float value = 0.0f;
    Stamp stamp;
    std::uint8_t flags = Flags::none;
};

/** A value the host should set on the local plugin. */
struct Apply
{
    std::string param;
    float value = 0.0f;
};

struct Options
{
    /** Outgoing changes to one parameter are sent at most this often. 0 sends every change. */
    double flushIntervalMs = 33.0;
    /** How long after applying a remote value a gesture-less change from the plugin counts as its echo. 0 turns echo suppression off. */
    double echoWindowMs = 250.0;
    /** Values closer than this are the same value. */
    float epsilon = 1.0e-6f;
};

/** Counters for the findings. All are totals since construction. */
struct Stats
{
    std::uint64_t localEvents = 0;          ///< changes the host reported from this machine
    std::uint64_t duplicates = 0;           ///< local changes that repeated the known value, not sent
    std::uint64_t echoes = 0;               ///< local changes inside the echo window with the latest applied value, not sent
    std::uint64_t echoesStale = 0;          ///< the same, but with an older applied value; the newest was set again
    std::uint64_t echoesAltered = 0;        ///< gesture-less local changes with another value while someone else holds the control, not sent
    std::uint64_t coalesced = 0;            ///< local changes replaced by a later one before they were sent
    std::uint64_t sentChanges = 0;
    std::uint64_t sentBatches = 0;
    std::uint64_t received = 0;
    std::uint64_t applied = 0;              ///< remote changes that moved the local plugin
    std::uint64_t receivedSame = 0;         ///< newer remote changes with the value already here, nothing to apply
    std::uint64_t stale = 0;                ///< remote changes older than what is known, ignored
    std::uint64_t heldDuringGesture = 0;    ///< remote changes ignored because someone here holds the control
};

class ParamSync
{
public:
    ParamSync (std::string memberId, Options = {});

    const std::string& member() const noexcept   { return memberId; }
    const Options& options() const noexcept      { return opts; }
    void setOptions (Options o)                  { opts = o; }

    /** Seeds the known value of a parameter without sending it, for example when
        the plugin is loaded. Does nothing if the parameter already has a value. */
    void seed (const std::string& param, float value);

    //==========================================================================
    // From this machine: the plugin's own window, or the host's controls.

    void localGestureBegin (const std::string& param, double nowMs);
    /** Returns a value to set on the local plugin if the change was a late echo
        of an older remote value, so the plugin goes back to the newest. */
    std::optional<Apply> localValue (const std::string& param, float value, double nowMs);
    void localGestureEnd (const std::string& param, double nowMs);

    //==========================================================================
    // From other members.

    /** Takes changes sent by another member, `from`, in the order they were sent.
        `from` is who sent them, as the relay stamps it; a change's stamp may
        name someone else, whose value it carries. Returns
        the values to set on the local plugin. The host should set them in a way
        that does not report them back here (see README, "Echoes"). */
    std::vector<Apply> receive (const std::string& from, const std::vector<Change>&, double nowMs);

    /** Forgets that a member holds any control, for example when they disconnect.
        Returns values to set on the local plugin, as receive() does. */
    std::vector<Apply> memberLeft (const std::string& member);

    //==========================================================================

    /** Returns what to send now, or nothing. Call it regularly: the host calls it
        every few milliseconds, and it holds changes back to honour the interval. */
    std::vector<Change> flush (double nowMs);

    /** True if something is waiting to be sent. */
    bool hasPending() const noexcept;

    //==========================================================================
    // State, for tests and the window.

    std::optional<float> value (const std::string& param) const;
    std::optional<Stamp> stamp (const std::string& param) const;
    bool isHeldHere (const std::string& param) const;
    /** Who else holds the control right now, or empty. */
    std::string heldBy (const std::string& param) const;
    const Stats& stats() const noexcept      { return counters; }

private:
    struct Param
    {
        float value = 0.0f;
        bool known = false;
        Stamp stamp;
        bool heldHere = false;
        std::string heldBy;              // another member holding it, from their gesture flags
        std::optional<Change> pending;   // waiting for the next flush
        bool pendingCarriesValue = false; // pending holds a local value change, not only a gesture flag
        double lastSentMs = -1.0e12;
        struct Recent { float value; double untilMs; };
        std::vector<Recent> recent;      // remote values applied here within the echo window, oldest first
        bool pluginDiffers = false;      // a local change was dropped, so the plugin may not hold `value`
        float pluginValue = 0.0f;        // what it holds instead, when pluginDiffers
    };

    static constexpr std::size_t maxRecent = 32;

    void receiveValue (Param&, const Change&, double nowMs, std::vector<Apply>&);

    Stamp nextStamp();
    void queue (Param&, const std::string& name, std::uint8_t flags);

    std::string memberId;
    Options opts;
    std::uint64_t clock = 0;
    std::map<std::string, Param> params;
    Stats counters;
};

} // namespace psync
