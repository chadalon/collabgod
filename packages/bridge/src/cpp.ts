// SPDX-License-Identifier: GPL-3.0-or-later
//
// Generates the app's side of the contract as C++, so the C++ compiler checks the app
// against the same definition the page is checked against. build.mjs writes the output
// to dist/cpp, where the app's CMakeLists.txt finds it; it is never committed.
//
//   BridgeContract.h          constants, an enum class per enum, a struct per object, their
//                             JSON codecs, the Handlers interface the app implements (one
//                             pure virtual per function and command, so a missing or
//                             mistyped handler does not compile), dispatch, and an emit
//                             function per event
//   BridgeContractExamples.h  every message's example, and a typed round trip for each, for
//                             the app's contract tests
//
// The hand-written half of the codec (reading and writing booleans, numbers, integers,
// strings and arrays, and the call envelope) is app/src/BridgeCodec.h.

import { type CommandDef, type Contract, commandBatchFunction, type EventDef, type FunctionDef } from './contract';
import { type EnumSchema, namedTypes, type ObjectSchema, type Schema } from './schema';

export interface GeneratedFile {
  readonly name: string;
  readonly text: string;
}

const RESERVED = new Set([
  'auto', 'bool', 'break', 'case', 'catch', 'char', 'class', 'const', 'continue', 'default', 'delete', 'do',
  'double', 'else', 'enum', 'explicit', 'export', 'extern', 'false', 'float', 'for', 'friend', 'goto', 'if',
  'inline', 'int', 'long', 'namespace', 'new', 'nullptr', 'operator', 'private', 'protected', 'public',
  'register', 'return', 'short', 'signed', 'sizeof', 'static', 'struct', 'switch', 'template', 'this',
  'throw', 'true', 'try', 'typedef', 'typename', 'union', 'unsigned', 'using', 'virtual', 'void', 'volatile',
  'while', 'read', 'write', 'dispatch', 'success', 'failure', 'fail', 'describe', 'expectObject', 'toString',
]);

function cppName(name: string, what: string): string {
  if (RESERVED.has(name)) throw new Error(`${what} '${name}' is a C++ keyword or a name the generated code uses`);
  return name;
}

function cppType(schema: Schema): string {
  switch (schema.kind) {
    case 'boolean':
      return 'bool';
    case 'number':
      return 'double';
    case 'integer':
      return 'juce::int64';
    case 'string':
      return 'juce::String';
    case 'enum':
    case 'object':
      return schema.name;
    case 'array':
      return `std::vector<${cppType(schema.items)}>`;
  }
}

function comment(doc: string | undefined, indent: string): string[] {
  if (doc === undefined) return [];
  if (doc.includes('*/')) throw new Error(`A doc comment contains */: ${doc}`);
  return [`${indent}/** ${doc.replace(/\.$/, '')}. */`];
}

const pascal = (name: string): string => name.charAt(0).toUpperCase() + name.slice(1);

/** A C++ string literal. Names in the contract are letters and digits, so only JSON needs a raw string. */
function rawString(text: string): string {
  if (text.includes(')json"')) throw new Error('An example contains )json"');
  return `R"json(${text})json"`;
}

