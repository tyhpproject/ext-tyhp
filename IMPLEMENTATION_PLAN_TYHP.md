# Wiring Tyhp to ext-tyhp

`IMPLEMENTATION_PLAN.md` is done. The `tyhp` module is tagged `0.1.0` (`https://github.com/tyhpproject/ext-tyhp`). On this machine it is loaded in Homebrew PHP 8.5.12 (`/opt/homebrew/bin/php`, `phpversion('tyhp')` is `0.1.0`, module `/opt/homebrew/lib/php/pecl/20250925/tyhp.so`). `php@8.2`, `php@8.3`, and `php@8.4` do not have it. Do not rebuild the extension. This plan wires the compiler and the runtime packages to the class that already exists.

The extension is optional. Every path below keeps today's PHP behavior when the module is missing, when `phpversion('tyhp')` is older than `0.1.0`, or when an `\Tyhp\Ext` function declines a value (`null`). A different result or a different exception is a bug. `tyhp build` emits the same PHP either way.

`\Tyhp\Ext` is a final class. Its static methods are:

| Method | Signature |
| --- | --- |
| `typeIs` | `(object $type, mixed $value): ?bool` |
| `typeParse` | `(string $spelling): ?array` |
| `bindGet` | `(mixed $target, array $typeArguments): ?\Closure` |
| `bindPut` | `(mixed $target, array $typeArguments, \Closure $bound): void` |
| `closureTree` | `(\Closure $fn, string $mode): array` |
| `dnsStart` | `(string $host): array` |
| `dnsTake` | `(int $id): array` |
| `dnsCancel` | `(int $id): void` |

Four repositories change.

| Repository | What changes |
| --- | --- |
| `tyhp-runtime-src` | `tyhpdef/php-ext-tyhp`, then `tyhp/core`, `tyhp/lambda`, `tyhp/io` |
| `tyhp` | The emitted parameter hint and the `fromClosure` prologue. The fluent tree rewrite is already shipped. |
| `tyhp-docs-src` | The pages below, keeping the Story 49 text and adding the extension contract |
| `tyhp-ai-dev-guide` | The guide sections below, on the same terms |

Each user-visible edit gets a line in that repository's `CHANGELOG.md` under `## [Unreleased]`, in the same change. Editing this plan is not one of those lines.

## Version gate

`tyhp/core` owns one process-lifetime check, used by `tyhp/lambda` and `tyhp/io` (both already depend on `tyhp/core`). Put it in a new file under `packages/tyhp/core/tyhp_src/`. `lambda` and `io` call that check. They do not call `extension_loaded` themselves.

- Read `extension_loaded('tyhp')` once. Remember the result.
- When it is loaded, compare `phpversion('tyhp')` with `0.1.0` (`version_compare`). That is the tagged module that exports every method above. An older module is ignored for the rest of the process.
- Callers do not check the extension themselves. `Type::is`, `Type::parse`, `Generic::bind`, `Expression::fromClosure`, `PropertyPath::fromClosure`, and `Dns::resolveAsync` are the entry points.

The minimum version is a constant next to that check. Bump it when a package starts depending on a new `\Tyhp\Ext` function.

`tyhp/core`, `tyhp/lambda`, and `tyhp/io` each add a Composer `suggest` entry:

```json
"ext-tyhp": "Faster type checks, generic bind, and closure trees; DNS lookups off the event loop. Install with: pie install tyhp/ext-tyhp"
```

`suggest` does not install the module. `ext-tyhp` is Composer's name for a loaded `tyhp` extension. `pie install tyhp/ext-tyhp` is the install command. None of these packages `require` `ext-tyhp`.

## `tyhpdef/php-ext-tyhp`

Tyhp code sees the module through a tyhpdef package, the same way it sees any other optional extension. Create it in `tyhp-runtime-src` before the `tyhp/core`, `tyhp/lambda`, and `tyhp/io` edits. Those edits call `\Tyhp\Ext`, and the compiler needs this package on the classpath to resolve that class.

