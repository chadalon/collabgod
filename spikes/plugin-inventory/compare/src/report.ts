// A comparison as Markdown, to paste into issue #14. Spike code.

import type { Report, SideSummary, Surprise, SurpriseKind } from './compare.js';
import type { Plugin } from './inventory.js';

const kindTitles: Record<SurpriseKind, string> = {
  'same-name-different-id': 'Same name, different class ID (the rule calls these different plugins)',
  'same-id-different-name': 'Same class ID, different name or vendor',
  'version-text-differs': 'Versions equal except for form',
  'same-version-different-release': 'Same reported version, different release version',
  'different-version-same-release': 'Different reported version, same release version',
  'factory-and-release-disagree': "Factory version differs from the bundle's own version",
  'moduleinfo-disagrees': 'moduleinfo.json disagrees with the factory',
  'duplicate-id': 'One class ID in several bundles on one machine',
  'replaces-other': "Declared replacement of the other machine's plugin",
  'no-version': 'No version reported',
  'unload-problem': 'Crashed or hung while unloading',
  'failed-here-works-there': 'Failed on one machine, works on the other',
};

/** Escapes text for a Markdown table cell. Plugin names are not trusted to be tame. */
function cell(s: string): string {
  return s.replace(/\\/g, '\\\\').replace(/\|/g, '\\|').replace(/[\r\n]+/g, ' ');
}

function sideLine(name: string, s: SideSummary): string {
  return `| ${name} | ${cell(s.label)} | ${cell(s.machine)} | ${s.bundles} | ${s.plugins} | ${s.failures.length} | ${s.withModuleInfo} | ${s.scanSeconds} |`;
}

function pluginRow(p: Plugin): string {
  return `| ${cell(p.vendor)} | ${cell(p.name)} | ${cell(p.version || '(none)')} | \`${p.cid}\` |`;
}

export function renderMarkdown(r: Report): string {
  const lines: string[] = [];
  const flagged = r.shared.filter((s) => s.flagged);
  const usable = r.shared.filter((s) => s.usable);

  lines.push('## Plugin inventories compared', '');
  lines.push('| | Label | Machine | Bundles | Plugins | Failed | With moduleinfo.json | Scan (s) |');
  lines.push('| --- | --- | --- | --- | --- | --- | --- | --- |');
  lines.push(sideLine('A', r.a), sideLine('B', r.b), '');

  lines.push(
    `Shared by class ID: **${r.shared.length}**, of which ${r.shared.filter((s) => s.verdict === 'identical').length} identical, ` +
      `${r.shared.filter((s) => s.verdict === 'equivalent-version').length} equal once the version text is normalised, ` +
      `and ${flagged.length} with different versions (${r.policy === 'flag' ? 'usable, flagged' : 'blocked'}). ` +
      `Usable in a lobby under this policy: **${usable.length}**. Only on A: ${r.onlyA.length}. Only on B: ${r.onlyB.length}.`,
    '',
  );

  if (flagged.length > 0) {
    lines.push('### Version mismatches', '', '| Vendor | Name | A | B | Class ID |', '| --- | --- | --- | --- | --- |');
    for (const s of flagged) {
      lines.push(`| ${cell(s.a.vendor)} | ${cell(s.a.name)} | ${cell(s.a.version || '(none)')} | ${cell(s.b.version || '(none)')} | \`${s.cid}\` |`);
    }
    lines.push('');
  }

  const groups = new Map<SurpriseKind, Surprise[]>();
  for (const s of r.surprises) groups.set(s.kind, [...(groups.get(s.kind) ?? []), s]);
  lines.push('### Surprises', '');
  if (groups.size === 0) lines.push('None.', '');
  for (const kind of Object.keys(kindTitles) as SurpriseKind[]) {
    const list = groups.get(kind);
    if (!list) continue;
    lines.push(`**${kindTitles[kind]}** (${list.length})`, '');
    for (const s of list) lines.push(`- ${s.side === 'both' ? '' : `${s.side.toUpperCase()}: `}${s.message.replace(/[\r\n]+/g, ' ')}`);
    lines.push('');
  }

  for (const [name, side] of [['A', r.a], ['B', r.b]] as const) {
    if (side.failures.length === 0) continue;
    lines.push(`### Bundles that did not scan on ${name}`, '', '| Bundle | Why |', '| --- | --- |');
    for (const f of side.failures) lines.push(`| ${cell(f.bundle)} | ${cell(f.reason)} |`);
    lines.push('');
  }

  for (const [name, list] of [['A', r.onlyA], ['B', r.onlyB]] as const) {
    if (list.length === 0) continue;
    lines.push(`<details><summary>Only on ${name} (${list.length})</summary>`, '', '| Vendor | Name | Version | Class ID |', '| --- | --- | --- | --- |');
    for (const p of list) lines.push(pluginRow(p));
    lines.push('', '</details>', '');
  }

  if (r.shared.length > 0) {
    lines.push(`<details><summary>Shared (${r.shared.length})</summary>`, '', '| Vendor | Name | Version | Class ID |', '| --- | --- | --- | --- |');
    for (const s of r.shared) lines.push(pluginRow(s.a));
    lines.push('', '</details>', '');
  }

  return lines.join('\n');
}
