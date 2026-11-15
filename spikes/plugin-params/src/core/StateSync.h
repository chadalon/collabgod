// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.
//
// The rules for syncing the part of a plugin that is not a parameter (a loaded
// preset, a wavetable, an LFO shape, a modulation assignment), with no
// dependency on JUCE, Tracktion Engine or a network. The only thing a host can
// do with that part is save the plugin's whole state and load it somewhere
// else, so that is what travels. The host reads the state when these rules ask,
// hashes it, and passes the hash in; the rules say when to announce it, when
// to fetch someone else's, and when to apply it.
//
// In short (README-STATE.md, "How state sync works"):
// - The hidden part of a plugin is one value with one owner at a time: the
//   newest announced state wins as a whole, ordered by the same Lamport stamps
//   as parameter changes. There is no merging inside an opaque state.
// - Parameters keep their own last-writer-wins order. A state carries its
//   sender's parameter values and stamps from when it was read; a parameter
//   known to be newer is put back after applying it. A parameter the state holds
//   differently from its sender's snapshot was changed by the plugin without
//   telling the host (a preset load, in some plugins): it counts as written at
//   the state's stamp and is sent on as a parameter change, so everyone learns it.
// - A change is found by reading the state: when the plugin hints that
//   something changed (VST3 setDirty, a restart, a burst of parameter reports
//   that change nothing), and optionally by polling. It is announced once the
//   state has been read unchanged for a quiet period and nobody here holds a control.
// - An announcement carries the state's hash, not its bytes. A member whose
//   plugin already produces that hash has it (the change was only parameters,
//   which parameter sync already carried) and fetches nothing.
// - After a state is applied, changes the plugin makes while it settles are
//   taken as the new baseline, not announced, so applying never echoes.
// - A change that appears after parameters arrived from someone else, with no
//   sign of anything done here, is those parameters and is not announced.
//
// Nothing here assumes two people.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ParamSync.h"

namespace psync
{

struct StateOptions
{
    /** A local change is announced once the state has been read unchanged for this long. */
    double quietMs = 300.0;
    /** A hint schedules a read this long after it, so a burst of hints costs one read. */
    double hintDelayMs = 30.0;
    /** A change is confirmed only once no parameter has moved here for this long: a value set on a plugin
        reaches its saved state only when it next processes audio, so a read straight after can miss it. */
    double paramQuietMs = 100.0;
    /** Read the state this often to find changes the plugin does not hint at. 0 turns polling off. */
    double pollMs = 0.0;
    /** Polling may take at most this share of the time; slow reads stretch the interval. */
    double pollBudget = 0.02;
    /** After applying another member's state, changes are taken as the plugin settling for this long
        (or two polling intervals, if longer), unless a person does something here first. */
    double settleMs = 1500.0;
    /** A member whose state differs from an announced one reads it once more after this long before fetching,
        because a parameter set from elsewhere reaches some plugins' saved state only once they process audio. 0 fetches at once. */
    double recheckMs = 150.0;
    /** A hint or local activity within this long makes a change "prompted". */
    double promptMs = 2000.0;
    /** This many unprompted changes in a row, with nothing done here, mark the plugin's state as noisy: polling stops. */
    int noisyAfter = 4;
};

/** A state as it is announced. `hash` is the SHA-256 of the bytes its sender saved. */
struct StateRef
{
    std::string hash;
    std::uint64_t size = 0;
    Stamp stamp;
};

/** What a member sends with the bytes of its state: the state, and its parameters as it knew them when it read it. */
struct StateOffer
{
    StateRef ref;
    ParamSync::Snapshot params;
};

enum class Need : std::uint8_t
{
    nothing,    ///< older than what is known, or already here
    recheck,    ///< read the state again after recheckMs and call recheck()
    fetch       ///< fetch the bytes for `hash` from `from`, then call arrived()
};

struct Step
{
    Need need = Need::nothing;
    std::string hash, from;
    /** Parameter values to set on the local plugin. */
    std::vector<Apply> apply;
};

struct StateStats
{
    std::uint64_t reads = 0;
    double readMsTotal = 0.0, readMsMax = 0.0;
    std::uint64_t hints = 0;
    std::uint64_t unchangedReads = 0;
    std::uint64_t localChanges = 0;          ///< reads that found a change made here
    std::uint64_t absorbedSettling = 0;      ///< changes while a remote state settled, taken as the baseline
    std::uint64_t absorbedRemoteParams = 0;  ///< changes explained by parameters from someone else, not announced
    std::uint64_t unprompted = 0;            ///< changes found with no hint or local activity nearby
    std::uint64_t announced = 0;
    std::uint64_t forced = 0;                ///< announced early because a newer state arrived during a local change
    std::uint64_t received = 0;
    std::uint64_t stale = 0;                 ///< older than the newest known, ignored
    std::uint64_t matched = 0;               ///< this plugin already had it: nothing fetched
    std::uint64_t rechecks = 0;
    std::uint64_t fetches = 0;
    std::uint64_t superseded = 0;            ///< bytes that arrived after a newer state was announced, dropped
    std::uint64_t applied = 0;
    std::uint64_t appliedDeferred = 0;       ///< applies held back while a control was held here
    std::uint64_t readbackDiffers = 0;       ///< applied states the plugin saved back differently
    std::uint64_t paramsPutBack = 0;         ///< parameters set again after a state carried an older value
    std::uint64_t paramsDiscovered = 0;      ///< parameters a state changed without its sender knowing, sent on
};

class StateSync
{
public:
    using ReadParams = std::function<std::map<std::string, float>()>;

