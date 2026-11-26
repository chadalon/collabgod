# `@collabgod/project`

The project file: saving a session to text and opening it again, in a documented, versioned format (DAW-6), and this install's table of where each sample lives. Issue [#25](https://github.com/chadalon/collabgod/issues/25). The format is [`docs/project-format.md`](../../docs/project-format.md); the decisions behind it are [ADR-0017](../../docs/adr/0017-project-file-v1.md).

Pure TypeScript like `@collabgod/session`, which it depends on: no file system, Node or browser APIs, so it runs in the app's page, in the relay and in tests. Reading and writing files is the app's job. The app writes them with `writeFileAtomically` (`app/src/ProjectFile.cpp`), so a crash cannot damage a project.

## Run it

From the repository root:

```sh
npm run typecheck -w @collabgod/project
npm test -w @collabgod/project               # about 40 tests in a few seconds
FC_SCALE=40 npm test -w @collabgod/project   # the property tests with 40 times as many random cases
```

## Using it

```ts
import { loadProject, projectOf, replicaFor, saveProject } from '@collabgod/project';

// Saving: the replica's confirmed state, how far into the shared order it is, and its member's undo history.
const text = saveProject(projectOf(me));
await app.writeProjectFile(path, text); // the app's atomic write

// Opening. Never throws: a file that cannot be read gives a reason.
const result = loadProject(text);
if (!result.ok) return tellThePerson(result.error.kind); // not-a-project, newer-version, unsupported-version, damaged
if (result.migratedFrom !== undefined) keepTheOriginal(path); // an older app cannot open the new version
const me = replicaFor(result.project, { author: memberId, nextOpId: opIdSource(`${memberId}:${randomRunId}:`) });
```

Opening a project never looks for its samples, so a project whose samples are nowhere on this machine still opens. Finding them is the sample location table's job:

```ts
import { SampleLocations } from '@collabgod/project';

const locations = SampleLocations.decode(await app.readDataFile('sample-locations.json'));
locations.record({ path, size, modifiedMs, fileHash, audioHash }); // after hashing a file a person added
const { resolution, forgotten } = await locations.resolve(clip.sample, app.stat);
// resolution: { kind: 'found', path, match: 'file' | 'audio' } or { kind: 'missing' }, which the clip shows
if (forgotten.length > 0) await app.writeDataFile('sample-locations.json', locations.encode());
```

## What is here

| File | What it holds |
| --- | --- |
| `src/format.ts` | The version 1 file: `saveProject`, field by field, one object per line |
| `src/load.ts` | `loadProject`: recognise the file, migrate it, check every field |
| `src/migrate.ts` | Migrations from older versions. Empty: version 1 is the first |
| `src/locations.ts` | `SampleLocations`: content hash to the paths on this machine that hold it |
| `src/index.ts` | `projectOf` and `replicaFor`, between a replica and a project |
| `test/format.test.ts` | What a saved file holds, and reopening it |
| `test/load.test.ts` | Every reason a file is refused, and migrations |
| `test/roundtrip.property.test.ts` | Random lobby and solo sessions: saved, loaded and saved again, the state and the text are the same, and the reopened replica behaves exactly as the original |
| `test/locations.test.ts` | Finding samples by file and audio hash, forgetting changed files, missing samples |
| `test/files.test.ts`, `test/files/` | Example files from each format version, kept for good: each must open in every later version |

The property tests drive the session model's own random runs (`packages/session/test/sim.ts`).

## Rules

- **A local path never goes into a project.** Samples are named by file hash, audio hash and name only, and every field a file holds is listed in `format.ts` and checked in `load.ts`.
- **A file is checked as strictly as an operation off the network.** A field missing, unknown or out of range, and the file is refused; nothing of it is loaded.
- **The same project gives the same text**, so saving twice changes nothing and a diff shows what changed.
- **Changing the format** means a new version, a migration and a new example file. See "Changing the format" in `docs/project-format.md`.

## Not here yet

- The app's Save and Open: the menu, the file dialog and the bridge messages that carry the text between the page and `writeFileAtomically`. The gate of Phase 1 needs them.
- Making a member and its secret when a project is created, and where the install keeps the secret (#36).
- The sample indexer that fills the location table as it scans (#42).
- Saving edits the relay had not confirmed (Phase 2, LOB-6).
