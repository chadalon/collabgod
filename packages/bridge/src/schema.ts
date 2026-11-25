// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small schema language for what crosses the web view bridge. A schema is a plain
// value, so the same definition gives the TypeScript type (Infer), the runtime check
// (decode) and the generated C++ (cpp.ts). Only what JSON carries and C++ can hold
// without surprises: booleans, finite numbers, safe integers, strings, string enums,
// arrays and named objects. Every field is required and unknown fields are refused.

export interface BooleanSchema {
  readonly kind: 'boolean';
  readonly doc?: string;
}

/** A finite number. C++ holds it as a double. */
export interface NumberSchema {
  readonly kind: 'number';
  readonly doc?: string;
}

/** A whole number within JavaScript's safe range. C++ holds it as a juce::int64. */
export interface IntegerSchema {
  readonly kind: 'integer';
  readonly doc?: string;
}

export interface StringSchema {
  readonly kind: 'string';
  readonly doc?: string;
}

/** One of a fixed set of strings. C++ holds it as an enum class of the same name. */
export interface EnumSchema<V extends string = string> {
  readonly kind: 'enum';
  readonly name: string;
  readonly values: readonly V[];
  readonly doc?: string;
}

export interface ArraySchema<T extends Schema = Schema> {
  readonly kind: 'array';
  readonly items: T;
  readonly doc?: string;
}

export type Fields = { readonly [field: string]: Schema };

/** An object with exactly these fields. C++ holds it as a struct of the same name. */
export interface ObjectSchema<F extends Fields = Fields> {
  readonly kind: 'object';
  readonly name: string;
  readonly fields: F;
  readonly doc?: string;
}

export type Schema = BooleanSchema | NumberSchema | IntegerSchema | StringSchema | EnumSchema | ArraySchema | ObjectSchema;

/** The TypeScript type of a value that matches a schema. */
export type Infer<S> = S extends BooleanSchema
  ? boolean
  : S extends NumberSchema | IntegerSchema
    ? number
    : S extends StringSchema
      ? string
      : S extends EnumSchema<infer V>
        ? V
        : S extends ArraySchema<infer T>
          ? Infer<T>[]
          : S extends ObjectSchema<infer F>
            ? { [K in keyof F]: Infer<F[K]> }
            : never;

const identifier = /^[A-Za-z][A-Za-z0-9]*$/;

function checkName(name: string, what: string): void {
  if (!identifier.test(name)) throw new Error(`${what} '${name}' must be letters and digits, starting with a letter`);
}

/** Builders for schemas. Each returns a new value; `doc` is carried into the generated C++. */
export const t = {
  boolean: (doc?: string): BooleanSchema => (doc === undefined ? { kind: 'boolean' } : { kind: 'boolean', doc }),
  number: (doc?: string): NumberSchema => (doc === undefined ? { kind: 'number' } : { kind: 'number', doc }),
  integer: (doc?: string): IntegerSchema => (doc === undefined ? { kind: 'integer' } : { kind: 'integer', doc }),
  string: (doc?: string): StringSchema => (doc === undefined ? { kind: 'string' } : { kind: 'string', doc }),

  enum<const V extends string>(name: string, values: readonly V[], doc?: string): EnumSchema<V> {
    checkName(name, 'Enum');
    if (values.length === 0) throw new Error(`Enum ${name} has no values`);
    for (const v of values) checkName(v, `Value of enum ${name}`);
    if (new Set(values).size !== values.length) throw new Error(`Enum ${name} repeats a value`);
    return doc === undefined ? { kind: 'enum', name, values } : { kind: 'enum', name, values, doc };
  },

  array<T extends Schema>(items: T, doc?: string): ArraySchema<T> {
    return doc === undefined ? { kind: 'array', items } : { kind: 'array', items, doc };
  },

  object<const F extends Fields>(name: string, fields: F, doc?: string): ObjectSchema<F> {
    checkName(name, 'Object');
    for (const field of Object.keys(fields)) checkName(field, `Field of ${name}`);
    return doc === undefined ? { kind: 'object', name, fields } : { kind: 'object', name, fields, doc };
  },

  /** The same schema with a doc comment, for a field. */
  doc<S extends Schema>(schema: S, doc: string): S {
    return { ...schema, doc };
  },
};

/** A value that does not match the contract. `path` says where, for example `frame.seq`. */
export class ContractError extends Error {
  constructor(
    readonly path: string,
    readonly problem: string,
  ) {
    super(`${path} ${problem}`);
    this.name = 'ContractError';
  }
}

