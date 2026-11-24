# `@collabgod/session`

The session model: what a project holds, every operation that changes it, and a replica that keeps one person's copy in step with the shared order. Issue [#23](https://github.com/chadalon/collabgod/issues/23). The design is [ADR-0004](../../docs/adr/0004-ordered-edit-log.md) (one ordered edit log) and [ADR-0014](../../docs/adr/0014-session-model-representation.md) (how the state is kept).

It is pure TypeScript with no runtime dependencies and no Node, browser, interface or engine APIs, so it runs in a test, in the app, in the relay and in a browser (ENG-1). The type check enforces that: the package is compiled with the ES2023 library only.

## Run it

```sh
cd packages/session
npm ci
npm run typecheck
npm test               # about 70 tests in about 10 seconds, the timing test included
FC_SCALE=40 npm test   # the property tests with 40 times as many random cases
```

## What is here

| File | What it holds |
| --- | --- |
| `src/types.ts` | The session's shapes and every kind of operation |
| `src/values.ts` | Units, limits and the rules each value must follow |
| `src/position.ts` | Position keys for track order |
| `src/session.ts` | `Session`: the state, and `apply`, the only way it changes |
| `src/replica.ts` | `Replica`: one person's copy, with optimistic edits and rollback; `Sequencer` for working solo |
| `test/session.test.ts` | Every operation, its inverse and every rejection reason |
| `test/inverse.property.test.ts` | Random edits: every inverse is exact, and malformed operations change nothing |
| `test/convergence.property.test.ts` | Two, three and four replicas under random delivery always end in the log's state |
| `test/replica.test.ts` | Two- and three-person stories, rejections, and how much a receive costs |
| `test/timing.test.ts` | The 4 ms budget on a 5,000-clip session |
| `test/sim.ts` | An in-memory lobby and the random edit generator |

## Using it

```ts
import { opIdSource, Replica, Sequencer, trackPositionAt } from '@collabgod/session';

const me = new Replica({ author: memberId, nextOpId: opIdSource(`${memberId}:${randomRunId}:`) });

const result = me.submit({
  type: 'track.add',
  track: { id: newTrackId, name: 'Drums', position: trackPositionAt(me.view, me.view.trackOrder.length) },
});
if (result.applied) send(result.operation); // in a lobby: to the relay

// Solo there is no relay; a Sequencer gives the order instead.
const order = new Sequencer();
if (result.applied) me.receive(order.next(result.operation));

// In a lobby, everything the relay sends, in order:
const outcome = me.receive(message);
if (outcome.kind === 'rejected') tellThePerson(outcome.operation, outcome.reason);
```

The interface draws `me.view`. Nothing else may change the session: `view` is read-only, and the only mutator is `Session.apply`, which the replica calls.

## Rules

- **Every change is an operation** with an id, the member who made it, and a change. `apply` is deterministic, never throws on what it is given, and checks every field, since operations come off the network.
- **An operation that cannot apply has no effect and says why**: `invalid`, `id-exists`, `target-missing`, `track-missing`, `out-of-range`, `guard-failed` or `no-change`. A replica reports its own operation as `rejected` when the relay's order shows it had no effect, so the interface can tell the person.
- **Every operation that applies returns its inverse**, which puts the state back exactly, revisions included. Inverses are guarded: one applied later only writes fields still holding the revision the original left. A new kind of operation needs an exact inverse, and `test/inverse.property.test.ts` fails if it is not.
- **Time is in ticks**, 960 to a quarter note. One tempo, in quarter notes per minute (20 to 999), and one time signature per project. A clip's `offset` is in **seconds of its file**, so a tempo change does not change which audio a clip starts on.
- **A trim never moves the audio.** `clip.trim` takes the new `start`, the new `end`, or both, and the other edge stays put. The model works out the offset from how far the left edge moved, at the session's tempo, so the interface never sends one. A left edge cannot go past the start of the file (`out-of-range`).
- **A clip refers to its sample by file hash, audio hash when known, and a file name** (ADR-0008). A name with a `/` or `\` in it, or any extra field on the sample, is refused, so a local path cannot get into the session.
- **Track order is a position key on each track**, sorted by key and then by id. Use `trackPositionAt(view, index, trackId)` to make one.
- **Members are marked as removed, never deleted** (ADR-0006). Nothing here assumes two members.
- **Fades are stored as set** and may add up to more than a trimmed clip. `effectiveFades` gives what should sound; the interface and the engine both use it.

## Operations

| Area | Operations a person makes | Inverses only |
| --- | --- | --- |
| Timeline | `tempo.set`, `timeSignature.set` | `timeline.patch` |
| Members | `member.add`, `member.rename`, `member.recolour`, `member.remove` | `member.patch`, `member.unadd` |
| Tracks | `track.add`, `track.remove`, `track.rename`, `track.move` | `track.patch`, and `track.add` and `track.remove` with their guards |
| Clips | `clip.add`, `clip.remove`, `clip.move`, `clip.trim`, `clip.split`, `clip.gain`, `clip.fade` | `clip.patch`, `clip.unsplit`, `clip.resplit`, and `clip.add` and `clip.remove` with their guards |

A split's `at` counts from the clip's start, so it cuts the same audio if someone moved the clip in the meantime. Moving a clip moves its audio with it; trimming it never does.

## Not here yet

- Undo and redo history per member (#24). `submit` returns each edit's inverse, and the replica recomputes the inverses of its unconfirmed edits whenever it rolls them back and reapplies them; #24 builds the history and its named tests on top.
- The project file (#25), and checking a member's secret (#25, #36).
- Resending unconfirmed operations after a reconnect (Phase 2, LOB-6). Operation ids already make a resent operation recognisable.
- A feed of what changed for the engine projection (#29) and the timeline (#31). See ADR-0014.
- Mute and the rest of the mixer, MIDI, automation and plugins: later phases.
- A build step. Consumers import the TypeScript source; the workspace in #19 decides how it is built.