From `tyhp-runtime-src`:

```bash
packages/new-php-ext.sh --php=/opt/homebrew/bin/php tyhp
```

The argument is the loaded module name `tyhp`. The script creates `packages/tyhpdef/php-ext-tyhp/`, Composer package `tyhpdef/php-ext-tyhp`, with `ext-tyhp` in `require` and the reflected file `Ext.Tyhp.tyhpdef`. Match `packages/tyhpdef/php-ext-bz2/composer.json`: `php >=8.2` and `ext-tyhp` in `require`, `tyhpdef/php` in `require-dev` and `extra.tyhp.require`, path repository `../php`. `ext-tyhp` means the `tyhp` module is loaded. It does not install that module.

The stock tyhpdef snapshots do not include this module. `--php` is the reflection binary that already has it loaded, and it cannot be combined with `--php-targets`. The `\Tyhp\Ext` signatures are the same on PHP 8.2 through 8.5, so that one reflection is the package. Regenerate with `--php-targets=8.2,8.3,8.4,8.5` only after each of those PHPs can load the module. On this machine only 8.5.12 can.

After generate, `overlay stamp` from the new package's `tests/` exits 0 or 5. Exit 5 is `TYHP8027` because `tyhp/core`, `tyhp/decimal`, `tyhp/async`, and `tyhp/lambda` are not in that test project. The stamp still applied. Do not add those packages to silence it.

`tyhp/core` already has `tyhpdef/php-ext-mbstring` in `require-dev` plus a path repository, and not in `extra.tyhp.require`. `tyhp/io` already has `tyhpdef/php-ext-curl`, `php-ext-pgsql`, `php-ext-mysqli`, and `php-ext-pdo` the same way. `tyhp/lambda` has no `tyhpdef/php-ext-*` entry today: add `require-dev` and a path repository. All three still only `suggest` `ext-tyhp`. The tyhpdef package's own `require` of `ext-tyhp` is the same platform requirement as `ext-bz2` on `tyhpdef/php-ext-bz2`. Do not copy `ext-tyhp` onto `tyhp/core`, `tyhp/lambda`, or `tyhp/io` `require` because a dev install notices it. A project that calls `\Tyhp\Ext` itself depends on `tyhpdef/php-ext-tyhp` the same way it depends on `tyhpdef/php-ext-curl`.

## tyhp/core

### `Type::is` and `Type::isType`

`packages/tyhp/core/tyhp_src/Type.tyhp`. `isType` already calls `is`. Leave that call.

`is(mixed $value, TypeLike|string $type)` already turns a `TypeLike` or string into a `Type` and unwraps `NamedType` before the `match`. Call `\Tyhp\Ext::typeIs` only once `$type` is a `Type`, with the arguments swapped: `typeIs($type, $value)`. A `bool` answer is returned as-is. `null` falls through to the current `match` on `$type->kind`. An exception from `typeIs` is not a decline.

The C matcher reads `kind`, `name`, `typeArgs`, `structFields`, `structRequiredKeys`, `isNullable`, `forward`, `lazyInner`, `lazyOp`, and `NamedType`'s `underlyingType`. Changing those names is an extension change first.

### `Type::parse`

`Type::parse` calls `\Tyhp\Ext::typeParse`. `null` uses the PHP parser (`tokenizeSpelling` / `parseUnionSpelling`). An array is the graph in `ext-tyhp/src/type_parse.c` (`kind`, `name`, `nullable`, `args`, `members`, `form`), not a `Type`. Build the same `Type` objects the PHP parser builds, including alias and struct resolution. Callers of `parse` still receive a `Type`. The two parsers have to agree on the spellings the extension accepts. A spelling only the PHP parser knows stays on the `null` path. `typeParse` returns `null` for those. It does not throw.

### `Generic::bind`

