// Comparing two machines' inventories. Spike code (issue #14).
//
// Applies the identity rule (identity.ts) to find what the two people share,
// then looks for the cases where the rule and reality might disagree.

import { judge, identityKey, looseName, normaliseVersion, releaseVersion, type Judgement, type VersionPolicy } from './identity.js';
import type { Failure, Plugin, ReadInventory } from './inventory.js';

export type Side = 'a' | 'b';

export interface Shared extends Judgement {
  cid: string;
  a: Plugin;
  b: Plugin;
}

export type SurpriseKind =
  /** Same vendor and name on both machines, different class IDs: the rule says different plugins. */
  | 'same-name-different-id'
  /** Same class ID, but the name or vendor differs between the machines. */
  | 'same-id-different-name'
  /** Same class ID, version texts differ only in form. */
  | 'version-text-differs'
  /** Same reported version, but the bundles' release versions differ: the factory version may be stale. */
  | 'same-version-different-release'
  /** Different reported versions, but the same release version: Windows and Mac builds may report differently. */
  | 'different-version-same-release'
  /** On one machine, the factory's version and the bundle's release version disagree. */
  | 'factory-and-release-disagree'
  /** moduleinfo.json disagrees with what the loaded factory reported. */
  | 'moduleinfo-disagrees'
  /** One class ID in more than one bundle on the same machine. */
  | 'duplicate-id'
  /** A plugin declares, in moduleinfo.json, that it replaces a class ID the other machine has. */
  | 'replaces-other'
  /** The plugin reports no version at all. */
  | 'no-version'
  /** The plugin crashed or hung after it was read, while unloading. */
  | 'unload-problem'
  /** A bundle failed to scan on one machine, and the other machine has it working. */
  | 'failed-here-works-there';

export interface Surprise {
  kind: SurpriseKind;
  side: Side | 'both';
  cid: string | null;
  message: string;
}

export interface SideSummary {
  label: string;
  machine: string;
  bundles: number;
  plugins: number;
  failures: Failure[];
  withModuleInfo: number;
  scanSeconds: number;
}

export interface Report {
  policy: VersionPolicy;
  a: SideSummary;
  b: SideSummary;
  shared: Shared[];
  onlyA: Plugin[];
  onlyB: Plugin[];
  surprises: Surprise[];
}

function byKey(plugins: Plugin[]): Map<string, Plugin[]> {
  const m = new Map<string, Plugin[]>();
  for (const p of plugins) {
    const k = identityKey(p);
    const list = m.get(k);
    if (list) list.push(p);
    else m.set(k, [p]);
  }
  return m;
}

const byName = (x: Plugin, y: Plugin) =>
  x.vendor.localeCompare(y.vendor) || x.name.localeCompare(y.name) || x.cid.localeCompare(y.cid);

function summarise(r: ReadInventory): SideSummary {
  const i = r.inventory;
  return {
    label: i.label || '(no label)',
    machine: `${i.machine.os} ${i.machine.osVersion} ${i.machine.arch}`,
    bundles: i.bundles.length,
    plugins: r.plugins.length,
    failures: r.failures,
    withModuleInfo: i.bundles.filter((b) => b.moduleInfo !== null).length,
    scanSeconds: Math.round(i.durationMs / 100) / 10,
  };
}

const show = (p: Plugin) => `${p.vendor} ${p.name}`.trim();

/** Surprises visible on one machine alone. */
export function singleSideSurprises(r: ReadInventory, side: Side): Surprise[] {
  const out: Surprise[] = [];

  for (const [cid, list] of byKey(r.plugins)) {
    if (list.length > 1) {
      out.push({ kind: 'duplicate-id', side, cid, message: `${show(list[0]!)} is in ${list.length} bundles: ${list.map((p) => p.bundle).join(', ')}` });
    }
  }

  for (const p of r.plugins) {
    if (p.version.trim() === '') {
      out.push({ kind: 'no-version', side, cid: p.cid, message: `${show(p)} (${p.bundle}) reports no version (factory info level ${p.infoLevel})` });
    }
    if (p.unloadProblem) {
      out.push({ kind: 'unload-problem', side, cid: p.cid, message: `${show(p)} (${p.bundle}) ${p.unloadProblem}` });
    }
    const release = releaseVersion(p);
    if (release !== null && p.version !== '' && normaliseVersion(release) !== normaliseVersion(p.version)) {
      out.push({
        kind: 'factory-and-release-disagree',
        side,
        cid: p.cid,
        message: `${show(p)}: the factory says ${p.version}, the bundle says ${release}`,
      });
    }
    if (p.moduleInfo) {
      const mc = p.moduleInfo.classes.find((c) => c.cid === p.cid);
      if (!mc) {
        const sameName = p.moduleInfo.classes.find((c) => c.name === p.name);
        out.push({
          kind: 'moduleinfo-disagrees',
          side,
          cid: p.cid,
          message: sameName
            ? `${show(p)}: moduleinfo.json gives class ID ${sameName.cid}, the factory ${p.cid}`
            : `${show(p)}: class ID ${p.cid} is not in moduleinfo.json`,
        });
      } else if (mc.version !== p.version || mc.name !== p.name) {
        out.push({
          kind: 'moduleinfo-disagrees',
          side,
          cid: p.cid,
          message: `${show(p)}: moduleinfo.json says "${mc.name}" ${mc.version}, the factory "${p.name}" ${p.version}`,
        });
      }
    }
  }
  return out;
}

