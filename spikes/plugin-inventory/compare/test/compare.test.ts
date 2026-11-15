import fc from 'fast-check';
import { describe, expect, it } from 'vitest';
import { compareInventories, type Report, type SurpriseKind } from '../src/compare.js';
import { readInventory } from '../src/inventory.js';
import { renderMarkdown } from '../src/report.js';
import { bundleFor, cid, failedBundle, inventory, moduleInfoText, type PluginSpec } from './helpers.js';

function compare(a: PluginSpec[], b: PluginSpec[], extraA = [] as ReturnType<typeof failedBundle>[], extraB = [] as ReturnType<typeof failedBundle>[]): Report {
  return compareInventories(
    readInventory(inventory('A', [...a.map(bundleFor), ...extraA], 'windows')),
    readInventory(inventory('B', [...b.map(bundleFor), ...extraB], 'macos')),
  );
}

const kinds = (r: Report) => r.surprises.map((s) => s.kind).sort();

describe('sorting plugins into shared and not', () => {
  it('matches by class ID alone', () => {
    const r = compare(
      [{ cid: cid(1), name: 'Alpha' }, { cid: cid(2), name: 'Beta' }],
      [{ cid: cid(2), name: 'Beta' }, { cid: cid(3), name: 'Gamma' }],
    );
    expect(r.shared.map((s) => s.a.name)).toEqual(['Beta']);
    expect(r.onlyA.map((p) => p.name)).toEqual(['Alpha']);
    expect(r.onlyB.map((p) => p.name)).toEqual(['Gamma']);
    expect(r.surprises).toEqual([]);
  });

  it('flags a version mismatch and still counts it usable', () => {
    const r = compare([{ cid: cid(1), name: 'Synth', version: '2.0.1' }], [{ cid: cid(1), name: 'Synth', version: '2.0.0' }]);
    expect(r.shared[0]).toMatchObject({ verdict: 'version-differs', usable: true, flagged: true });
  });
});

describe('surprises', () => {
  const cases: [string, () => Report, SurpriseKind[]][] = [
    [
      'same vendor and name, different class IDs',
      () => compare([{ cid: cid(1), name: 'Serum 2', vendor: 'Xfer Records' }], [{ cid: cid(9), name: 'Serum2', vendor: 'Xfer records' }]),
      ['same-name-different-id'],
    ],
    [
      'same class ID, renamed',
      () => compare([{ cid: cid(1), name: 'Pro-Q 3' }], [{ cid: cid(1), name: 'Pro-Q3' }]),
      ['same-id-different-name'],
    ],
    [
      'version text differs only in form',
      () => compare([{ cid: cid(1), name: 'X', version: '1.5' }], [{ cid: cid(1), name: 'X', version: '1.5.0.0' }]),
      ['version-text-differs'],
    ],
    [
      'same reported version, different releases',
      () =>
        compare(
          [{ cid: cid(1), name: 'X', version: '1.0', binaryVersion: { productVersion: '1.0.0.0' } }],
          [{ cid: cid(1), name: 'X', version: '1.0', binaryVersion: { shortVersion: '1.1' } }],
        ),
      // B's factory says 1.0 but its bundle 1.1: also a disagreement on B alone.
      ['factory-and-release-disagree', 'same-version-different-release'],
    ],
    [
      'different reported versions, same release',
      () =>
        compare(
          [{ cid: cid(1), name: 'X', version: '3.2.1', binaryVersion: { productVersion: '3.2.1.0' } }],
          [{ cid: cid(1), name: 'X', version: '3.2.0', binaryVersion: { shortVersion: '3.2.1' } }],
        ),
      ['different-version-same-release', 'factory-and-release-disagree'],
    ],
    [
      'moduleinfo.json with a different class ID than the factory',
      () =>
        compare(
          [{ cid: cid(1), name: 'X', moduleInfo: moduleInfoText({ version: '1.0.0', classes: [{ cid: cid(7), name: 'X', version: '1.0.0' }] }) }],
          [],
        ),
      ['moduleinfo-disagrees'],
    ],
    [
      'the same class ID in two bundles on one machine',
      () => compare([{ cid: cid(1), name: 'X', bundle: 'X.vst3' }, { cid: cid(1), name: 'X', bundle: 'Old/X.vst3' }], []),
      ['duplicate-id'],
    ],
    [
      'a plugin that declares it replaces the other machine’s',
      () =>
        compare(
          [{ cid: cid(2), name: 'Synth 2', moduleInfo: moduleInfoText({ version: '1.0.0', classes: [{ cid: cid(2), name: 'Synth 2', version: '1.0.0' }], replaces: [{ replacement: cid(2), old: [cid(1)] }] }) }],
          [{ cid: cid(1), name: 'Synth' }],
        ),
      ['replaces-other'],
    ],
    ['no version', () => compare([{ cid: cid(1), name: 'X', version: '' }], []), ['no-version']],
    ['a crash while unloading', () => compare([{ cid: cid(1), name: 'X', ending: 'crashed' }], []), ['unload-problem']],
    [
      'a bundle that failed on one machine and works on the other, matched by moduleinfo.json',
      () =>
        compare(
          [],
          [{ cid: cid(1), name: 'X' }],
          [failedBundle('Renamed.vst3', 'timed-out', moduleInfoText({ version: '1.0.0', classes: [{ cid: cid(1), name: 'X', version: '1.0.0' }] }))],
        ),
      ['failed-here-works-there'],
    ],
    [
      'a bundle that failed on one machine and works on the other, matched by file name',
      () => compare([{ cid: cid(1), name: 'X', bundle: 'Vendor/X.vst3' }], [], [], [failedBundle('X.vst3', 'crashed')]),
      ['failed-here-works-there'],
    ],
  ];

  it.each(cases)('%s', (_name, run, expected) => {
    expect(kinds(run())).toEqual([...expected].sort());
  });
});