Already shipped in `packages/tyhp/core/tyhp_src/Generic.tyhp`. Do not reimplement it.

Story 49's binder is the behavior to keep. Layout 2's declared method is the binder (`#[\Tyhp\GenericRuntime(erased: false, binder: "decode", layouts: [2])]`, parameters `\Tyhp\TypeLike|string|null`, return `\Closure`). No `decode__tyhpGeneric` is emitted for that layout. Layout 1, including `#[\Tyhp\InferGeneric]`, keeps the plain method and the `name__tyhpGeneric` companion. `Generic::bind` reads the stamp, accepts layouts `[1, 2]`, normalizes with `Type::arg`, pads missing arguments with `null`, and invokes the binder the stamp names. A class-string bind returns a `Generic` instance. A second `new()` or `__invoke` on that instance throws `\LogicException` (`newWithArgs`). A callable bind returns the binder's `\Closure`.

Null type arguments stay null through `bind`. Inference runs inside the value closure the compiler emits (`Tyhp/TyhpLang/Emitter/TyhpEmitter.GenericVariants.cs`, `EmitVariantClosureInference`), on every value call. The cache stores that closure. It does not infer before `bindGet` / `bindPut`, and it does not skip the inference.

When the gate is open, call `bindGet` with the target and the original type-argument list, nulls included. A returned closure is the result. On a miss, today's `bind` runs. Call `bindPut` only when that result is a `\Closure`. A class bind's `Generic` instance is not a closure; `bindPut` would throw `TypeError` on it, and the one-shot `LogicException` would be lost. Instance methods are rejected inside the extension (an `[object, method]` pair, or a closure with `$this`): `bindGet` returns `null` and `bindPut` returns without storing. Those binds stay uncached. The cache key is the target plus the arguments after `Type::arg`-style normalization (`Type`, `BuiltIn::toType()`, class-string via `Type::fromClassName`). `Type::int()` is a singleton and `BuiltIn::int->toType()` returns it, so `T::int` and `Type::int()` share an entry.

### `Json::decode`

`packages/tyhp/core/tyhp_src/Json.tyhp`. No new C call, and no edit. `decode<T>(string $json, ...)` is the Story 49 binder: `Json::decode(T::struct([...]))($json)`. It checks the value with `Type::isType<T>`, which calls `Type::is`. `tryDecode` calls `decode`. Invalid JSON still throws `\JsonException` from `json_decode`. A value that does not match still throws `\Tyhp\Exceptions\IncompatibleTypeException`. There is no `Json` test in `packages/tyhp/core/tests/` today. Add one with a payload large enough to exercise the struct walk, with the module loaded and with it absent. The two runs return the same value or the same exception.

## tyhp/lambda

`Expression::fromClosure(\Closure $fn, mixed $expectedShape): Expression` and `PropertyPath::fromClosure(\Closure $fn, mixed $expectedShape): PropertyPath` do not exist yet. Add them. `PropertyPath::fromClosure` throws when the body is not a property chain.

`Expression::lambda` and `PropertyPath::from` already take a `\Closure`. That closure is the executable half of a tree the caller already built (`Expression.tyhp`, `PropertyPath.tyhp`). They are binder-first. Do not send them through `closureTree`.

When the gate is open, call `\Tyhp\Ext::closureTree($fn, 'expression')` or `'property-path'`. The array fields are the ones at the top of `ext-tyhp/src/closure_tree.c`. Hydrate `kind` into the existing node classes:

| `kind` | Class |
| --- | --- |
| `param` | `ParameterExpression` |
| `const`, `capture` | `ConstantExpression` (`capture` uses the copied value) |
| `prop` | `PropertyAccessExpression` |
| `nullsafe_prop` | `NullSafeAccessExpression` |
| `index` | `ArrayAccessExpression` |
| `binary` | `BinaryExpression` |
| `unary` | `UnaryExpression` |
| `ternary` | `TernaryExpression` |
| `coalesce` | `CoalesceExpression` |
| `cast` | `CastExpression` |
| `instanceof` | `InstanceofExpression` |
| `new` | `NewExpression` |
| `call` | `MethodCallExpression` |
| `nullsafe_call` | `NullSafeMethodCallExpression` |
| `static_call` | `StaticMethodCallExpression` |
| `array` | `ArrayExpression` |
| `lambda` | `LambdaExpression` |