/** Checks what the TypeScript types cannot: unique names across kinds, and no clash with the batch function. */
export function validate(contract: Contract): void {
  const names = new Map<string, string>();
  const claim = (name: string, what: string): void => {
    const previous = names.get(name);
    if (previous !== undefined) throw new Error(`'${name}' is both ${previous} and ${what}`);
    names.set(name, what);
  };
  claim(commandBatchFunction, 'the reserved command batch function');
  for (const name of Object.keys(contract.functions)) claim(cppName(name, 'Function'), 'a function');
  for (const name of Object.keys(contract.commands)) claim(cppName(name, 'Command'), 'a command');
  for (const name of Object.keys(contract.events)) cppName(name, 'Event');

  const argTypes = new Set<string>();
  for (const [name, def] of Object.entries(contract.commands) as [string, CommandDef][]) {
    if (argTypes.has(def.args.name)) throw new Error(`Command ${name} shares its arguments type ${def.args.name} with another command`);
    argTypes.add(def.args.name);
  }
  if (!/^#[0-9a-f]{6}$/.test(contract.backgroundColour)) throw new Error('backgroundColour must be #rrggbb, lower case');
}

function roots(contract: Contract): Schema[] {
  const out: Schema[] = [];
  for (const def of Object.values(contract.functions) as FunctionDef[]) {
    if (def.params !== null) out.push(def.params);
    if (def.result !== null) out.push(def.result);
  }
  for (const def of Object.values(contract.events) as EventDef[]) out.push(def.payload);
  for (const def of Object.values(contract.commands) as CommandDef[]) out.push(def.args);
  return out;
}

function enumCode(e: EnumSchema): string[] {
  const values = e.values.map((v) => cppName(v, `Value of enum ${e.name}`));
  return [
    ...comment(e.doc, '    '),
    `    enum class ${e.name} { ${values.join(', ')} };`,
    '',
    `    inline juce::String toString (${e.name} value)`,
    '    {',
    '        switch (value)',
    '        {',
    ...values.map((v) => `            case ${e.name}::${v}: return "${v}";`),
    '        }',
    '        return {};',
    '    }',
    '',
    `    inline bool read (const juce::var& v, ${e.name}& out, const juce::String& path, juce::String& error)`,
    '    {',
    '        if (! v.isString())',
    `            return fail (error, path, "is " + describe (v) + ", not one of ${values.join(', ')}");`,
    '',
    '        const auto text = v.toString();',
    ...values.map((v) => `        if (text == "${v}") { out = ${e.name}::${v}; return true; }`),
    `        return fail (error, path, "is \\"" + text + "\\", not one of ${values.join(', ')}");`,
    '    }',
    '',
    `    inline juce::var write (${e.name} value) { return toString (value); }`,
    '',
  ];
}

function structCode(o: ObjectSchema): string[] {
  const fields = Object.entries(o.fields);
  const fieldList = fields.map(([f]) => `"${f}"`).join(', ');
  return [
    ...comment(o.doc, '    '),
    `    struct ${o.name}`,
    '    {',
    ...fields.flatMap(([f, s]) => [...comment(s.doc, '        '), `        ${cppType(s)} ${cppName(f, `Field of ${o.name}`)} {};`]),
    '',
    `        bool operator== (const ${o.name}&) const = default;`,
    '    };',
    '',
    `    inline bool read (const juce::var& v, ${o.name}& out, const juce::String& path, juce::String& error)`,
    '    {',
    `        if (! expectObject (v, { ${fieldList} }, "${o.name}", path, error))`,
    '            return false;',
    '',
    fields.length === 0
      ? '        return true;'
      : fields
          .map(([f], i) => `${i === 0 ? '        return ' : '            && '}read (v["${f}"], out.${f}, path + ".${f}", error)`)
          .join('\n') + ';',
    '    }',
    '',
    `    inline juce::var write (const ${o.name}& in)`,
    '    {',
    '        auto* object = new juce::DynamicObject();',
    ...fields.map(([f]) => `        object->setProperty ("${f}", write (in.${f}));`),
    '        return juce::var (object);',
    '    }',
    '',
  ];
}

const paramSig = (def: FunctionDef): string => (def.params === null ? '' : `const ${def.params.name}& params`);
const resultType = (def: FunctionDef): string => (def.result === null ? 'void' : def.result.name);

function dispatchCase(name: string, def: FunctionDef): string[] {
  const call = def.params === null ? `handlers.${name}()` : `handlers.${name} (params)`;
  const answer =
    def.result === null ? [`                ${call};`, '                return success();'] : [`                return success (write (${call}));`];
  if (def.params === null) {
    return [
      `            if (name == "${name}")`,
      '            {',
      '                if (! args.isEmpty())',
      `                    return failure ("${name} takes no arguments");`,
      ...answer,
      '            }',
      '',
    ];
  }
  return [
    `            if (name == "${name}")`,
    '            {',
    `                ${def.params.name} params;`,
    '                juce::String error;',
    '                if (args.size() != 1)',
    `                    return failure ("${name} takes one argument, not " + juce::String (args.size()));`,
    `                if (! read (args[0], params, "${name}.params", error))`,
    '                    return failure (error);',
    ...answer,
    '            }',
    '',
  ];
}

export function generateContractHeader(contract: Contract): string {
  validate(contract);
  const types = namedTypes(roots(contract));
  const fns = Object.entries(contract.functions) as [string, FunctionDef][];
  const evs = Object.entries(contract.events) as [string, EventDef][];
  const cmds = Object.entries(contract.commands) as [string, CommandDef][];
  const argb = `0xff${contract.backgroundColour.slice(1)}`;

  const lines: string[] = [
    '// Generated by packages/bridge/build.mjs from packages/bridge/src/contract.ts. Do not edit:',
    '// change the contract and run `npm run build`. See packages/bridge/README.md.',
    '// SPDX-License-Identifier: GPL-3.0-or-later',
    '',
    '#pragma once',
    '',
    '#include "BridgeCodec.h"',
    '',
    '#include <array>',
    '#include <exception>',
    '#include <variant>',
    '#include <vector>',
    '',
    'namespace collabgod::bridge',
    '{',
    `    inline constexpr int contractVersion = ${contract.version};`,
    '',
    '    /** Behind the page, in the web view and the window. */',
    `    inline constexpr juce::uint32 backgroundArgb = ${argb};`,
    '',
    '    // ------------------------------------------------------------------ types',
    '',
  ];

  for (const type of types) lines.push(...(type.kind === 'enum' ? enumCode(type) : structCode(type)));

  lines.push(
    '    // ------------------------------------------------------------------ commands',
    '',
    '    /** One command from a batch. */',
    `    using Command = std::variant<${cmds.map(([, d]) => d.args.name).join(', ')}>;`,
    '',
    '    // ------------------------------------------------------------------ handlers',
    '',
    '    /** What the app implements: one function per function and command in the contract. All',
    '        are called on the message thread. A function may throw; the page then gets an error. */',
    '    class Handlers',
    '    {',
    '    public:',
    '        virtual ~Handlers() = default;',
    '',
    '        // Functions: the page calls, the app answers.',
    ...fns.flatMap(([name, def]) => [...comment(def.doc, '        '), `        virtual ${resultType(def)} ${name}${def.params === null ? '()' : ` (${paramSig(def)})`} = 0;`]),
    '',
    '        // Commands: the page sends them to the engine, in one batch per display frame.',
    ...cmds.flatMap(([name, def]) => [...comment(def.doc, '        '), `        virtual void ${name} (const ${def.args.name}& command) = 0;`]),
    '    };',
    '',
    `    inline constexpr std::array<const char*, ${fns.length}> functionNames { ${fns.map(([n]) => `"${n}"`).join(', ')} };`,
    `    inline constexpr std::array<const char*, ${evs.length}> eventNames { ${evs.map(([n]) => `"${n}"`).join(', ')} };`,
    `    inline constexpr std::array<const char*, ${cmds.length}> commandNames { ${cmds.map(([n]) => `"${n}"`).join(', ')} };`,
    `    inline constexpr const char* commandBatchFunction = "${commandBatchFunction}";`,
    '',
    '    /** The name a command has in the contract. */',
    '    inline const char* commandName (const Command& command)',
    '    {',
    ...cmds.map(([name, def]) => `        if (std::holds_alternative<${def.args.name}> (command)) return "${name}";`),
    '        return "";',
    '    }',
    '',
    '    /** Decodes a batch, `{ "commands": [{ "type": ..., "args": ... }] }`. All or nothing:',
    '        on the first command that does not match the contract, returns false and fills error. */',
    '    inline bool readBatch (const juce::var& v, std::vector<Command>& out, juce::String& error)',
    '    {',
    `        if (! expectObject (v, { "commands" }, "CommandBatch", "${commandBatchFunction}.params", error))`,
    '            return false;',
    '',
    '        const auto* list = v["commands"].getArray();',
    '        if (list == nullptr)',
    `            return fail (error, "${commandBatchFunction}.params.commands", "is " + describe (v["commands"]) + ", not an array");`,
    '',
    '        out.clear();',
    '        out.reserve ((size_t) list->size());',
    '',
    '        for (int i = 0; i < list->size(); ++i)',
    '        {',
    '            const auto& item = list->getReference (i);',
    `            const auto path = "${commandBatchFunction}.params.commands[" + juce::String (i) + "]";`,
    '',
    '            if (! expectObject (item, { "type", "args" }, "Command", path, error))',
    '                return false;',
    '',
    '            const auto type = item["type"];',
    ...cmds.flatMap(([name, def]) => [
      `            if (type.isString() && type.toString() == "${name}")`,
      '            {',
      `                ${def.args.name} args;`,
      '                if (! read (item["args"], args, path + ".args", error))',
      '                    return false;',
      '                out.emplace_back (std::move (args));',
      '                continue;',
      '            }',
    ]),
    `            return fail (error, path + ".type", "is " + describe (type) + ", not one of ${cmds.map(([n]) => n).join(', ')}");`,
    '        }',
    '',
    '        return true;',
    '    }',
    '',
    '    /** Carries out one command. */',
    '    inline void run (Handlers& handlers, const Command& command)',
    '    {',
    ...cmds.map(([name, def]) => `        if (const auto* c = std::get_if<${def.args.name}> (&command)) handlers.${name} (*c);`),
    '    }',
    '',
    '    /** Answers a call from the page: checks its arguments, calls the handler and wraps the',
    '        answer in the envelope the page expects, { ok, result } or { ok, error }. */',
    '    inline juce::var dispatch (Handlers& handlers, const juce::String& name, const juce::Array<juce::var>& args)',
    '    {',
    '        try',
    '        {',
    ...fns.flatMap(([name, def]) => dispatchCase(name, def)),
    '            if (name == commandBatchFunction)',
    '            {',
    '                std::vector<Command> batch;',
    '                juce::String error;',
    '                if (args.size() != 1)',
    `                    return failure ("${commandBatchFunction} takes one argument, not " + juce::String (args.size()));`,
    '                if (! readBatch (args[0], batch, error))',
    '                    return failure (error);',
    '                for (const auto& command : batch)',
    '                    run (handlers, command);',
    '                return success();',
    '            }',
    '',
    '            return failure ("there is no function named " + name);',
    '        }',
    '        catch (const std::exception& e)',
    '        {',
    '            return failure (name + " failed: " + e.what());',
    '        }',
    '    }',
    '',
    '    /** Registers every function and the command batch with the web view, each answered by dispatch.',
    '        `handlers` must outlive the web view. */',
    '    inline juce::WebBrowserComponent::Options withHandlers (juce::WebBrowserComponent::Options options, Handlers& handlers)',
    '    {',
    '        const auto add = [&] (const char* name)',
    '        {',
    '            options = options.withNativeFunction (name, [&handlers, n = juce::String (name)] (const juce::Array<juce::var>& args,',
    '                                                                                           juce::WebBrowserComponent::NativeFunctionCompletion done)',
    '            {',
    '                done (dispatch (handlers, n, args));',
    '            });',
    '        };',
    '',
    '        for (const auto* name : functionNames)',
    '            add (name);',
    '',
    '        add (commandBatchFunction);',
    '        return options;',
    '    }',
    '',
    '    // ------------------------------------------------------------------ events',
    '',
    ...evs.flatMap(([name, def]) => [
      ...comment(def.doc, '    '),
      `    inline void emit${pascal(name)} (juce::WebBrowserComponent& browser, const ${def.payload.name}& payload)`,
      '    {',
      `        browser.emitEventIfBrowserIsVisible ("${name}", write (payload));`,
      '    }',
      '',
    ]),
    '} // namespace collabgod::bridge',
    '',
  );

  return lines.join('\n');
}

export function generateExamplesHeader(contract: Contract): string {
  validate(contract);
  const rows: { kind: string; name: string; role: string; type: string; json: string }[] = [];
  for (const [name, def] of Object.entries(contract.functions) as [string, FunctionDef][]) {
    if (def.params !== null) rows.push({ kind: 'function', name, role: 'params', type: def.params.name, json: JSON.stringify(def.example.params) });
    if (def.result !== null) rows.push({ kind: 'function', name, role: 'result', type: def.result.name, json: JSON.stringify(def.example.result) });
  }
  for (const [name, def] of Object.entries(contract.events) as [string, EventDef][]) {
    rows.push({ kind: 'event', name, role: 'payload', type: def.payload.name, json: JSON.stringify(def.example) });
  }
  for (const [name, def] of Object.entries(contract.commands) as [string, CommandDef][]) {
    rows.push({ kind: 'command', name, role: 'args', type: def.args.name, json: JSON.stringify(def.example) });
  }

  return [
    '// Generated by packages/bridge/build.mjs from packages/bridge/src/contract.ts. Do not edit.',
    "// Every message's example from the contract, for the app's contract tests.",
    '// SPDX-License-Identifier: GPL-3.0-or-later',
    '',
    '#pragma once',
    '',
    '#include "BridgeContract.h"',
    '',
    'namespace collabgod::bridge::examples',
    '{',
    '    struct Example',
    '    {',
    '        const char* kind; // function, event or command',
    '        const char* name;',
    '        const char* role; // params, result, payload or args',
    '        const char* type; // the C++ type it decodes into',
    '        const char* json;',
    '    };',
    '',
    `    inline constexpr std::array<Example, ${rows.length}> all {{`,
    ...rows.map((r) => `        { "${r.kind}", "${r.name}", "${r.role}", "${r.type}", ${rawString(r.json)} },`),
    '    }};',
    '',
    '    /** Decodes `in` as the C++ type named `type`, then encodes it again into `out`.',
    '        Returns false and fills `error` if it does not decode, or if no type has that name. */',
    '    inline bool roundTrip (const juce::String& type, const juce::var& in, juce::var& out, juce::String& error)',
    '    {',
    ...[...new Set(rows.map((r) => r.type))].flatMap((type) => [
      `        if (type == "${type}")`,
      '        {',
      `            ${type} value;`,
      `            if (! read (in, value, "${type}", error))`,
      '                return false;',
      '            out = write (value);',
      '            return true;',
      '        }',
    ]),
    '        error = "no type named " + type;',
    '        return false;',
    '    }',
    '} // namespace collabgod::bridge::examples',
    '',
  ].join('\n');
}

/** Both generated headers. */
export function generateCpp(contract: Contract): GeneratedFile[] {
  return [
    { name: 'BridgeContract.h', text: generateContractHeader(contract) },
    { name: 'BridgeContractExamples.h', text: generateExamplesHeader(contract) },
  ];
}
