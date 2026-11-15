import { describe, expect, it } from 'vitest';
import { judge, looseName, normaliseVersion, releaseVersion } from '../src/identity.js';

describe('normaliseVersion', () => {
  it.each([
    ['1.2.3', '1.2.3'],
    ['1.2.3.0', '1.2.3'],
    ['1.2.0.0', '1.2'],
    ['v1.02', '1.2'],
    [' 2.0 ', '2'],
    ['0', '0'],
    ['1.2 beta', '1.2 beta'],
    ['1.2.3 (Build 45)', '1.2.3 (build 45)'],
    ['', ''],
  ])('%j becomes %j', (input, expected) => {
    expect(normaliseVersion(input)).toBe(expected);
  });
});

describe('judge: the starting rule from issue #14', () => {
  it('same version: identical', () => {
    expect(judge({ version: '1.2.3' }, { version: '1.2.3' })).toEqual({ verdict: 'identical', usable: true, flagged: false });
  });

  it('versions that differ only in form count as the same', () => {
    expect(judge({ version: '1.2.3' }, { version: '1.2.3.0' })).toEqual({ verdict: 'equivalent-version', usable: true, flagged: false });
  });

  it('different versions: usable but flagged (PLG-7)', () => {
    expect(judge({ version: '1.2.3' }, { version: '1.2.4' })).toEqual({ verdict: 'version-differs', usable: true, flagged: true });
  });

  it('different versions under the block policy: not usable', () => {
    expect(judge({ version: '1.2.3' }, { version: '1.2.4' }, 'block')).toEqual({ verdict: 'version-differs', usable: false, flagged: true });
  });

  it('no version on either side counts as identical; the comparison reports it separately', () => {
    expect(judge({ version: '' }, { version: '' }).verdict).toBe('identical');
  });
});

describe('releaseVersion', () => {
  it('prefers Info.plist, then the Windows product version, then moduleinfo.json', () => {
    const mi = { version: '3.0', factoryVendor: '', classes: [], compatibility: [] };
    expect(releaseVersion({ binaryVersion: { shortVersion: '1.0' }, moduleInfo: mi })).toBe('1.0');
    expect(releaseVersion({ binaryVersion: { productVersion: '2.0.0.0' }, moduleInfo: mi })).toBe('2.0.0.0');
    expect(releaseVersion({ binaryVersion: { productVersion: '0.0.0.0' }, moduleInfo: mi })).toBe('3.0');
    expect(releaseVersion({ binaryVersion: null, moduleInfo: null })).toBeNull();
  });
});

describe('looseName', () => {
  it('ignores case, spacing, punctuation and Unicode normal form', () => {
    expect(looseName('Xfer Records', 'Serum 2')).toBe(looseName('xfer records', 'Serum2'));
    expect(looseName('Véndor', 'X')).toBe(looseName('Véndor', 'X'));
    expect(looseName('A', 'Serum')).not.toBe(looseName('A', 'Serum 2'));
  });
});