Then apply `$expectedShape`, so an untyped `fn ($u) => …` still records the parameter type from the Tyhp signature (`User` for `Expression<callable(User): bool>`). `closureTree` returns an array or throws `\InvalidArgumentException`. It does not return `null`. That exception propagates. Do not catch it and tokenize the source.

When the gate is closed, tokenize the closure's source. `ReflectionFunction` supplies the file, start line, and end line. The tokenizer accepts the same bodies the opcode walker accepts and throws `\InvalidArgumentException` for the same rejections. It is the only source parse. The extension never opens the file.

Captured values come from `ReflectionFunction::getStaticVariables()` on this path. The extension path already copied them into the `capture` node.

Trees the compiler built keep their checked types, including `array<Post>`. `fromClosure` is only how a `\Closure` becomes a tree.

## The compiler

Already shipped. Do not redo it. An inline `fn` passed to an `Expression` or `PropertyPath` parameter is rewritten to a fluent tree before emit:

- `Tyhp/TyhpLang/Emitter/ExpressionTreeEmissionHelper.cs`
- `Tyhp/TyhpLang/Emitter/ExpressionTreeEmissionHelper.Fluent.cs`
- `Tyhp/TyhpLang/Emitter/PropertyPathEmissionHelper.cs`
- call sites in `Tyhp/TyhpLang/Emitter/AliasConverter.cs`

The emitted call is `\Tyhp\Expr::param(...)->…->backedBy(fn)` or `\Tyhp\PropertyPath::on(...)->…->backedBy(fn)`. Those call sites do not call `fromClosure`. A closure variable at a `.tyhp` call site stays **TYHP4323** (`Expression`) or **TYHP4320** (`PropertyPath`). `tyhp build` keeps emitting that explicit tree. It does not switch the output over to runtime closure parsing.

Not shipped yet: the emitted PHP signature of that parameter also accepts `\Closure`, and the value body starts with the prologue from `DESIGN.md`:

```php
if ($predicate instanceof \Closure) {
    $predicate = \Tyhp\Expression::fromClosure($predicate, /* expected callable shape */);
}
```

Widen only parameter hints, in `FormatParameter` and `FormatParameterWithoutPromotion` (`Tyhp/TyhpLang/Emitter/TyhpEmitter.Declarations.cs`). `FormatParameterList` feeds both the method signature and the layout-2 value closure (`OpenVariantValueClosure` in `TyhpEmitter.GenericVariants.cs`). Do not widen return types or properties in `SpellEmittedType` (`TyhpEmitter.PhpType.cs`).

Insert the prologue at the start of the value body `EmitMethodBody` already writes. For a layout-2 binder that body is the closure inside the binder, not the binder. The binder stays type arguments only (`TypeLike|string|null`), which Story 49 already emits. Do not add a second prologue in `TyhpEmitter.GenericVariants.cs` unless a fixture shows that closure missed the union. `EmitVariantClosureInference` stays ahead of the body and still sees null type arguments.

The expected shape is the Tyhp parameter's callable type. A `PropertyPath` parameter calls `PropertyPath::fromClosure`. The rest of the body still sees a tree.

Emitted PHP stays legal PHP 8.2 with or without the module. There is no second emit mode. `Tyhp/TyhpLang/Emitter/technical-guide.md` already describes the fluent rewrite. Add the parameter union and the prologue there. Leave the Mechanism C and Mechanism D sections as Story 49 wrote them.

## tyhp/io

