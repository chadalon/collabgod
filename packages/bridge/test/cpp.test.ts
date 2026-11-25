// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generated C++. The app's build compiles it and its --bridge-test runs every
// example through it; these check what the generator writes and that it refuses what
// C++ cannot hold.

import { describe, expect, it } from 'vitest';
import { commands, contract, events, functions } from '../src/contract';
import { generateContractHeader, generateCpp, generateExamplesHeader } from '../src/cpp';
import { t } from '../src/schema';

const header = generateContractHeader(contract);
const examples = generateExamplesHeader(contract);

describe('BridgeContract.h', () => {
  it('declares a handler for every function, with its types', () => {
    for (const [name, def] of Object.entries(functions)) {
      const result = def.result === null ? 'void' : def.result.name;
      const params = def.params === null ? '()' : ` (const ${def.params.name}& params)`;
      expect(header).toContain(`virtual ${result} ${name}${params} = 0;`);
      expect(header).toContain(`if (name == "${name}")`);
    }
  });

  it('declares a handler for every command, and decodes it in a batch', () => {
    for (const [name, def] of Object.entries(commands)) {
      expect(header).toContain(`virtual void ${name} (const ${def.args.name}& command) = 0;`);
      expect(header).toContain(`type.toString() == "${name}"`);
    }
  });

  it('has an emit function for every event', () => {
    for (const [name, def] of Object.entries(events)) {
      const fn = `emit${name.charAt(0).toUpperCase()}${name.slice(1)}`;
      expect(header).toContain(`inline void ${fn} (juce::WebBrowserComponent& browser, const ${def.payload.name}& payload)`);
    }
  });

  it('carries the contract version and the background colour', () => {
    expect(header).toContain(`inline constexpr int contractVersion = ${contract.version};`);
    expect(header).toContain(`backgroundArgb = 0xff${contract.backgroundColour.slice(1)};`);
  });

  it('declares each type before a type that uses it', () => {
    expect(header.indexOf('struct Timings')).toBeLessThan(header.indexOf('struct Diagnostics'));
    expect(header.indexOf('struct Diagnostics')).toBeLessThan(header.indexOf('struct SelfCheckReport'));
    expect(header.indexOf('enum class FrameDelivery')).toBeLessThan(header.indexOf('struct HostInfo'));
  });

  it('maps each kind of field to its C++ type', () => {
    expect(header).toMatch(/juce::int64 seq \{\};/);
    expect(header).toMatch(/double hostTimeMs \{\};/);
    expect(header).toMatch(/juce::String platform \{\};/);
    expect(header).toMatch(/bool development \{\};/);
    expect(header).toMatch(/std::vector<juce::String> failures \{\};/);
    expect(header).toMatch(/FrameDelivery frameDelivery \{\};/);
  });

  it('is the same every time', () => {
    expect(generateCpp(contract)).toEqual(generateCpp(contract));
  });
});

describe('BridgeContractExamples.h', () => {
  it('has every example, and a round trip for each type', () => {
    const rows = [
      ...Object.values(functions).flatMap((d) => [d.params, d.result].filter((s) => s !== null)),
      ...Object.values(events).map((d) => d.payload),
      ...Object.values(commands).map((d) => d.args),
    ];
    expect(examples).toContain(`std::array<Example, ${rows.length}> all`);
    for (const schema of rows) expect(examples).toContain(`if (type == "${schema!.name}")`);
    expect(examples).toContain(`R"json(${JSON.stringify(events.frame.example)})json"`);
  });
});

describe('the generator refuses', () => {
  const withFunction = (name: string) => ({
    ...contract,
    functions: { ...contract.functions, [name]: { doc: 'x', params: null, result: null, example: { params: undefined, result: undefined } } },
  });

  it('a name that is a C++ keyword', () => {
    expect(() => generateContractHeader(withFunction('delete') as never)).toThrow('C++ keyword');
  });

  it('a function named like the command batch', () => {
    expect(() => generateContractHeader(withFunction('commands') as never)).toThrow("'commands' is both");
  });

  it('a doc comment that would end the C++ comment', () => {
    const bad = { ...contract, events: { frame: { ...events.frame, payload: t.object('Other', {}, 'a */ b') } } };
    expect(() => generateContractHeader(bad as never)).toThrow('*/');
  });

  it('two commands with the same arguments type', () => {
    const bad = { ...contract, commands: { probe: commands.probe, probe2: commands.probe } };
    expect(() => generateContractHeader(bad as never)).toThrow('shares its arguments type');
  });
});