describe('the report', () => {
  it('escapes plugin names in tables', () => {
    const md = renderMarkdown(compare([{ cid: cid(1), name: 'A|B\nC' }], []));
    expect(md).toContain('A\\|B C');
  });

  it('reports the block policy when asked', () => {
    const a = readInventory(inventory('A', [bundleFor({ cid: cid(1), name: 'X', version: '1' })]));
    const b = readInventory(inventory('B', [bundleFor({ cid: cid(1), name: 'X', version: '2' })]));
    const r = compareInventories(a, b, 'block');
    expect(r.shared[0]?.usable).toBe(false);
    expect(renderMarkdown(r)).toContain('blocked');
  });
});

// ---------------------------------------------------------------------------
// Properties over random pairs of inventories.
// ---------------------------------------------------------------------------

const scale = Number(process.env['FC_SCALE'] ?? '1');

const pluginArb = fc.record({
  id: fc.integer({ min: 1, max: 40 }),
  name: fc.constantFrom('Alpha', 'Beta', 'Gamma', 'Delta', 'Serum 2', 'Pro-Q 3'),
  vendor: fc.constantFrom('Vendor', 'Other Vendor'),
  version: fc.constantFrom('', '1', '1.0', '1.0.0', '1.1', '2.0.0', 'v2'),
  release: fc.option(fc.constantFrom('1.0', '1.1', '2.0'), { nil: undefined }),
});
const sideArb = fc.array(pluginArb, { maxLength: 25 });

type RandomPlugin = { id: number; name: string; vendor: string; version: string; release: string | undefined };

function toInventory(label: string, list: RandomPlugin[]) {
  return readInventory(
    inventory(
      label,
      list.map((p, i) =>
        bundleFor({
          cid: cid(p.id),
          name: p.name,
          vendor: p.vendor,
          version: p.version,
          bundle: `b${i}.vst3`,
          binaryVersion: p.release === undefined ? null : { shortVersion: p.release },
        }),
      ),
    ),
  );
}

describe('properties', () => {
  it('every class ID lands in exactly one of shared, only A and only B', () => {
    fc.assert(
      fc.property(sideArb, sideArb, (la, lb) => {
        const r = compareInventories(toInventory('A', la), toInventory('B', lb));
        const ids = [...r.shared.map((s) => s.cid), ...r.onlyA.map((p) => p.cid), ...r.onlyB.map((p) => p.cid)];
        expect(new Set(ids).size).toBe(ids.length);
        expect(new Set(ids)).toEqual(new Set([...la, ...lb].map((p) => cid(p.id))));
      }),
      { numRuns: 200 * scale },
    );
  });

  it('swapping the machines swaps the sides and changes nothing else', () => {
    fc.assert(
      fc.property(sideArb, sideArb, (la, lb) => {
        const ab = compareInventories(toInventory('A', la), toInventory('B', lb));
        const ba = compareInventories(toInventory('A', lb), toInventory('B', la));
        expect(ba.onlyA.map((p) => p.cid)).toEqual(ab.onlyB.map((p) => p.cid));
        expect(ba.onlyB.map((p) => p.cid)).toEqual(ab.onlyA.map((p) => p.cid));
        const verdicts = (r: Report) => new Map(r.shared.map((s) => [s.cid, `${s.verdict}/${s.usable}/${s.flagged}`]));
        expect(verdicts(ba)).toEqual(verdicts(ab));
        const surpriseKinds = (r: Report) => r.surprises.map((s) => s.kind).sort();
        expect(surpriseKinds(ba)).toEqual(surpriseKinds(ab));
      }),
      { numRuns: 200 * scale },
    );
  });

  it('an inventory compared with itself shares everything, all identical', () => {
    fc.assert(
      fc.property(sideArb, (l) => {
        const r = compareInventories(toInventory('A', l), toInventory('B', l));
        expect(r.onlyA).toEqual([]);
        expect(r.onlyB).toEqual([]);
        expect(r.shared.every((s) => s.verdict === 'identical' && s.usable && !s.flagged)).toBe(true);
        const crossMachine: SurpriseKind[] = ['same-name-different-id', 'same-id-different-name', 'version-text-differs', 'same-version-different-release', 'different-version-same-release', 'replaces-other'];
        expect(r.surprises.filter((s) => crossMachine.includes(s.kind) && s.side === 'both')).toEqual(
          // A duplicate class ID compares its first bundle on each side; with the same list those are the same plugin.
          [],
        );
      }),
      { numRuns: 200 * scale },
    );
  });

  it('under the block policy, exactly the flagged ones are unusable', () => {
    fc.assert(
      fc.property(sideArb, sideArb, (la, lb) => {
        const r = compareInventories(toInventory('A', la), toInventory('B', lb), 'block');
        for (const s of r.shared) expect(s.usable).toBe(!s.flagged);
      }),
      { numRuns: 100 * scale },
    );
  });
});