`packages/tyhp/io/tyhp_src/Dns/Dns.tyhp`. `resolveAsync` today is only `Offload::runAsync(new DnsLookup($host), $token)`. When the gate is open, call `\Tyhp\Ext::dnsStart` and register the pipe on the running loop:

```php
$loop = \Tyhp\EventLoop::getInstance();
$loop->addReadStream($pipe, $callback);
```

`addReadStream` is an instance method (`packages/tyhp/async/tyhp_src/EventLoop.tyhp`). Do not edit `EventLoop`. The loop already waits on read streams. This does not add a scheduler. `dnsStart` returns `['id' => int, 'pipe' => stream]`. It throws `\RuntimeException` (`Could not start the DNS lookup.`) when the thread cannot start. That is not a decline. Do not catch it and fall through to `Offload`.

When the pipe is readable, remove it with `removeReadStream` (the owner removes the stream, as `IoWait` does), then `dnsTake($id)` on the main thread:

- `ok` true, `addresses` present: fulfill the promise with that list.
- `reason` `unresolved`: reject with `IoException` and the `message` `dnsTake` returned (`Could not resolve host {host}.`), the same text as `DnsLookup::run`.
- `reason` `cancelled`: reject with `OperationCancelledException`. Do not fulfill with addresses.

A token that is already cancelled, or that cancels while the thread is running, calls `dnsCancel`. The thread still finishes. A late result is not written into the promise. `dnsTake` throws `\InvalidArgumentException` for an unknown id and `\LogicException` when the lookup is not ready. Those are call errors, not a fallback.

This path is preferred over `Offload`, including when a worker pool is up. The `Offload` path is unchanged and remains the fallback when the gate is closed. A failure on that fallback still rejects with `ParallelException` when `Offload::usesWorkers()` is true. The extension path is in-process, so its failure is `IoException`.

`packages/tyhp/io/composer.json` `suggest` for `tyhp/parallel` currently says Dns runs in a worker. Say that `Offload` and PDO do, and that Dns uses the worker when `ext-tyhp` is not in use.

`DnsLookup`, PDO, and user `OffloadJob` classes stay as they are. HTTP, sockets, Redis, Postgres, MySQL, processes, and files stay in PHP.

## Docs

Story 49 already updated the pages and guide sections below. Keep that text: binder-first calls (`Json::decode(T::struct([...]))($json)`, `Box::new(T::int)(42)`), `#[\Tyhp\InferGeneric]` companions, `T::int` / `Type::arg` / `Generic::bind`, `_expr` / `_path`, `Expression::of` / `PropertyPath::on`, and the compile-time fluent tree (`Expr::param(...)->backedBy(fn)`, `PropertyPath::on(...)->backedBy(fn)`). Add the extension on top. Do not rewrite those pages around `fromClosure`.

Docs (`tyhp-docs-src/content/`):

- Expression trees and PHP callers: `tyhp_3000_parsableLambdas.md`, `package_lambda.md`, `php_package_lambda.md`, `cli_interopContract.md`
- Generics and PHP call syntax: `tyhp_0500_generics.md`, `package_core.md`, `php_packages.md`, `php_package_core.md`, `quickref.md`
- Install and extensions: `php_package_io.md`, `package_io.md` (Dns is described as an `Offload` worker today), `tyhp_2100_extensions.md`, `tyhpdef_extensions.md`
- Left as Story 49 wrote them unless a sample names Dns or a closure-as-tree: `package_async.md`, `php_package_async.md`, `tyhp_2500_phpMagicMethods.md`, `tyhp_2700_compileTimeConstructs.md`, `tyhp_0152_lazyObjects.md`, `faq_general.md`, `faq_tyhpSyntax.md`, `cli_build.md`, `cli_tyhpdefGeneration.md`, `diagnostics_reference.md`

AI guide:

- `guide/21-expression-trees.md`, `handbook/07-runtime-api.md`, `handbook/05-php-interop.md`, `guide/25-runtime-packages.md`
- Generics, preserved: `guide/07-generics.md`, `guide/29-php-mapping.md`
- Index lines only where they state the expression or install fact: `QUICK_GUIDE.md`, `SKILL.md`, and the matching `REGEN.md` item
- `guide/12-extensions.md` gains the install line. `guide/23-tyhpdef.md` and `guide/26-build-cli.md` stay as they are unless they tell a reader that `tyhp build` emits a different file when the module is loaded.

`php_package_lambda.md` currently says a PHP closure is not captured as a tree. Replace that guidance with the contract below. Keep `_expr`, `_path`, `Expression::of`, and `PropertyPath::on`.

`package_io.md` and `php_package_io.md` say Dns runs through `Offload`. Add that `ext-tyhp` handles the lookup first, and that `Offload` remains the path when the module is absent or too old. Leave the rest of the `Offload` chapter.

### Supported contract

Write this as how Tyhp works. The install line is `pie install tyhp/ext-tyhp`.

- `ext-tyhp` is not required. Tyhp runtime code still behaves as specified when the module is not installed. A missing or too-old module is silent: the same value, or the same exception.
- Installing `ext-tyhp` is recommended. It improves the performance and stability of Tyhp runtime code.
- PHP that uses `tyhp/lambda` — a Composer package, or any PHP caller — builds expressions and property paths with the explicit fluent API (`_expr` / `Expression::of`, `_path` / `PropertyPath::on`, and the `backedBy` chains `tyhp build` emits). That PHP does not rely on parsing a closure at runtime. A package author cannot know whether the process that later runs it has `ext-tyhp` loaded, or which version is loaded.
- A PHP caller who knows that process will have `ext-tyhp` loaded may pass a closure to an `Expression` or `PropertyPath` parameter. That is the easier PHP spelling in that case. `fromClosure` turns it into the same kind of tree.
- `tyhp build` keeps emitting the explicit expression and property-path declarations for an inline `fn`. The compiler does not switch that output over to runtime closure parsing. A closure variable in `.tyhp` is still TYHP4323 or TYHP4320.
- In PHP, the explicit fluent expressions and property paths are the recommended form for speed and stability, whether or not `ext-tyhp` is installed.

`Type::is`, `Type::parse`, `Generic::bind`, and `Json::decode` keep the call shapes Story 49 documented. The module speeds the match and the bind cache. `Generic::bind` is still the PHP binder.

Each of `tyhp-docs-src` and `tyhp-ai-dev-guide` gets one `## [Unreleased]` line for the extension contract.

## Tests

On this machine the opcode path and `dnsStart` run under `/opt/homebrew/bin/php` (8.5.12, module loaded). `php -n` is the module-absent run. `php@8.2`, `php@8.3`, and `php@8.4` do not load the module, so they exercise the PHP path only. Do not block this work on installing the module for those binaries.

- `Type::is` and `Type::parse`: the same values, including a node or a spelling the extension declines (`null`, then the PHP path).
- `Generic::bind`: a static callable binder hits the cache, `T::int` and `Type::int()` share it, an instance binder does not, a class bind is not cached and a second `new()` throws `\LogicException`, and a null type argument still infers on the value call.
- `Json::decode(T::struct([...]))($json)`: the same decoded struct or the same `\JsonException` / `IncompatibleTypeException`. `tryDecode` follows `decode`.
- `fromClosure`: with the module, the opcode path; without it, the source path. The trees evaluate equally. A rejected body throws on both paths. A property path rejects an index. `closureTree` throwing does not fall through to the tokenizer.
- Compiler fixtures: an inline `fn` in `.tyhp` is still emitted as a fluent `backedBy` chain (`tests/Tyhp.Tests/Emitter/ExpressionTreeCheckerEmitterParityTests.cs` and the other expression-tree emitter tests). A new fixture shows the `\Closure` union and the prologue on the value signature, including a layout-2 binder's inner closure. The binder signature stays type arguments only.
- `Dns::resolveAsync`: the same addresses as `DnsLookup::run` (`packages/tyhp/io/tests/OffloadTest.php` already awaits `127.0.0.1` and `localhost`). Cancellation rejects with `OperationCancelledException` and does not apply a late result. With the module absent, the call still goes through `Offload`.

