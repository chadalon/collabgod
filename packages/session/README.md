# `@collabgod/session`

The session model: what a project holds, every operation that changes it, and a replica that keeps one person's copy in step with the shared order and holds their undo history. Issues [#23](https://github.com/chadalon/collabgod/issues/23) and [#24](https://github.com/chadalon/collabgod/issues/24). The design is [ADR-0004](../../docs/adr/0004-ordered-edit-log.md) (one ordered edit log), [ADR-0014](../../docs/adr/0014-session-model-representation.md) (how the state is kept) and [ADR-0017](../../docs/adr/0017-project-file-v1.md) (who made what). Saving it is [`@collabgod/project`](../project).

It is pure TypeScript with no runtime dependencies and no Node, browser, interface or engine APIs, so it runs in a test, in the app, in the relay and in a browser (ENG-1). The type check enforces that: the package is compiled with the ES2023 library only.

## Run it

From the repository root (the package is part of the root npm workspace):

```sh
npm ci
npm run typecheck -w @collabgod/session
npm test -w @collabgod/session               # about 130 tests in about 15 seconds, the timing test included
FC_SCALE=40 npm test -w @collabgod/session   # the property tests with 40 times as many random cases
```

`npm run test:ts` runs it along with every other package.

## What is here

| File | What it holds |
| --- | --- |
| `src/types.ts` | The session's shapes and every kind of operation |
| `src/values.ts` | Units, limits and the rules each value must follow |
| `src/position.ts` | Position keys for track order |
| `src/session.ts` | `Session`: the state, and `apply`, the only way it changes |
| `src/replica.ts` | `Replica`: one person's copy, with optimistic edits, rollback, and undo and redo; `Sequencer` for working solo |
| `test/session.test.ts` | Every operation, its inverse and every rejection reason |
| `test/inverse.property.test.ts` | Random edits: every inverse is exact, and malformed operations change nothing |
| `test/convergence.property.test.ts` | Two, three and four replicas under random edits, undos, redos and delivery always end in the log's state |
| `test/undo.test.ts` | Undo and redo: solo and in a lobby, the named cases from the Phase 0 findings, removed targets, edits not yet confirmed, undo at the same moment as someone else's edit, and history across a new replica |
| `test/undo.property.test.ts` | Random runs: an undo puts back exactly what the shared order had before the edit; undoing everything solo gets back exactly to the start; solo and lobby agree |
| `test/replica.test.ts` | Two- and three-person stories, rejections, and how much a receive costs |
| `test/timing.test.ts` | The 4 ms budget on a 5,000-clip session |
| `test/sim.ts` | An in-memory lobby, the random edit generator and random runs |

## Using it

```ts
import { type Operation, opIdSource, Replica, Sequencer, trackPositionAt } from '@collabgod/session';

const me = new Replica({ author: memberId, nextOpId: opIdSource(`${memberId}:${randomRunId}:`) });

const result = me.submit({
  type: 'track.add',
  track: { id: newTrackId, name: 'Drums', position: trackPositionAt(me.view, me.view.trackOrder.length) },
});
if (result.applied) result.send.forEach(sendToRelay);

// Undo and redo work the same way, and say which edit they reversed and which they skipped.
const undone = me.undo();
if (undone.applied) undone.send.forEach(sendToRelay);

// In a lobby, everything the relay sends, in order. A receive can release operations to send.
const outcome = me.receive(message);
outcome.send.forEach(sendToRelay);
if (outcome.kind === 'rejected') tellThePerson(outcome.operation, outcome.reason);

// Solo there is no relay; a Sequencer gives the order instead, and sendToRelay is just:
const order = new Sequencer();
const sendToRelay = (operation: Operation): void => me.receive(order.next(operation)).send.forEach(sendToRelay);
```

**Always send what `send` holds**, from `submit`, `undo`, `redo` and `receive`, in the order you get it. It is usually the one operation just made, but an undo of an edit the relay has not confirmed yet waits, along with anything made after it, until that edit comes back (see Undo below).

The interface draws `me.view`. Nothing else may change the session: `view` is read-only, and the only mutator is `Session.apply`, which the replica calls.

**If `receive` throws, build a new replica** from a confirmed snapshot and the log after it, and give it the old one's `history`. A gap in `seq`, or your own operations coming back out of the order you sent them, means the connection broke its promise; nothing has changed when those are thrown. A failed rollback means an inverse was not exact, which is a bug in this package: the view may be half rolled back, so the replica marks itself `broken` and refuses every later call.

## Rules