export function compareInventories(a: ReadInventory, b: ReadInventory, policy: VersionPolicy = 'flag'): Report {
  const ka = byKey(a.plugins);
  const kb = byKey(b.plugins);

  const shared: Shared[] = [];
  const onlyA: Plugin[] = [];
  const onlyB: Plugin[] = [];
  const surprises: Surprise[] = [...singleSideSurprises(a, 'a'), ...singleSideSurprises(b, 'b')];

  // Duplicates on one machine are reported above; compare the first of each.
  for (const [k, listA] of ka) {
    const pa = listA[0]!;
    const listB = kb.get(k);
    if (!listB) {
      onlyA.push(pa);
      continue;
    }
    const pb = listB[0]!;
    shared.push({ cid: k, a: pa, b: pb, ...judge(pa, pb, policy) });
  }
  for (const [k, listB] of kb) if (!ka.has(k)) onlyB.push(listB[0]!);

  for (const s of shared) {
    const { a: pa, b: pb } = s;
    if (pa.name !== pb.name || pa.vendor !== pb.vendor) {
      surprises.push({ kind: 'same-id-different-name', side: 'both', cid: s.cid, message: `${s.cid} is "${show(pa)}" on A and "${show(pb)}" on B` });
    }
    if (s.verdict === 'equivalent-version') {
      surprises.push({ kind: 'version-text-differs', side: 'both', cid: s.cid, message: `${show(pa)}: version "${pa.version}" on A, "${pb.version}" on B` });
    }
    const ra = releaseVersion(pa);
    const rb = releaseVersion(pb);
    if (ra !== null && rb !== null) {
      const sameRelease = normaliseVersion(ra) === normaliseVersion(rb);
      if (s.verdict !== 'version-differs' && !sameRelease) {
        surprises.push({
          kind: 'same-version-different-release',
          side: 'both',
          cid: s.cid,
          message: `${show(pa)} reports ${pa.version} on both, but the bundles say ${ra} on A and ${rb} on B`,
        });
      } else if (s.verdict === 'version-differs' && sameRelease) {
        surprises.push({
          kind: 'different-version-same-release',
          side: 'both',
          cid: s.cid,
          message: `${show(pa)} reports ${pa.version} on A and ${pb.version} on B, but both bundles say ${ra}`,
        });
      }
    }
  }

  // The same plugin under two IDs, as far as names can tell.
  const looseB = new Map<string, Plugin[]>();
  for (const p of onlyB) {
    const k = looseName(p.vendor, p.name);
    looseB.set(k, [...(looseB.get(k) ?? []), p]);
  }
  for (const pa of onlyA) {
    for (const pb of looseB.get(looseName(pa.vendor, pa.name)) ?? []) {
      surprises.push({
        kind: 'same-name-different-id',
        side: 'both',
        cid: pa.cid,
        message: `"${show(pa)}" is ${pa.cid} on A and ${pb.cid} on B`,
      });
    }
  }

  // Declared replacements across the two machines.
  const replacements = (from: Plugin[], to: Map<string, Plugin[]>, side: Side) => {
    for (const p of from) {
      for (const c of p.moduleInfo?.compatibility ?? []) {
        if (c.replacement !== p.cid) continue;
        for (const old of c.replaces) {
          const there = to.get(old)?.[0];
          if (there && there.cid !== p.cid) {
            surprises.push({
              kind: 'replaces-other',
              side,
              cid: p.cid,
              message: `${show(p)} on ${side.toUpperCase()} declares it replaces ${old}, which the other machine has as ${show(there)}`,
            });
          }
        }
      }
    }
  };
  replacements(a.plugins, kb, 'a');
  replacements(b.plugins, ka, 'b');

  // A bundle that failed here but works there, matched by moduleinfo class ID or bundle file name.
  const failedButPresent = (failures: Failure[], otherPlugins: Plugin[], other: Map<string, Plugin[]>, side: Side) => {
    const otherByFile = new Map(otherPlugins.map((p) => [baseName(p.bundle), p]));
    for (const f of failures) {
      const viaInfo = f.moduleInfo?.classes.map((c) => other.get(c.cid)?.[0]).find((p) => p !== undefined);
      const there = viaInfo ?? otherByFile.get(baseName(f.bundle));
      if (there) {
        surprises.push({
          kind: 'failed-here-works-there',
          side,
          cid: there.cid,
          message: `${f.bundle} failed on ${side.toUpperCase()} (${f.reason}); the other machine has ${show(there)} working`,
        });
      }
    }
  };
  failedButPresent(a.failures, b.plugins, kb, 'a');
  failedButPresent(b.failures, a.plugins, ka, 'b');

  shared.sort((x, y) => byName(x.a, y.a));
  onlyA.sort(byName);
  onlyB.sort(byName);

  return { policy, a: summarise(a), b: summarise(b), shared, onlyA, onlyB, surprises };
}

function baseName(path: string): string {
  return (path.split('/').pop() ?? path).toLowerCase();
}