function describe(value: unknown): string {
  if (value === null) return 'null';
  if (Array.isArray(value)) return 'an array';
  if (typeof value === 'number' && !Number.isFinite(value)) return String(value);
  return typeof value === 'object' ? 'an object' : `a ${typeof value}`;
}

/**
 * Checks that `value` matches `schema` and returns it, typed. Throws ContractError
 * naming the first place it does not. Returns a fresh copy, so the caller may keep it.
 */
export function decode<S extends Schema>(schema: S, value: unknown, path = 'value'): Infer<S> {
  return decodeAny(schema, value, path) as Infer<S>;
}

function decodeAny(schema: Schema, value: unknown, path: string): unknown {
  switch (schema.kind) {
    case 'boolean':
      if (typeof value !== 'boolean') throw new ContractError(path, `is ${describe(value)}, not a boolean`);
      return value;
    case 'number':
      if (typeof value !== 'number' || !Number.isFinite(value)) {
        throw new ContractError(path, `is ${describe(value)}, not a finite number`);
      }
      return value;
    case 'integer':
      if (typeof value !== 'number' || !Number.isSafeInteger(value)) {
        throw new ContractError(path, `is ${typeof value === 'number' ? value : describe(value)}, not a whole number`);
      }
      return value;
    case 'string':
      if (typeof value !== 'string') throw new ContractError(path, `is ${describe(value)}, not a string`);
      return value;
    case 'enum':
      if (typeof value !== 'string' || !schema.values.includes(value)) {
        throw new ContractError(path, `is ${JSON.stringify(value) ?? describe(value)}, not one of ${schema.values.join(', ')}`);
      }
      return value;
    case 'array':
      if (!Array.isArray(value)) throw new ContractError(path, `is ${describe(value)}, not an array`);
      return value.map((item: unknown, i) => decodeAny(schema.items, item, `${path}[${i}]`));
    case 'object': {
      if (typeof value !== 'object' || value === null || Array.isArray(value)) {
        throw new ContractError(path, `is ${describe(value)}, not an object (${schema.name})`);
      }
      const record = value as Record<string, unknown>;
      const out: Record<string, unknown> = {};
      for (const [field, fieldSchema] of Object.entries(schema.fields)) {
        if (!Object.hasOwn(record, field)) throw new ContractError(`${path}.${field}`, 'is missing');
        out[field] = decodeAny(fieldSchema, record[field], `${path}.${field}`);
      }
      for (const field of Object.keys(record)) {
        if (!Object.hasOwn(schema.fields, field)) throw new ContractError(`${path}.${field}`, `is not a field of ${schema.name}`);
      }
      return out;
    }
  }
}

/** Every named type a schema uses, objects and enums, each once, dependencies first. */
export function namedTypes(roots: readonly Schema[]): (ObjectSchema | EnumSchema)[] {
  const out: (ObjectSchema | EnumSchema)[] = [];
  const byName = new Map<string, Schema>();

  const visit = (schema: Schema): void => {
    if (schema.kind === 'array') return visit(schema.items);
    if (schema.kind !== 'object' && schema.kind !== 'enum') return;
    const seen = byName.get(schema.name);
    if (seen !== undefined) {
      if (!sameShape(seen, schema)) throw new Error(`Two different types are both named ${schema.name}`);
      return;
    }
    byName.set(schema.name, schema);
    if (schema.kind === 'object') for (const field of Object.values(schema.fields)) visit(field);
    out.push(schema);
  };

  roots.forEach(visit);
  return out;
}

/** Whether two schemas describe the same values, ignoring doc comments. */
export function sameShape(a: Schema, b: Schema): boolean {
  if (a.kind !== b.kind) return false;
  switch (a.kind) {
    case 'enum': {
      const other = b as EnumSchema;
      return a.name === other.name && a.values.length === other.values.length && a.values.every((v, i) => v === other.values[i]);
    }
    case 'array':
      return sameShape(a.items, (b as ArraySchema).items);
    case 'object': {
      const other = b as ObjectSchema;
      const keys = Object.keys(a.fields);
      return (
        a.name === other.name &&
        keys.length === Object.keys(other.fields).length &&
        keys.every((k) => {
          const x = a.fields[k];
          const y = other.fields[k];
          return x !== undefined && y !== undefined && sameShape(x, y);
        })
      );
    }
    default:
      return true;
  }
}