    /** `params` is the member's parameter sync, which shares the clock. `readParams`
        returns what the plugin holds now, by parameter ID. */
    StateSync (ParamSync& params, ReadParams readParams, StateOptions = {});

    const StateOptions& options() const noexcept { return opts; }
    void setOptions (StateOptions o)             { opts = o; }

    /** The plugin was loaded: its state is the baseline. It has stamp 0 until
        someone changes it, so a state anyone has touched wins over an untouched one. */
    void loaded (const std::string& hash, std::uint64_t size);

    //==========================================================================
    // Finding local changes

    /** The plugin hinted that its state may have changed. */
    void hint (double nowMs);
    /** Something was done on this machine that can change the state: a parameter
        the plugin or this host changed. Makes a change found soon after "prompted". */
    void localActivity (double nowMs);
    /** Parameters or a state from someone else were applied here. */
    void remoteActivity (double nowMs);

    /** True if the host should read the state now. `pollAllowed` lets the host
        restrict polling, for example to while the plugin's window is open. */
    bool readDue (double nowMs, bool pollAllowed) const;
    /** The host read the state, which took `readMs`. */
    void observed (const std::string& hash, std::uint64_t size, double readMs, double nowMs);

    /** Returns a state to announce, if a local change has been seen unchanged for
        quietMs and no control is held here. The host keeps the bytes it read for
        that hash, with the offer's parameters, to send to whoever asks. */
    std::optional<StateOffer> flush (double nowMs, bool gestureHeld);

    /** Announces the current state now, whatever the rules say: the person asked to. */
    StateOffer announceNow (const std::string& hash, std::uint64_t size);

    /** The newest agreed state, with its own stamp, to tell a member who just joined. */
    std::optional<StateRef> current() const;

    //==========================================================================
    // Others' states

    /** Another member announced a state. `currentHash` is what this plugin saves now. */
    Step receive (const std::string& from, const StateRef&, const std::string& currentHash, double nowMs);
    /** True when a recheck asked for by receive() is due. */
    bool recheckDue (double nowMs) const;
    Step recheck (const std::string& currentHash, double nowMs);

    /** The bytes for `hash` arrived (and match it), with the parameters its
        sender sent along. True if they are still wanted: then call applyDue()
        until it returns them, and applied() once done. */
    bool arrived (const std::string& hash, ParamSync::Snapshot sentParams);
    /** Returns the hash of a state to apply now, or nothing. Applying waits while a control is held here. */
    std::optional<std::string> applyDue (bool gestureHeld);
    /** The host applied the state and read it back. Returns parameter values to
        set again; parameters the state changed silently are queued in parameter sync to send. */
    std::vector<Apply> applied (const std::string& readbackHash, std::uint64_t size, double nowMs);

