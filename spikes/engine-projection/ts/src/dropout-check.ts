// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The dropout check (issue #11: "edits during playback do not cause a dropout").
 *
 *   npm run dropout-check -- --bin <path to engine_projection> [options]
 *
 * Plays a session through the real audio device while the session model is edited
 * many times a second, and the projection keeps the engine in step. Three phases:
 *
 *   1. quiet   10 s, no edits: the baseline for this machine
 *   2. edits   30 s of random model edits (moves, resizes, adds, removes, undos, tracks),
 *              half of them right at the playhead; halfway through, the engine is emptied
 *              and rebuilt from the model while it plays
 *   3. after    5 s, no edits
 *
 * What to listen for: the left channel is a steady 440 Hz tone that no edit touches.
 * Any click, gap or crackle in it is a dropout. The right channel is the session's clips,
 * jumping about as they are edited; abrupt starts and stops there are expected.
 *
 * What it measures, per phase: the device's dropout counter, the click detector on the
 * left channel (a plugin on the master bus that counts clicks and gaps on the audio
 * thread), and transport stops nobody asked for (#101). At the end it checks the engine
 * still matches the model. A report is saved as JSON in the current folder.
 *
 * Options:
 *   --bin <path>            engine_projection (or set ENGINE_PROJECTION_BIN)
 *   --seconds <n>           length of the edits phase (default 30)
 *   --rate <n>              edits a second (default 10)
 *   --tracks <n>            tracks to start with (default 8)
 *   --clips <n>             clips to start with (default 48)
 *   --samples <folder>      use the .wav/.aif files in this folder instead of generated tones
 *   --simulated-audio       no device: a simulated sound card (for trying it without speakers)
 *   --tracktion-wave-nodes  play clips the way Tracktion does by default (finding 4), to compare
 *   --audio-cpus <n>        CPUs Tracktion may use for audio (default: all)
 *   --seed <n>              random seed (default 11)
 */

import { writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { emptyState, type SessionState } from '../../../session-model/src/model';
import { defaultTimebase } from './document';
import { RandomEditor, seededRandom } from './edits';
import { EngineProcess, type EngineStats } from './engine';
import { Projector } from './projector';
import { folderLibrary, generatedLibrary } from './samples';

const args = process.argv.slice(2);
const flag = (name: string): boolean => args.includes(name);
const option = (name: string, fallback?: string): string | undefined => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const number = (name: string, fallback: number): number => Number(option(name, String(fallback)));

const binary = option('--bin', process.env['ENGINE_PROJECTION_BIN']);
if (!binary) {
  console.error('Give the engine binary with --bin <path> (or ENGINE_PROJECTION_BIN). See the README.');
  process.exit(2);
}

const editSeconds = number('--seconds', 30);
const rate = number('--rate', 10);
const loopSeconds = 8;
const tps = defaultTimebase.ticksPerSecond;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const percentile = (values: number[], p: number): number => {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.floor((sorted.length * p) / 100))] ?? 0;
};

interface PhaseResult {
  name: string;
  seconds: number;
  edits: number;
  xruns: number | null;
  probeClicks: number;
  probeGaps: number;
  maxProbeStep: number;
  unexpectedStops: number;
  lateBlocks: number | null;
  maxBlockMs: number | null;
  eventEditTimes: readonly number[];
}

