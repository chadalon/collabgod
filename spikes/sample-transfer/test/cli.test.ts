// SPDX-License-Identifier: GPL-3.0-or-later

import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const cli = fileURLToPath(new URL('../src/cli.ts', import.meta.url));

/** Run the CLI. The relay URL points nowhere, so a run that got past option checking would fail to connect instead. */
function run(...args: string[]) {
  const result = spawnSync(process.execPath, ['--import', 'tsx', cli, ...args], { encoding: 'utf8', timeout: 20_000 });
  return { status: result.status, stderr: result.stderr };
}

describe('the CLI refuses options meant for the other side', () => {
  it('refuses --drop-at-mb on share', () => {
    const { status, stderr } = run('share', 'wss://127.0.0.1:9', '--size-mb', '1', '--drop-at-mb', '20');
    expect(status).toBe(1);
    expect(stderr).toContain('--drop-at-mb is an option for fetch, not share.');
  });

  it('names every misplaced option on fetch', () => {
    const { status, stderr } = run('fetch', 'wss://127.0.0.1:9', 'K7M-QX4', '--size-mb', '1', '--window-kib', '512');
    expect(status).toBe(1);
    expect(stderr).toContain('--size-mb, --window-kib are options for share, not fetch.');
  });

  it('accepts options both sides read', () => {
    const { stderr } = run('fetch', 'wss://127.0.0.1:9', 'K7M-QX4', '--where', 'Chapel Hill', '--drop-at-mb', '5');
    expect(stderr).not.toContain('is an option for');
    expect(stderr).not.toContain('are options for');
  });
});