## Done

This plan is done when `tyhpdef/php-ext-tyhp` describes `\Tyhp\Ext`, those suites agree with and without the module, the three runtime packages suggest `ext-tyhp` and do not require it, the docs and the AI guide state the contract above without replacing the Story 49 pages, and `tyhp build` of an expression-tree call still emits one PHP file of explicit trees that runs on 8.2 either way.

## Phases

Do these in order. Each phase leaves the packages working with and without the module.

1. **`tyhpdef/php-ext-tyhp`.** Repo `tyhp-runtime-src`. Run `packages/new-php-ext.sh --php=/opt/homebrew/bin/php tyhp`. That creates `packages/tyhpdef/php-ext-tyhp/` (`composer.json`, `Ext.Tyhp.tyhpdef`, overlays). Accept overlay-stamp exit 0 or 5. No edits to `tyhp/core`, `tyhp/lambda`, `tyhp/io`, or the compiler.

2. **`tyhp/core` gate, `Type::is`, `Type::parse`, `Generic::bind`.** Repo `tyhp-runtime-src`. `packages/tyhp/core/composer.json` (`suggest`, `require-dev`, path repository). New gate file under `packages/tyhp/core/tyhp_src/`. `Type.tyhp` and `Generic.tyhp` only as the hooks above. Do not edit `Json.tyhp`. Tests in `packages/tyhp/core/tests/TypeTest.php`, `GenericTest.php`, and a new JSON test. `CHANGELOG.md`.

3. **`fromClosure` and the compiler prologue.** Repos `tyhp-runtime-src` and `tyhp`. `packages/tyhp/lambda/tyhp_src/Expression.tyhp`, `PropertyPath.tyhp`, `packages/tyhp/lambda/composer.json`, tests in `packages/tyhp/lambda/tests/ExpressionTest.php` and `PropertyPathTest.php`. Compiler: `Tyhp/TyhpLang/Emitter/TyhpEmitter.Declarations.cs` and `Tyhp/TyhpLang/Emitter/technical-guide.md`. Do not edit `ExpressionTreeEmissionHelper.cs`, `ExpressionTreeEmissionHelper.Fluent.cs`, or `PropertyPathEmissionHelper.cs`. Touch `TyhpEmitter.GenericVariants.cs` only if a layout-2 fixture misses the union. New emitter fixture next to `tests/Tyhp.Tests/Emitter/ExpressionTreeCheckerEmitterParityTests.cs`. Both changelogs.

4. **`Dns::resolveAsync`.** Repo `tyhp-runtime-src`. `packages/tyhp/io/tyhp_src/Dns/Dns.tyhp` and `packages/tyhp/io/composer.json` (`suggest` for `ext-tyhp`, the `tyhp/parallel` suggest line, `require-dev`, path repository). Do not edit `EventLoop.tyhp` or `DnsLookup.tyhp`. Extend `packages/tyhp/io/tests/OffloadTest.php`. `CHANGELOG.md`.

5. **Docs and AI guide.** Repos `tyhp-docs-src` and `tyhp-ai-dev-guide`. The files named in [Docs](#docs). Preserve the Story 49 pages. Add the [supported contract](#supported-contract). One changelog line in each repo.

6. **Package tests, module loaded and `php -n`.** Repos `tyhp-runtime-src` and `tyhp`. Run the core, lambda, and io suites, and the expression-tree emitter fixtures, under `/opt/homebrew/bin/php` and under `php -n`. The [Tests](#tests) list is the pass bar.