- **Every change is an operation** with an id, the member who made it, and a change. `apply` is deterministic, never throws on what it is given, and checks every field, since operations come off the network.
- **An operation that cannot apply has no effect and says why**: `invalid`, `id-exists`, `target-missing`, `track-missing`, `out-of-range`, `guard-failed` or `no-change`. A replica reports its own operation as `rejected` when the relay's order shows it had no effect, so the interface can tell the person.
- **Every operation that applies returns its inverse**, which puts the state back exactly, revisions and authors included. Inverses are guarded: one applied later only writes fields still holding the revision the original left. A new kind of operation needs an exact inverse, and `test/inverse.property.test.ts` fails if it is not.
- **Time is in ticks**, 960 to a quarter note. One tempo, in quarter notes per minute (20 to 999), and one time signature per project. A clip's `offset` is in **seconds of its file**, so a tempo change does not change which audio a clip starts on.
- **A trim ties each edge to a point in the audio.** `clip.trim` takes `from`, `to` or both: seconds into the clip's file where it should start and end. The other edge stays put. `fileTimeAt(clip, tick, tempo)` turns the tick where a person dropped an edge into that point. The model turns each point back into a whole tick through where the clip's audio sits now and works out the offset from that tick, so a trim never moves the audio. After someone moves the clip or changes the tempo, a trim still lands on the same sound. Of two trims to the same edge at once, the later wins. A left edge cannot go past the start of the file or the timeline (`out-of-range`).
- **A clip refers to its sample by file hash, audio hash when known, and a file name** (ADR-0008). A name with a `/` or `\` in it, or any extra field on the sample, is refused, so a local path cannot get into the session.
- **Track order is a position key on each track**, sorted by key and then by id. Use `trackPositionAt(view, index, trackId)` to make one.
- **Who made what is part of the state** (SYN-9, ADR-0017). Beside each field's revision, `by` names the member whose operation wrote it, and every member, track and clip has `createdBy`, the member who added it (for the right half of a split, whoever split it). Inverses put both back exactly, so after an undo `by` names whoever made what is there again, and a removed clip comes back with its creator.
- **A session knows which project it is** (`projectId`, ADR-0017). It is given to `Session.empty` when the project is created, travels with every snapshot, and no operation changes it. A `Replica` is always started from a session, so it knows too.
- **Members are marked as removed, never deleted** (ADR-0006). Nothing here assumes two members.
- **A member carries a verifier**, fixed for life: what checks their secret, never the secret (ADR-0006). The one scheme so far is `sha256`, the SHA-256 of a secret of at least 128 random bits that the member's install keeps. `member.add` refuses a member without one, or a verifier with any other field in it; other fields on the member are dropped.
- **Fades are stored as set** and may add up to more than a trimmed clip. `effectiveFades` gives what should sound; the interface and the engine both use it.

## Undo

Undo is per member (SYN-5, ADR-0004). `undo()` sends the inverse of this member's latest edit still in effect as a new operation, which the relay orders like any other; `redo()` sends the inverse of the latest undo. Both work the same solo and in a lobby.

- **An undo applies only while every field it writes was last written by the edit it reverses**, compared by revision, never by value (`spikes/session-model` finding 2). It puts back the revision that was there before, so the undo before it, this member's or anyone's, is in effect again. Guards are per field: undoing a move keeps someone else's later change to the same clip's length.
- **An edit no longer in effect is skipped and forgotten**: someone else has written one of its fields, or removed what it changed, or removed the track a clip it removed would go back to. Undo goes on to the next older edit, and the result lists what it skipped. A forgotten edit stays forgotten even if the other person's undo later brings its target back. An edit the relay has not confirmed yet is different: if it has no effect on this replica's screen for now, undo passes over it but keeps it, because only the relay's order says whether it has an effect. If it has none, it comes back `rejected` and leaves the history then.
- **An edit that had no effect in the shared order is removed from the history** when it comes back `rejected` (finding 4).
- **A new edit clears redo.** Adding a member is not undoable (ADR-0014).
- **An undo is the inverse in the shared order.** An undo of an edit the relay has not confirmed yet shows at once, but it is sent only once that edit comes back, carrying the inverse worked out where the relay put it; anything made after the undo waits with it. Sent at once, it would carry the inverse against this replica's view, and if someone else's edit to the same thing was ordered first, the undo would wipe theirs out too. If the edit comes back rejected, the undo (and a redo of it) is dropped without being sent.
- **At the same moment as someone else's edit to the same thing**: if theirs is ordered first, the undo comes back `rejected` and theirs stands, and the next undo goes on to an older edit; if the undo is ordered first, it applies and their edit, being later, wins, after which a redo is skipped. Either way every machine agrees.
- **An undo puts back who made what as well**: each field it restores gets back its revision and its author, even on a replica built from a snapshot taken after that author's edit was overwritten.
- **The history belongs to the member, not the connection** (ADR-0006). A dropped connection leaves the replica, and its history, as they were. A new replica for the same member, after an error or a restart, takes the old one's `history`: plain data, confirmed entries only, refused for any other member. So if the app builds a new replica while some edits are sent but not confirmed, and the relay then orders them, those edits are in effect but cannot be undone.
- **`canUndo` and `canRedo` say whether that side holds anything, not whether any of it still applies.** Only trying tells. The interface should handle an undo or redo that finds nothing, for instance by greying the button out once it has.

## Operations

| Area | Operations a person makes | Inverses only |
| --- | --- | --- |
| Timeline | `tempo.set`, `timeSignature.set` | `timeline.patch` |
| Members | `member.add`, `member.rename`, `member.recolour`, `member.remove` | `member.patch`, `member.unadd` |
| Tracks | `track.add`, `track.remove`, `track.rename`, `track.move` | `track.patch`, and `track.add` and `track.remove` with their guards |
| Clips | `clip.add`, `clip.remove`, `clip.move`, `clip.trim`, `clip.split`, `clip.gain`, `clip.fade` | `clip.patch`, `clip.unsplit`, `clip.resplit`, and `clip.add` and `clip.remove` with their guards |

A split's `at` counts from the clip's start and a trim's edges are points in the audio, so both cut the same audio if someone moved the clip in the meantime. Moving a clip moves its audio with it; trimming it never does.

## Not here yet

- A limit on how long the undo history grows, and grouping several operations into one undo step. The project file saves the history as it is (`@collabgod/project`).
- Making a member's secret (#161) and checking it (#36). The model only holds the verifier.
- Resending unconfirmed operations after a reconnect (Phase 2, LOB-6). Operation ids already make a resent operation recognisable.
- A feed of what changed for the engine projection (#29) and the timeline (#31). See ADR-0014.
- Mute and the rest of the mixer, MIDI, automation and plugins: later phases.
- A build step. Consumers import the TypeScript source; the workspace in #19 decides how it is built.