    /** A member left. If their bytes were being waited for, stop waiting. */
    void memberLeft (const std::string& member);

    //==========================================================================
    const std::string& baseline() const noexcept   { return base; }
    const Stamp& agreedStamp() const noexcept      { return agreed.stamp; }
    const std::string& agreedHash() const noexcept { return agreed.hash; }
    bool isNoisy() const noexcept                  { return noisy; }
    void setNoisy (bool n) noexcept                { noisy = n; }
    bool hasLocalChange() const noexcept           { return pending.has_value(); }
    bool isWaiting() const noexcept                { return ! want.hash.empty() || pendingApply.has_value(); }
    double pollIntervalMs() const;
    const StateStats& stats() const noexcept       { return counters; }

private:
    Step matchedHere();
    StateOffer announce (const std::string& hash, std::uint64_t size, ParamSync::Snapshot);
    bool prompted (double nowMs) const;
    double confirmAtMs() const;

    ParamSync& params;
    ReadParams readParams;
    StateOptions opts;

    StateRef agreed;               // the newest state anyone announced
    std::string base;              // what this plugin saves when it holds the agreed state

    struct Pending { std::string hash; std::uint64_t size; double changedMs; bool confirmed; ParamSync::Snapshot params; };
    std::optional<Pending> pending; // a local change not announced yet
    bool force = false;             // announce it at the next flush

    struct Want { std::string hash, from; std::optional<double> recheckAtMs; };
    Want want;
    std::optional<std::string> pendingApply;
    ParamSync::Snapshot sentParams;   // with the bytes being applied

    std::optional<double> readAtMs;  // a read asked for by a hint or a quiet check
    double lastReadMs = -1.0e12;
    std::string lastHash;
    double prevReadMs = -1.0e12;
    double lastHintMs = -1.0e12;
    double lastLocalMs = -1.0e12;
    double lastRemoteMs = -1.0e12;
    double settleUntilMs = -1.0e12;
    double appliedAtMs = -1.0e12;
    int unpromptedStreak = 0;
    bool noisy = false;

    StateStats counters;
};

//==============================================================================
// Helpers, pure and tested on their own.

/** SHA-256 of a block of bytes, as lowercase hex (as ADR-0008 names samples). */
std::string sha256Hex (const void* data, std::size_t size);

/** Text in a saved state that looks like a file path: an absolute Windows,
    macOS or Linux path, a file:// URL, or a name ending in an audio, sample or
    preset extension. Finds 8-bit and UTF-16 text; finds nothing in compressed or
    encrypted state. Each string is returned once, in the order found. */
std::vector<std::string> findPathLikeStrings (const void* data, std::size_t size, std::size_t maxResults = 64);

/** Replaces a home folder at the start of a path with "~", so a report can be
    pasted without the person's user name. */
std::string redactHome (const std::string& path);

/** What applying a state did, from what the host can see. */
enum class ApplyOutcome : std::uint8_t
{
    exact,           ///< the plugin saves exactly what it was given
    resaved,         ///< it saves something else, but every parameter matches the sender's
    paramsDiffer,    ///< it took the state, but some parameters differ from the sender's: a silent difference
    unchanged        ///< it saves what it saved before: it refused the state, or ignored it
};

struct ApplyReport
{
    ApplyOutcome outcome = ApplyOutcome::exact;
    std::vector<std::string> differing;   ///< parameter IDs that differ, up to 20
    std::size_t compared = 0;
};

/** Classifies an apply. `sent` are the sender's parameter values, `here` what
    the plugin holds after applying; only IDs both have are compared. */
ApplyReport classifyApply (const std::string& receivedHash, const std::string& hashBefore, const std::string& readbackHash,
                           const std::map<std::string, float>& sent, const std::map<std::string, float>& here,
                           float tolerance = 1.0e-4f);

const char* toString (ApplyOutcome);

} // namespace psync
