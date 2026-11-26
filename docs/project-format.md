# The project file

- **Format version:** 1
- **Issue:** [#25](https://github.com/chadalon/collabgod/issues/25) (DAW-6, LOB-7, SYN-9)
- **Code:** [`packages/project`](../packages/project), which writes and reads it, and [`app/src/ProjectFile.cpp`](../app/src/ProjectFile.cpp), which puts it on disk
- **Decisions:** [ADR-0017](adr/0017-project-file-v1.md), and ADR-0006, ADR-0008 and ADR-0014 before it

A project file is the contract between versions of the app: a project saved today must open in every later version. This page says exactly what a version 1 file holds. If the code and this page disagree, one of them has a bug.

## In short

- One UTF-8 JSON document, with LF line endings and a final newline. The app names it `<name>.collabgod`.
- It starts with `"format": "collabgod.project"` and a whole-number `"version"`. An app opens any version up to its own, migrating older ones; it refuses a newer one and says so.
- It holds the session as the shared order left it: the timeline, every member, track and clip, with the revision and the author of every field and the creator of every object. It holds no log.
- It names each sample by content: its file hash, its audio hash when known, and its last-known file name. **It never holds a path.** Where a sample lives on this machine is kept by this install, outside the project (see [Samples](#samples)).
- It may hold the undo and redo history of the member who saved it.
- It is written so that a crash cannot damage it (see [Writing](#writing)).

## An example

[`packages/project/test/files/v1-small.collabgod`](../packages/project/test/files/v1-small.collabgod) is a whole version 1 file: one member, two tracks, three clips, one of them trimmed, and an undo history. Shortened:

```json
{
  "format": "collabgod.project",
  "version": 1,
  "seq": 11,
  "timeline": {"tempo":92.5,"timeSignature":{"numerator":7,"denominator":8},"rev":{"tempo":"chad:run1:2","timeSignature":"chad:run1:3"},"by":{"tempo":"chad","timeSignature":"chad"}},
  "members": [
    {"id":"chad","name":"Chad","colour":"#ff8800","removed":false,"verifier":{"scheme":"sha256","value":"…64 hex digits…"},"createdBy":"chad","rev":{…},"by":{…}}
  ],
  "tracks": [
    {"id":"t1","name":"Drums","position":"i","createdBy":"chad","rev":{"name":"chad:run1:4","position":"chad:run1:4"},"by":{"name":"chad","position":"chad"}}
  ],
  "clips": [
    {"id":"k3","sample":{"fileHash":"…","audioHash":"…","name":"sub.aif"},"trackId":"t2","start":370,"length":3470,"offset":0.25,"gain":0,"fadeIn":0,"fadeOut":0,"createdBy":"chad","rev":{…},"by":{…}}
  ],
  "history": {
    "member": "chad",
    "undo": [
      {"opId":"chad:run1:9","inverse":{"type":"clip.patch","clipId":"k3","set":{…},"expect":{…},"restore":{…},"restoreBy":{…}}}
    ],
    "redo": []
  }
}
```

Each member, track, clip and history entry is on a line of its own, so a change to one clip is a change to one line and two saves can be compared with an ordinary diff. The same project always gives exactly the same text.

## The fields

Every object holds exactly the fields listed. A file with a field missing, a field the format does not define, or a value out of range is refused as damaged, and nothing of it is loaded. Values follow the same rules as the operations that make them (`packages/session/src/values.ts`), so a file can never hold a state the app could not have reached by editing.

### The top level

| Field | Holds |
| --- | --- |
| `format` | Always `"collabgod.project"`. Anything else is not a project file. |
| `version` | The format version, a whole number. This page describes `1`. |
| `seq` | How many operations of the shared order the state includes, zero or more. Working alone, the next edit is `seq + 1`. |
| `timeline` | The project's one tempo and time signature (DAW-5). |
| `members` | Every member, removed ones included, by id. |
| `tracks` | Every track, in track order. |
| `clips` | Every clip, by id. |
| `history` | Optional. The undo and redo history of the member who saved the file. |

### Revisions, authors and creators: who made what

Every editable field has a **revision**, the id of the operation that last wrote it, and an **author**, the id of the member whose operation that was. They sit beside the values in `rev` and `by`, one entry per editable field. Every member, track and clip also has `createdBy`, the member who added it, which never changes.

- An empty string in `rev` and `by` means no operation has written the field yet, as for the default tempo.
- Operation ids and member ids are opaque strings of 1 to 256 characters. Nothing may read meaning into them (ADR-0006, ADR-0014).
- Undo and rollback put back the revision *and* the author that were there before, so `by` always names who made what is on screen now, and a removed clip that comes back has its creator and authors again.
- This is the whole of the history the file keeps: who added each object and who last changed each field. Not the log of every operation. ADR-0017 has the reasons.

### `timeline`

| Field | Holds |
| --- | --- |
| `tempo` | Quarter notes per minute, 20 to 999, not necessarily whole |
| `timeSignature` | `numerator` 1 to 32, `denominator` 1, 2, 4, 8, 16 or 32 |
| `rev`, `by` | For `tempo` and `timeSignature` |

### `members`

A member is a person in the project, not a connection (ADR-0006). Members are marked as removed and never deleted, so the author of an old edit can always be named.

| Field | Holds |
| --- | --- |
| `id` | Opaque, made by the app |
| `name` | The display name: 1 to 64 characters, not all spaces. Personal data: see the privacy review (#73) |
| `colour` | `#rrggbb`, lowercase |
| `removed` | `true` once the member has been removed |
| `verifier` | What checks the member's secret. Never the secret itself; see below |
| `createdBy` | The member who added this member |
| `rev`, `by` | For `name`, `colour` and `removed` |

`verifier` is an object with two fields. Version 1 knows one scheme:

| `scheme` | `value` |
| --- | --- |
| `sha256` | The SHA-256 of the member's secret, 64 lowercase hex digits. The secret is at least 128 random bits, kept by the member's install and never written to a project |

How a returning member proves the secret is for #36. If it needs another kind of verifier, such as a public key, that is a new scheme and a new format version.

### `tracks`

| Field | Holds |
| --- | --- |
| `id` | Opaque |
| `name` | 0 to 256 characters |
| `position` | The track's position key, a base-36 fraction of up to 1,024 characters. Tracks are ordered by key, then by id (ADR-0014) |
| `createdBy` | The member who added it |
| `rev`, `by` | For `name` and `position` |

### `clips`

Time is in **ticks**, 960 to a quarter note, read against the project's tempo and time signature. A clip's offset into its file is in **seconds of the file**, so a tempo change does not change which audio a clip starts on (ADR-0014).

| Field | Holds |
| --- | --- |
| `id` | Opaque |
| `sample` | The audio it plays, by content. See below. Fixed for the life of the clip |
| `trackId` | The track it is on, which must be in `tracks` |
| `start` | Where it starts on the timeline, in whole ticks, zero or more |
| `length` | How long it is, in whole ticks, more than zero |
| `offset` | Where in its file its audio starts, in seconds, zero or more |
| `gain` | Clip gain in decibels, −96 to +24 |
| `fadeIn`, `fadeOut` | Fade lengths in whole ticks. They may add up to more than `length`; what sounds is `effectiveFades` in `packages/session` |
| `createdBy` | The member who added it. For the right half of a split, the member who split it |
| `rev`, `by` | For every field above except `id` and `sample` |

### Samples

`sample` names the audio by content (ADR-0008), never by where it is:

| Field | Holds |
| --- | --- |
| `fileHash` | SHA-256 of the file's bytes, 64 lowercase hex digits. The sample's identity |
| `audioHash` | Optional. SHA-256 of the decoded PCM audio, for WAV and AIFF, when the indexer computed it |
| `name` | The file's name when the clip was made, for display and for suggestions (SMP-7). A name, never a path: it may not contain `/` or `\` |

**A local path never goes into a project.** The file is meant to be opened on other machines, and where a file lives on one machine is that machine's business (NFR-6).

Instead, each install keeps its own **sample location table**: for each content hash, the paths on this machine that held it when the app last looked, each with the file's size and modification time at that moment. It lives in the app's data folder, outside every project, as `sample-locations.json` (`SampleLocations` in `packages/project`). Opening a project looks each sample up there:

1. A file with the same `fileHash`, if one is listed.
2. Otherwise a file with the same `audioHash`: the same samples with different metadata, which sounds the same and counts as an exact match (ADR-0008).
3. An entry is trusted only while the file at its path still has the size and modification time it had when it was hashed. Otherwise it has changed or gone, and the entry is forgotten.
4. A sample with no trusted entry is **missing**. The clip shows it as missing, and the project opens anyway. That is not an error.

The table is a cache of what the file system says, so an unreadable one is treated as empty. How it grows into the full sample index of Phase 2 is #42.

### `history`

The undo and redo history of the member who saved the file (SYN-5), so undo still works after the project is closed and opened again. Anyone else who opens the file starts with an empty history.

| Field | Holds |
| --- | --- |
| `member` | Whose history it is |
| `undo` | Oldest first: the next undo tries the last entry |
| `redo` | Oldest first, likewise |

Each entry is `{"opId": …, "inverse": …}`: the operation it reverses and the guarded change that reverses it, exactly as `packages/session` made it. An inverse is checked like any other operation when undo applies it, so a stale or damaged one is skipped, never applied wrongly. Only edits the relay had confirmed are saved.

## What is not in the file

- **Paths of any kind**, as above.
- **Edits the relay had not confirmed** when the file was saved. Resending them after a reconnect is Phase 2 (LOB-6).
- **The member's secret, and which member this install is.** Both belong to the install.
- **The log of operations.** See ADR-0017.
- Mixer, plugins, MIDI and automation: later phases, each a new format version.

## Writing

The app writes projects, and the sample location table, with `writeFileAtomically` in `app/src/ProjectFile.cpp`, so that a crash at any moment leaves either the whole old file or the whole new one:

1. The text goes to a new temporary file in the same folder, `.<name>.<random>.tmp`.
2. The temporary file is flushed to the disk: `FlushFileBuffers` on Windows, `fsync` on Linux, `F_FULLFSYNC` on macOS.
3. It is renamed over the project in one step: `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` on Windows, `rename` elsewhere. On Linux and macOS the folder is then flushed as well. On Windows another program reading the project at that moment (a virus scanner, the search indexer) can make the rename fail with a sharing error, so it is tried again for up to a second.
4. If anything fails before the rename, the temporary file is removed and the old project is untouched.

A crash can leave a temporary file behind, which is never read. `CollabGod --project-file-test` checks all of this, including a writer killed at random moments while it saves, twenty times over.

## Reading

`loadProject` in `packages/project` never throws. It gives the project, or one of these reasons, which the interface turns into words for the person:

| Reason | When |
| --- | --- |
| `not-a-project` | Not JSON, or no `"format": "collabgod.project"` |
| `newer-version` | Saved by a newer app. The person needs to update |
| `unsupported-version` | An old version with no migration |
| `damaged` | A field is missing, unknown or out of range, or the objects break a rule, such as a clip on a track that does not exist |

A byte order mark and CRLF line endings are accepted, in case another tool rewrote the file.

## Changing the format

1. Raise `PROJECT_VERSION` in `packages/project/src/format.ts`, and change the writer and the checks in `load.ts` to the new shape.
2. Add a migration from the old version to the new one in `migrate.ts`. It works on the parsed JSON, one version up, and must not drop anything a person made.
3. Keep every old example in `packages/project/test/files/`, and add one for the new version. Every old example must still open.
4. Update this page: the version at the top, the fields, and a line in the history below.
5. If the change has lasting consequences, write an ADR.

When the app opens a file it had to migrate (`loadProject` says so in `migratedFrom`), it must keep the original beside it before saving over it, because an older app cannot open the new version.

## History

| Version | Date | Change |
| --- | --- | --- |
| 1 | 2026-11-26 | The first version (#25) |
