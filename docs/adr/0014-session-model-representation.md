# ADR-0014: The session model is one mutable state with exact inverses, timed in ticks, with tracks ordered by key

- **Status:** Proposed
- **Date:** 2026-11-23
- **Deciders:** Chad

## Context

Issue #23 turns the session-model spike into the package everything else builds on (`packages/session`). Phase 0 left it four things to settle, each of which ends up in every operation and in the project file (#25), so they are expensive to change once projects exist:

- **Speed.** The spike copied the whole state for every operation and took 41 ms to receive one remote edit on a 5,000-clip session with 20 unconfirmed local edits (ADR-0004, `session-model` finding 5). The budget Chad accepted is 4 ms, and the cost must grow with the number of unconfirmed edits, not the size of the session.
- **Time.** The spike had no tempo and no clip offset, so a trim from the left could not be expressed (`engine-projection` finding 9).
- **Track order.** The spike placed tracks by list index and never tested reordering.
- **Undo.** Per-person undo (#24) needs every field to carry the operation that last wrote it, and every inverse to be guarded by those revisions (ADR-0004).

## Decision

1. **One mutable state per replica, rolled back by exact inverses.** Every operation that applies returns an inverse that puts the state back exactly, revisions included. When someone else's operation arrives, a replica undoes its unconfirmed operations newest first, applies the new one, and applies its own again. When its own operation comes back from the relay in order, nothing is rolled back. The objects the session hands out are frozen and replaced, never changed.
2. **Revisions are in the state from the start, and every inverse is guarded.** The inverse that rollback uses is the same one undo will send. Inverses of structural operations carry exact copies of what they put back (`clip.unsplit` and `clip.resplit` carry the right half of a split), so applying one never recomputes anything.
3. **Positions and lengths are whole ticks, 960 to a quarter note.** The project has one tempo, in quarter notes per minute, and one time signature (DAW-5); both change only by operation. **A clip's offset into its file is in seconds of the file**, not in ticks. **A trim ties each edge it moves to a point in the clip's audio**, in seconds into the file. The model turns that point into a whole tick through where the audio sits now, and works out the offset from that tick, so a trim never moves the audio and keeps its meaning after someone moves the clip or changes the tempo.
4. **Each track holds a position key, and the track list is sorted by key, then by id.** A key is a base-36 fraction; a reorder writes a new key between the new neighbours' keys. A reorder is then a write to one field of one track, with its own revision.
5. **Operation ids are opaque strings the replica makes** from a prefix the app supplies, which must not repeat when the app starts again. The model never makes ids itself and has no randomness.

On PR #152 (2026-11-23) Chad agreed to 1 and 4, and for 3 asked for whichever choice keeps editing from feeling glitchy. Decision 3 as written is that choice. On 2026-11-24 he asked that a trim keep its meaning after someone moves the clip, and chose edges tied to points in the audio over signed distances from each edge.

## Alternatives considered

- **Structural sharing** (persistent maps, so each version shares most of the last). It would also meet the budget, but needs a persistent map, written here or taken as a dependency, and every lookup on every frame pays for it. Rollback by inverses reuses the inverses undo needs anyway.
- **Clip offset in ticks.** All integer arithmetic. But v1's audio is not stretched, and with a tick offset a tempo change moves where in its file every trimmed clip starts: a clip trimmed to start on a drum hit would start slightly before or after it. It is not needed for stretching either (see Consequences).
- **Trim edges as ticks on the timeline**, as this record first had it. A trim ordered after someone else's move of the same clip trims the wrong amount: a half-bar trim of a clip moved half a bar left takes a whole bar, and a move the other way can lengthen the clip.
- **Trim edges as signed distances from where each edge is.** That keeps a trim's meaning after a move, but two trims of the same edge at once add up instead of the later one winning, and a tempo change ordered first changes how much audio the trim takes.
- **A trim that carries the new offset**, worked out by the interface. Two faults: if someone's tempo change is ordered just before it, the offset was worked out at the old tempo and the audio jumps (by 20 ms for a one-beat trim from 120 to 125 BPM); and a trim that sets both edges undoes another person's trim of the other edge at the same moment.
- **Track order by list index**, as the spike did. Simplest, but a move made while someone else adds a track above lands one place off, and an index is not a field, so undo cannot guard it.
- **Track order by "after track X".** Keeps the intent, but fails when track X is removed at the same time.

## Consequences

- A reader cannot hold an old version of the whole session for free. It can keep individual objects and compare them by identity, or take a snapshot, which costs time in proportion to the session. The engine projection (#29) and the timeline (#31) will probably want a feed of what changed; every write goes through a few private methods, so adding one is a local change.
- Every new kind of operation needs an exact inverse. The inverse property test fails for one that is not exact.
- A tempo change keeps each clip on its beats and keeps the point in its file where it starts. Its length in ticks stays, so it plays a little more or less of its file, and unstretched audio drifts against the grid inside the clip, as in any DAW. Stretching to the tempo (DAW-8) fixes the drift and keeps the offset in seconds: a stretched clip converts ticks to seconds of its file at its own source tempo, which never changes, instead of at the project's.
- For a clip that is not stretched, a tempo change after a split opens the seam between the halves. Each half's length is in ticks but its audio is not stretched: when the tempo goes down, the left half plays on past the cut and the audio around it plays twice; when it goes up, the left half stops short and audio is skipped. The same happens to any two clips trimmed to butt up against each other. This is the expected trade-off for unstretched audio; stretching (#66, DAW-8) fixes it for stretched clips.
- A clip's offset is a floating-point number. Trim and split compute it from the tempo the same way on every machine, so replicas agree bit for bit. A trim back to the start of the file snaps to exactly zero; otherwise rounding can land it a hair either side, and a hair below would refuse the trim. Edges land on whole ticks, so when the start of a file falls between two ticks, as it can after a tempo change, a trim to it stops at the first tick inside the file.
- Two people trimming the two edges of one clip at the same moment both keep their trim, because each trim moves only the edges it names. Two people trimming the same edge at the same moment: the later one in the shared order wins, because each names a point in the audio, so the trims do not add up. A trim ordered after someone else's move of the same clip still lands on the same sound.
- Keys grow slowly: by one digit every 35 tracks added at the end, and by about one digit every 5 moves into the same gap. Keys longer than 1,024 characters are refused, which takes thousands of moves into one gap. Two tracks added at the same spot at the same moment get the same key and are ordered by id.
- The model accepts any author. Checking that the author is a member who has not been removed is the relay's job (ADR-0006, #36).
- `member.unadd` exists only so an unconfirmed `member.add` can be rolled back. Members are never deleted otherwise, and undo (#24) should not offer it.

## How we will know

The timing test kept with the package, which asserts the 95th percentile of 500 receives in the budget case. Against the spike's 41 ms: on the machine that wrote it, 0.09 ms at the median; on Chad's machine, 0.07 ms at the median and a slowest single receive of 3.0 ms; in CI on PR #152, 0.09 to 0.20 ms at the 95th percentile on macOS, Linux and Windows, with a slowest single receive of 10.8 ms, once in 500, on the Windows runner. Every receive in that case does the same work, 41 applies (two per unconfirmed edit and one more, as a test counts), so the outliers are most likely garbage collection or the runner's scheduling, not the algorithm. Garbage collection pauses the whole app, not only the model; if they show up in real sessions, that is a question for the app as a whole, measured on the maintainers' machines. If #29 or #31 finds that reading the session costs too much without a change feed, add one, not a second copy of the state. If reorders surprise people at the Phase 2 gate, revisit the position keys. When stretching (#66) is built, check that stretched clips convert ticks to seconds at their own source tempo, as above, and that it closes the split seam for them.
