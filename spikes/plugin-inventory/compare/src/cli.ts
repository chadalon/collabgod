// npm run inventory -- compare A.json B.json [--block-version-mismatch] [--out report.md] [--json report.json]
// npm run inventory -- summary A.json
//
// Spike code (issue #14). See ../README.md.

import { readFile, writeFile } from 'node:fs/promises';
import { parseArgs } from 'node:util';
import { compareInventories, singleSideSurprises } from './compare.js';
import { readInventory, type ReadInventory } from './inventory.js';
import { renderMarkdown } from './report.js';

const usage = `usage:
  npm run inventory -- compare A.json B.json [--block-version-mismatch] [--out report.md] [--json report.json]
  npm run inventory -- summary A.json`;

async function load(path: string): Promise<ReadInventory> {
  return readInventory(JSON.parse(await readFile(path, 'utf8')) as unknown);
}

async function main(argv: string[]): Promise<number> {
  const { positionals, values } = parseArgs({
    args: argv,
    allowPositionals: true,
    options: {
      out: { type: 'string' },
      json: { type: 'string' },
      'block-version-mismatch': { type: 'boolean', default: false },
    },
  });
  const [command, ...files] = positionals;

  if (command === 'compare' && files.length === 2) {
    const [a, b] = await Promise.all(files.map(load));
    const report = compareInventories(a!, b!, values['block-version-mismatch'] ? 'block' : 'flag');
    const md = renderMarkdown(report);
    if (values.out) await writeFile(values.out, md + '\n');
    else process.stdout.write(md + '\n');
    if (values.json) await writeFile(values.json, JSON.stringify(report, null, 2) + '\n');
    return 0;
  }

  if (command === 'summary' && files.length === 1) {
    const r = await load(files[0]!);
    const i = r.inventory;
    const out = process.stdout;
    out.write(`${i.label || '(no label)'}: ${i.machine.os} ${i.machine.osVersion} ${i.machine.arch}, scanned ${i.scannedAt}\n`);
    out.write(`${i.bundles.length} bundles, ${r.plugins.length} plugins, ${r.failures.length} failed, ${(i.durationMs / 1000).toFixed(1)} s\n`);
    for (const f of r.failures) out.write(`  failed: ${f.bundle}: ${f.reason}\n`);
    for (const s of singleSideSurprises(r, 'a')) out.write(`  ${s.kind}: ${s.message}\n`);
    for (const n of r.notes) out.write(`  note: ${n}\n`);
    for (const p of i.problems) out.write(`  problem: ${p}\n`);
    return 0;
  }

  process.stderr.write(usage + '\n');
  return 64;
}

main(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (e: unknown) => {
    process.stderr.write(`${e instanceof Error ? e.message : String(e)}\n`);
    process.exit(1);
  },
);
