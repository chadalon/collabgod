// SPDX-License-Identifier: GPL-3.0-or-later

import { type MemberId, type OpId, Replica, Session } from '@collabgod/session';
import type { Project } from './format';

export { PROJECT_EXTENSION, PROJECT_FORMAT, PROJECT_VERSION, type Project, saveProject } from './format';
export { type LoadError, type LoadErrorKind, type LoadOptions, type LoadResult, loadProject } from './load';
export {
  type FileStat,
  LOCATIONS_FORMAT,
  LOCATIONS_VERSION,
  type Resolution,
  type SampleLocation,
  SampleLocations,
  type Stat,
} from './locations';
export { MIGRATIONS, type Migration } from './migrate';

/**
 * What to save for a replica: its confirmed state, how far into the shared order
 * that is, and its member's undo history. Edits the relay has not confirmed yet
 * are not in it. Costs time in proportion to the session's size.
 */
export function projectOf(replica: Replica): Project {
  return { seq: replica.seq, session: replica.confirmedSnapshot(), history: replica.history };
}

/**
 * A replica for `author` on a project just loaded. It takes the saved undo
 * history if the project was saved by the same member, and starts with none
 * otherwise. `nextOpId` must not repeat ids from before the project was saved:
 * give it a prefix new to this run of the app (see `opIdSource`).
 */
export function replicaFor(
  project: Project,
  options: { readonly author: MemberId; readonly nextOpId: () => OpId },
): Replica {
  const history = project.history?.member === options.author ? project.history : undefined;
  return new Replica({
    author: options.author,
    nextOpId: options.nextOpId,
    session: Session.fromSnapshot(project.session),
    seq: project.seq,
    ...(history === undefined ? {} : { history }),
  });
}
