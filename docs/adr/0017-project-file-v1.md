# ADR-0017: The project file keeps who made what as state, not as a log

- **Status:** Proposed
- **Date:** 2026-11-26
- **Deciders:** Chad

## Context

Issue #25 defines the project file, the contract between versions of the app (DAW-6). It inherits questions that earlier records left open:

- **How much history the project keeps** (ADR-0006, "Still to settle"): the whole log of operations, or who created and who last changed each object. Every operation records its author (SYN-9), and who made what must be saved with the project.
- **What checks a member's secret** (ADR-0006): the project holds the member list and "only what is needed to check that secret, never the secret itself". How a member proves it is #36's.
- **Where a sample lives on this machine** (ADR-0008, AGENTS.md): a path never goes into a project, but a saved project has to open again on the machine that saved it.
- **Whether undo survives closing the project.** The session model hands out the undo history as plain data for this (`packages/session`, "Not here yet").

The session model already keeps, for every editable field, the id of the operation that last wrote it (ADR-0004, ADR-0014). Operation ids are opaque, so they do not say who made them.

## Decision

1. **Who made what is part of the session state.** Beside each field's revision the model keeps the member whose operation it was (`by`), and every member, track and clip keeps the member who added it (`createdBy`). Inverses carry and restore both exactly, like the revisions, so undo, rollback and snapshots keep them right. The project file saves the state, and so saves who made what.
2. **The project keeps no log of operations.** Who added each object and who last changed each field is the history it keeps. Chad chose this over the whole log on 2026-11-26.
3. **A member carries a verifier, fixed for the member's life.** It is added with the member, by the `member.add` operation, so every copy of the project can check a returning member, whoever hosts. Version 1 has one scheme, `sha256`: the SHA-256 of a secret of at least 128 random bits, which the member's install keeps.
4. **The saving member's undo history goes in the project**, so undo works after it is opened again. Chad chose this on 2026-11-26. Another member opening the file starts with none.
5. **Where samples live is a table kept by the install**, from content hash to the paths that held it, each checked against the file's size and modification time before it is trusted. It is never part of a project. A sample with no trusted entry is shown as missing; the project opens anyway.
6. **The file is JSON with a format marker and a whole-number version**, one object per line, written field by field so that the same project always gives the same text. Older versions are migrated on load, one version at a time, by migrations that work on the parsed JSON. A newer version is refused with a reason.
7. **Writes are atomic**: a temporary file in the same folder, flushed to the disk, then renamed over the project.

The format itself is documented in [`docs/project-format.md`](../project-format.md).

## Alternatives considered

- **The whole log.** Full history, and a log since a common point could help merge two copies that were edited apart (#40). But the file grows with every edit for as long as the project lives, not with what is in it, loading means replaying or keeping a snapshot beside it anyway, and nothing in v1 reads old operations. #40 can keep a log since the last shared point if it needs one.
- **A table from operation id to author, kept beside the state** (what was first proposed to Chad, with the same result). An inverse can put back a revision whose operation is no longer named anywhere in the state: one member's undo restores another's older edit. A replica built from a snapshot taken in between, as a late joiner's or a reopened project's would be, has no row for that operation, and the author would be lost or the copies would disagree. Keeping the author beside the revision, inside what inverses carry, cannot go wrong that way, and the convergence tests check it with every other field.
- **Who last changed each object, not each field.** Smaller, but an undo of one field's edit would have to put back the object's last author, which is wrong when someone else changed another field in the meantime. Per field it follows the revisions exactly.
- **The verifier only in the project file, outside the session.** Then the copy of whoever hosts would lack the other members' verifiers whenever they joined through someone else. It has to travel with the member, which means in the operation that adds them.
- **Deciding the proof now** (a key pair instead of a hashed token). It is #36's to decide. A hashed random token is the simplest thing that fits "never the secret itself"; a key pair would be a second scheme.
- **Paths in the project, or beside it in the project's folder.** A path is wrong on any other machine and gives away how someone's disk is laid out (NFR-6). A file beside the project would be copied along with it.
- **A binary or compressed format.** Smaller and faster for very large sessions, but no diff, no reading it by eye when something goes wrong, and a 5,000-clip session is a few megabytes of text.

## Consequences

- Every member, track and clip carries one more string per field and one for its creator. The model's timing test (4 ms for a receive on a 5,000-clip session) still passes.
- Every inverse that puts back a field carries its author (`restoreBy`), and every inverse that puts back an object carries its revisions, authors and creator (`restore`). A new kind of operation must do the same, or the inverse property test fails.
- `member.add` needs a verifier, so the app must make a secret and its hash when it makes a member: when a project is created, and when someone joins.
- Who made what is only as trustworthy as the members. The relay stamps each operation's author from the connection it has checked (ADR-0006), but an inverse names the revisions and authors it puts back, and nothing keeps the history to check them against. A member could forge them, as they already could the revisions. That is acceptable between people who invite each other; it is not an audit trail.
- The file cannot answer "what did Sam do yesterday?". Showing who made each edit (#95) can show who last changed each field and who added each object, and nothing older. If people want more, that is the argument for keeping a log.
- A saved undo history holds whole copies of what it would put back, such as a removed clip. That is how the model works; the file grows with the history, which has no limit yet (`packages/session`, "Not here yet").
- Unconfirmed edits are not saved. Saving in the middle of a lobby session saves what the relay had confirmed.
- The project opens even when no sample is on this machine. The interface has to show missing samples on their clips.
- The sample location table is written by the app with the same atomic write, and how it grows into the sample index is #42.
- A project file holds display names, which are personal data (#73).
- Changing the format means a new version, a migration and a new example file, kept for good (`docs/project-format.md`, "Changing the format").

## How we will know

The property tests in `packages/project`: random lobby and solo runs saved, loaded and saved again give the same state, revisions and authors included, and the same text; a replica opened from the file behaves exactly as one given the project without it. The convergence tests in `packages/session` check that replicas agree on who made what, including after one is rebuilt from a snapshot. The Phase 1 gate (place a sample, hear it, save, reopen) is the first real use. If people want to see more history than the last author of each field (#95), or #40 needs a log to merge two copies, revisit decision 2. When #36 settles how a member proves their secret, revisit decision 3.
