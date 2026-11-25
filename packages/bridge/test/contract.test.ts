// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every message in the contract: its example matches its schema, and the same example
// with a field missing, an extra field, or a field of the wrong type does not. These
// loop over the contract, so a new message is covered as soon as it is added.

import { describe, expect, it } from 'vitest';
import { backgroundColour, commandBatchFunction, commands, contract, events, functions } from '../src/contract';
import { validate } from '../src/cpp';
import { ContractError, decode, type ObjectSchema, type Schema } from '../src/schema';

type Case = [string, ObjectSchema, unknown];

const cases: Case[] = [
  ...Object.entries(functions).flatMap(([name, def]): Case[] => [
    ...(def.params === null ? [] : [[`function ${name} params`, def.params, def.example.params] as Case]),
    ...(def.result === null ? [] : [[`function ${name} result`, def.result, def.example.result] as Case]),
  ]),
  ...Object.entries(events).map(([name, def]): Case => [`event ${name} payload`, def.payload, def.example]),
  ...Object.entries(commands).map(([name, def]): Case => [`command ${name} args`, def.args, def.example]),
];

/** A value of the wrong kind for a field of this schema. */
function wrongFor(schema: Schema): unknown {
  switch (schema.kind) {
    case 'boolean':
      return 'true';
    case 'number':
    case 'integer':
      return '1';
    case 'string':
      return 1;
    case 'enum':
      return 'notAValue';
    case 'array':
      return {};
    case 'object':
      return [];
  }
}

describe('the contract', () => {
  it('has examples for every message', () => {
    expect(cases.length).toBeGreaterThan(0);
    for (const def of Object.values(functions)) {
      expect(def.params === null).toBe(def.example.params === undefined);
      expect(def.result === null).toBe(def.example.result === undefined);
    }
  });

  it.each(cases)('%s: the example matches', (_, schema, example) => {
    expect(decode(schema, example)).toEqual(example);
  });

  it.each(cases)('%s: a missing, extra or mistyped field is refused', (_, schema, example) => {
    const record = example as Record<string, unknown>;
    for (const [field, fieldSchema] of Object.entries(schema.fields)) {
      const { [field]: _removed, ...without } = record;
      expect(() => decode(schema, without), `without ${field}`).toThrow(ContractError);
      expect(() => decode(schema, { ...record, [field]: wrongFor(fieldSchema) }), `${field} mistyped`).toThrow(ContractError);
    }
    expect(() => decode(schema, { ...record, notInTheContract: 1 })).toThrow(ContractError);
  });

  it('uses each name once, and never the batch function', () => {
    expect(() => validate(contract)).not.toThrow();
    expect(Object.keys(functions)).not.toContain(commandBatchFunction);
    expect(() => validate({ ...contract, commands: { ...commands, hello: commands.probe } } as never)).toThrow("'hello' is both");
  });

  it('gives a background colour the app can parse', () => {
    expect(backgroundColour).toMatch(/^#[0-9a-f]{6}$/);
  });

  it('documents every message', () => {
    const all = [...Object.values(functions), ...Object.values(events), ...Object.values(commands)];
    for (const def of all) expect(def.doc.length).toBeGreaterThan(10);
  });
});
