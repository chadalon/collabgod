# Engine projection contract

What the TypeScript side sends the engine, and what the engine reports back. Both sides test themselves against the examples in [`examples`](examples): the C++ self-test runs them through Tracktion Engine, and `ts/test/contract.test.ts` runs them through the TypeScript reference (`ts/src/commands.ts`). If both pass, the two agree.

The engine side has no logic about the session. It carries out primitive commands and reports what Tracktion holds. Every decision (what changed, in what order to send it) is made in TypeScript.

## Transport

One JSON object per line. Requests go to `engine_projection serve` on stdin; each gets one reply on stdout with the same `seq`. The engine's log goes to stderr. In the app the same messages would cross the web view bridge from spike #8; stdio stands in for it so that Node can drive the engine in tests.

| Request | Reply |
| --- | --- |
| `{ "seq", "type": "hello" }` | `{ "seq", "ok", "audio": { simulated, type, name, sampleRate, blockSize, audioCpus, machineCpus }, "probeHz" }` |
| `{ "seq", "type": "apply", "commands": [...] }` | `{ "seq", "ok", "applied", "failedAt"?, "error"?, "applyMs", "rebuildMs", "clips" }` |
| `{ "seq", "type": "readback" }` | `{ "seq", "ok", "doc": <document> }` |
| `{ "seq", "type": "transport", "play"?, "position"?, "loop"?: [start, end] or null }` | `{ "seq", "ok" }` |
| `{ "seq", "type": "arm" }` | `{ "seq", "ok" }`: resets the click detector and the simulated sound card's block timings |
| `{ "seq", "type": "stats" }` | `{ "seq", "ok", settled, settleMs, playing, audibleSec, unexpectedStops, xruns, cpu, maxBlockMs, lateBlocks, detector: { armed, seconds, probeClicks, probeGaps, maxProbeStep, clipEdges, eventEditTimes } }` |
| `{ "seq", "type": "record", "seconds" }` | `{ "seq", "ok", "seconds" }`: from now on, records the output in the order it goes to the device, for up to `seconds` (1 to 600) |
| `{ "seq", "type": "recording", "file"? }` | `{ "seq", "ok", "sampleRate", "analysis": { seconds, windows, windowSeconds, probeDbfs, probeLowestDbfs, probeLowestAt, dipWindows, dipDb, leftResidualMaxDbfs, leftResidualMaxAt, crosstalkDb, crosstalkAt, leftPeakDbfs, rightPeakDbfs, samplesOverFullScale }, "file"?, "error"? }`: measures what was recorded (`src/OutputAnalysis.h`), and writes it as a 32-bit float WAV to `file`, an absolute path, if given. Levels of silence are `null` |
| `{ "seq", "type": "quit" }` | `{ "seq", "ok" }`, then the process exits |

`applyMs` is the time to change Tracktion's edit; `rebuildMs` is the time Tracktion then takes to rebuild its playback graph, which the engine forces straight after the batch so it can be measured. Both are on the engine's message thread.

## Commands

Times are seconds on the edit's timeline. Ids are the model's. A batch is carried out in order and **stops at the first command that cannot be carried out**; the ones before it stay done, and the reply gives `failedAt` and `error`. The TypeScript side then reads back and rebuilds.

| Command | Fails when |
| --- | --- |
| `{ "op": "track.add", "id", "after": id or null }`: a new track straight after `after`; `null` means first | the id exists, or `after` is not a track |
| `{ "op": "track.move", "id", "after": id or null }` | no such track, `after` is not a track, or is the track itself |
| `{ "op": "track.remove", "id" }`: the track and every clip on it | no such track |
| `{ "op": "clip.add", "id", "trackId", "start", "length", "sampleHash", "file" }` | the id exists, no such track, `start < 0`, `length <= 0`, or the file cannot be read |
| `{ "op": "clip.set", "id", "trackId"?, "start"?, "length"? }`: only the fields given. The content moves with the clip; its offset into the file stays | no such clip or track, nothing given, or an invalid value |
| `{ "op": "clip.remove", "id" }` | no such clip |
| `{ "op": "reset" }`: removes every track the model made | never |

`file` is an absolute path on this machine, in the platform's own form: backslashes on Windows. The engine reports it as the operating system writes it, so a path sent with forward slashes on Windows reads back different and looks like drift (the Windows CI run caught this). Node's `path.resolve` gives the right form. It goes from the TypeScript side to the engine on the same machine and nowhere else: never into the session, never over the network (AGENTS.md). `sampleHash` is the content identity the session uses (ADR-0008); the engine only stores it so the readback can report it.

The engine's first track, the probe, is not the model's. It never appears in a readback and no command can touch it.

## Readback

```json
{
  "tracks": [
    { "id": "t1", "clips": [ { "id": "a", "start": 1, "length": 1, "offset": 0, "sampleHash": "…", "file": "/…/a.wav" } ] }
  ]
}
```

Tracks in the order the edit holds them, after the probe. Clips sorted by id. Every value is read from Tracktion's own objects, not from anything the engine side remembers, so a readback that matches the projection means Tracktion really holds it. Times compare equal within a microsecond.

## Examples

Each file in `examples` holds `description`, `commands`, the `readback` expected after running them on an empty engine, and `failedAt` when a command is expected to fail. `${samples}/` stands for a folder that holds `a.wav`, `b.wav` and `c.wav`, with the platform's separator; each test substitutes its own.
