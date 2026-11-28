// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Migrations from older format versions to newer ones.
 *
 * When the format changes, raise `PROJECT_VERSION` in `format.ts`, change the
 * writer and the checks in `load.ts` to the new shape, and add a migration here
 * that turns a document of the old version into one of the new. Each migration
 * goes exactly one version up and sets `version` itself; `loadProject` runs
 * them in order, so a file from any older version reaches the current one. Then
 * add the old version's example file to `test/files/` and describe the change
 * in `docs/project-format.md`.
 *
 * A migration works on the parsed JSON, never on the model, so it keeps working
 * however much the model changes later. It must be pure and must not drop what
 * a person made: anything it cannot express in the new version is a reason not
 * to change the format that way.
 */

/** Turns a parsed document of version `from` into one of version `from + 1`. */
export interface Migration {
  readonly from: number;
  readonly migrate: (file: Readonly<Record<string, unknown>>) => Readonly<Record<string, unknown>>;
}

/** Version 1 is the first, so there is nothing to migrate yet. */
export const MIGRATIONS: readonly Migration[] = Object.freeze([]);