async function main(): Promise<void> {
  const library = option('--samples') ? folderLibrary(option('--samples') as string) : generatedLibrary(join(tmpdir(), 'collabgod-dropout-check'));
  const extraArgs = [
    ...(flag('--tracktion-wave-nodes') ? ['--tracktion-wave-nodes'] : []),
    ...(option('--audio-cpus') ? ['--audio-cpus', option('--audio-cpus') as string] : []),
  ];
  const log: string[] = [];
  const engine = new EngineProcess(binary as string, {
    simulatedAudio: flag('--simulated-audio'),
    extraArgs,
    onLog: (line) => {
      log.push(line);
      if (/stopped without being asked|Device lists changed|settled/.test(line)) console.log(`  [engine] ${line}`);
    },
  });

  const audio = await engine.hello();
  console.log(`Audio: ${audio.type} / ${audio.name}, ${audio.sampleRate} Hz, ${audio.blockSize} samples per block, ${audio.audioCpus} of ${audio.machineCpus} CPUs for audio`);
  console.log(`Samples: ${library.hashes.length} ${option('--samples') ? `from ${option('--samples')}` : 'generated tones'}`);
  const settled = await engine.waitUntilSettled();
  console.log(`Engine settled after ${Math.round(settled.settleMs)} ms`);

  // The session: built through the model, like everything after it.
  let state: SessionState = emptyState;
  let playheadSec = 0;
  const editor = new RandomEditor(seededRandom(number('--seed', 11)), {
    sampleHashes: library.hashes,
    spanTicks: loopSeconds * tps,
    maxTracks: number('--tracks', 8) + 4,
    playheadTicks: () => Math.round(playheadSec * tps),
  });
  const projector = new Projector(engine, (hash) => library.files.get(hash));

  for (let t = 0; t < number('--tracks', 8); t++) {
    state = editor.applyEdit(state, { op: { type: 'track.add', track: { id: `start-t${t}`, name: `t${t}` }, index: t }, description: '' }).state;
  }
  while (Object.keys(state.clips).length < number('--clips', 48)) {
    const edit = editor.next(state);
    if (edit.op.type === 'clip.add') state = editor.applyEdit(state, edit).state;
  }
  const initial = await projector.update(state);
  console.log(`Session: ${state.trackOrder.length} tracks, ${Object.keys(state.clips).length} clips, sent as ${initial.commands} commands`);

  await engine.transport({ loop: [0, loopSeconds], position: 0, play: true });
  while (!(await engine.stats()).playing) await sleep(50);
  console.log('Playing an 8 s loop. Left: a steady 440 Hz tone that must stay clean. Right: the clips.\n');

  const results: PhaseResult[] = [];
  const updateMs: number[] = [];
  const applyMs: number[] = [];
  const rebuildMs: number[] = [];
  let rejected = 0;
  let projectorRebuilds = 0;

  const phase = async (name: string, seconds: number, editing: boolean): Promise<void> => {
    await engine.arm();
    await sleep(100);
    const start: EngineStats = await engine.stats();
    const began = Date.now();
    let edits = 0;
    let nextStatus = began + 1000;
    let rebuiltMidway = false;
    console.log(`== ${name}: ${seconds} s${editing ? `, ${rate} edits a second` : ', no edits'}`);

    while (Date.now() - began < seconds * 1000) {
      if (editing) {
        if (!rebuiltMidway && Date.now() - began >= (seconds * 1000) / 2) {
          rebuiltMidway = true;
          console.log('   -> now emptying the engine and rebuilding it from the model while it plays. Listen.');
          const t0 = performance.now();
          const commands = await projector.rebuildFromScratch();
          console.log(`   -> rebuilt with ${commands} commands in ${(performance.now() - t0).toFixed(1)} ms`);
        }
        const edit = editor.next(state);
        const result = editor.applyEdit(state, edit);
        if (result.applied) {
          state = result.state;
          const t0 = performance.now();
          const update = await projector.update(state);
          updateMs.push(performance.now() - t0);
          if (update.applyMs !== undefined) applyMs.push(update.applyMs);
          if (update.rebuildMs !== undefined) rebuildMs.push(update.rebuildMs);
          if (update.rebuilt) projectorRebuilds++;
          edits++;
        } else {
          rejected++;
        }
        await sleep(1000 / rate);
      } else {
        await sleep(100);
      }

      if (Date.now() >= nextStatus) {
        nextStatus += 1000;
        const s = await engine.stats();
        playheadSec = s.audibleSec;
        console.log(
          `   ${((Date.now() - began) / 1000).toFixed(0).padStart(3)} s  pos ${s.audibleSec.toFixed(2)} s  ` +
            `dropouts ${s.xruns < 0 ? 'n/a' : s.xruns - start.xruns}  probe clicks ${s.detector.probeClicks}  gaps ${s.detector.probeGaps}  ` +
            `stops ${s.unexpectedStops - start.unexpectedStops}  edits ${edits}  cpu ${(s.cpu * 100).toFixed(1)}%` +
            (s.maxBlockMs >= 0 ? `  slowest block ${s.maxBlockMs.toFixed(1)} ms, ${s.lateBlocks} late` : ''),
        );
      }
    }

    const end = await engine.stats();
    results.push({
      name,
      seconds: end.detector.seconds,
      edits,
      xruns: end.xruns < 0 ? null : end.xruns - start.xruns,
      probeClicks: end.detector.probeClicks,
      probeGaps: end.detector.probeGaps,
      maxProbeStep: end.detector.maxProbeStep,
      unexpectedStops: end.unexpectedStops - start.unexpectedStops,
      lateBlocks: end.lateBlocks < 0 ? null : end.lateBlocks,
      maxBlockMs: end.maxBlockMs < 0 ? null : end.maxBlockMs,
      eventEditTimes: end.detector.eventEditTimes,
    });
  };

  await phase('quiet', 10, false);
  await phase('edits', editSeconds, true);
  await phase('after', 5, false);

  const drift = await projector.verify();
  await engine.transport({ play: false });
  await engine.quit();

  console.log('\n| Phase | Seconds | Edits | Device dropouts | Probe clicks | Probe gaps | Unexpected stops | Late blocks (simulated) |');
  console.log('| --- | --- | --- | --- | --- | --- | --- | --- |');
  for (const r of results) {
    console.log(
      `| ${r.name} | ${r.seconds.toFixed(1)} | ${r.edits} | ${r.xruns ?? 'n/a'} | ${r.probeClicks} | ${r.probeGaps} | ${r.unexpectedStops} | ${r.lateBlocks ?? 'n/a'} |`,
    );
  }
  console.log(
    `\nModel to engine, per edit (round trip through the engine process): median ${percentile(updateMs, 50).toFixed(2)} ms, p95 ${percentile(updateMs, 95).toFixed(2)} ms, max ${percentile(updateMs, 100).toFixed(2)} ms`,
  );
  console.log(
    `  of which changing the edit: median ${percentile(applyMs, 50).toFixed(2)} ms; Tracktion's graph rebuild: median ${percentile(rebuildMs, 50).toFixed(2)} ms, p95 ${percentile(rebuildMs, 95).toFixed(2)} ms, max ${percentile(rebuildMs, 100).toFixed(2)} ms`,
  );
  console.log(`Edits the model rejected (nothing to change): ${rejected}. Rebuilds after a refused command: ${projectorRebuilds}.`);
  console.log(drift === undefined ? 'The engine matches the model at the end.' : `DRIFT: ${drift}`);

  const edits = results.find((r) => r.name === 'edits');
  const quiet = results.find((r) => r.name === 'quiet');
  // A late block on the simulated sound card is what a dropout would be on a real one.
  const clean = (r: PhaseResult | undefined) =>
    r !== undefined && r.probeClicks === 0 && r.probeGaps === 0 && (r.xruns ?? 0) === 0 && (r.lateBlocks ?? 0) === 0;
  const verdict =
    drift !== undefined
      ? 'FAIL: the engine drifted from the model'
      : !clean(quiet)
        ? 'INCONCLUSIVE: this machine drops out with no edits at all; fix that first (device, buffer size)'
        : clean(edits)
          ? 'PASS: no dropout detected during edits' + (edits?.xruns === null && !audio.simulated ? ' (this device has no dropout counter: listen too)' : '')
          : 'FAIL: dropouts during edits that were not there without them';
  console.log(`\n${verdict}`);
  if (results.some((r) => r.unexpectedStops > 0)) {
    console.log('Note: the transport stopped by itself and was restarted; that is the MIDI device problem in #101, not this check.');
  }

  const report = {
    verdict,
    when: new Date().toISOString(),
    platform: process.platform,
    audio,
    options: { editSeconds, rate, seed: number('--seed', 11), extraArgs, samples: option('--samples') ?? 'generated' },
    session: { tracks: state.trackOrder.length, clips: Object.keys(state.clips).length },
    phases: results,
    updateMs: { p50: percentile(updateMs, 50), p95: percentile(updateMs, 95), max: percentile(updateMs, 100) },
    rebuildMs: { p50: percentile(rebuildMs, 50), p95: percentile(rebuildMs, 95), max: percentile(rebuildMs, 100) },
    rejected,
    projectorRebuilds,
    drift: drift ?? null,
    engineLog: log,
  };
  const file = `dropout-check-${process.platform}-${new Date().toISOString().replace(/[:.]/g, '-')}.json`;
  writeFileSync(file, JSON.stringify(report, null, 2));
  console.log(`Report: ${file}`);
  process.exit(verdict.startsWith('PASS') ? 0 : 1);
}

main().catch((error: unknown) => {
  console.error(error);
  process.exit(1);
});
