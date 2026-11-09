# Wiring Tyhp to ext-tyhp

This plan starts after `IMPLEMENTATION_PLAN.md` is done: the `tyhp` module loads, `phpversion('tyhp')` reports its version, and the `\Tyhp\Ext` functions behave as that plan specifies. Nothing in that plan edits these repositories. This plan does.

The extension is optional. Every path below keeps today's PHP behavior when the module is missing, when `phpversion('tyhp')` is older than the package requires, or when an `\Tyhp\Ext` function declines a value. A different result or a different exception is a bug.

Three repositories change.

| Repository | What changes |
| --- | --- |
| `tyhp-runtime-src` | `tyhp/core`, `tyhp/lambda`, `tyhp/io` |
| `tyhp` | The emitter, for `Expression\|\Closure` and `PropertyPath\|\Closure` parameters |
| `tyhp-docs-src` and `tyhp-ai-dev-guide` | The pages and guide sections that describe calling those parameters from PHP, and installing the module |

Each user-visible edit gets a line in that repository's `CHANGELOG.md` under `## [Unreleased]`, in the same change.

## Version gate

`tyhp/core` owns one process-lifetime check, used by `tyhp/lambda` and `tyhp/io` (both already depend on `tyhp/core`).

- Read `extension_loaded('tyhp')` once. Remember the result.
- When it is loaded, compare `phpversion('tyhp')` with the minimum this package set was built against. An older module is ignored for the rest of the process.
- Callers do not check the extension themselves. `Type::is`, `Generic::bind`, `Expression::fromClosure`, and `Dns::resolveAsync` are the entry points.

The minimum version is a constant next to that check. Bump it when a package starts depending on a new `\Tyhp\Ext` function.

`tyhp/core`, `tyhp/lambda`, and `tyhp/io` each add a Composer `suggest` entry:

```json
"ext-tyhp": "Faster type checks, generic bind, and closure trees; DNS lookups off the event loop. Install with: pie install tyhp/ext-tyhp"
```

`suggest` does not install the module. `ext-tyhp` is Composer's name for a loaded `tyhp` extension. `pie install tyhp/ext-tyhp` is the install command from `IMPLEMENTATION_PLAN.md`. None of these packages `require` `ext-tyhp`.

## tyhp/core

### `Type::is` and `Type::isType`

`packages/tyhp/core/tyhp_src/Type.tyhp`. `isType` already calls `is`. `is` calls `\Tyhp\Ext::typeIs` when the gate is open. A `bool` answer is returned as-is. `null` falls through to the current `match` on `$type->kind`.

The stand-in `Tyhp\Type` in the extension tests and this class must keep the same field names the matcher reads. Changing `kind`, `typeArgs`, `structFields`, or the follow fields is an extension change first.

### `Type::parse`

`Type::parse` calls `\Tyhp\Ext::typeParse`. An array result is built into the same `Type` objects the PHP parser builds today. `null` uses the PHP parser. The two parsers have to agree on the spellings the extension accepts. A spelling only the PHP parser knows stays on the `null` path.

### `Generic::bind`

`packages/tyhp/core/tyhp_src/Generic.tyhp`. The binder stays in PHP, including both layouts in `DESIGN.md` (the declared method, and a `name__tyhpGeneric` companion), `Type::arg`, inference when a binder argument is null, and the `\LogicException` a one-shot binder throws on a second `new()` or `__invoke`.

On entry, when the gate is open, `bindGet($target, $typeArguments)` may return a closure. That closure is the result. On a miss, today's bind runs, then `bindPut` stores the closure it returned. `bindPut` ignores instance methods, so those binds stay uncached. The cache key is computed inside the extension, which is what makes `T::int` and `Type::int()` share an entry.

### `Json::decode`

`packages/tyhp/core/tyhp_src/Json.tyhp`. No new C call. `decode` already checks the value with `Type::isType`, which goes through `Type::is`. Invalid JSON still throws `\JsonException` from `json_decode`. A value that does not match still throws `\Tyhp\Exceptions\IncompatibleTypeException`. Confirm that with a payload large enough to exercise the struct walk, with the module loaded and with it absent. The two runs return the same value or the same exception.

## tyhp/lambda

`Expression::fromClosure(\Closure $fn, mixed $expectedShape): Expression` and `PropertyPath::fromClosure(\Closure $fn, mixed $expectedShape): PropertyPath` are the public entry points. They do not exist yet. `PropertyPath::fromClosure` throws when the body is not a property chain.

When the gate is open, call `\Tyhp\Ext::closureTree($fn, 'expression')` or `'property-path'`. Hydrate the array into the existing node classes:

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

Then apply `$expectedShape`, so an untyped `fn ($u) => …` still records the parameter type from the Tyhp signature (`User` for `Expression<callable(User): bool>`). `\InvalidArgumentException` from `closureTree` propagates.

When the gate is closed, tokenize the closure's source. `ReflectionFunction` supplies the file, start line, and end line. The tokenizer accepts the same bodies the opcode walker accepts and throws `\InvalidArgumentException` for the same rejections. It is the only source parse. The extension never opens the file.

Captured values come from `ReflectionFunction::getStaticVariables()` on this path. The extension path already copied them into the `capture` node.

Trees the compiler built keep their checked types, including `array<Post>`. `fromClosure` is only how a `\Closure` becomes a tree.

## The compiler

In `tyhp`, an inline `fn` passed to an `Expression` or `PropertyPath` parameter is still rewritten to a fluent tree before emit (`Tyhp/TyhpLang/Emitter/ExpressionTreeEmissionHelper.cs` and `ExpressionTreeEmissionHelper.Fluent.cs`). Those call sites do not call `fromClosure`. A closure variable at a Tyhp call site stays an error.

The emitted signature of that parameter is `Expression|\Closure` or `PropertyPath|\Closure`. The emitter inserts the prologue from `DESIGN.md` at the start of the function:

```php
if ($predicate instanceof \Closure) {
    $predicate = \Tyhp\Expression::fromClosure($predicate, /* expected callable shape */);
}
```

The expected shape is the Tyhp parameter type. A `PropertyPath` parameter calls `PropertyPath::fromClosure`. The rest of the body still sees a tree.

Emitted PHP stays legal PHP 8.2 with or without the module. There is no second emit mode.

## tyhp/io

`packages/tyhp/io/tyhp_src/Dns/Dns.tyhp`. When the gate is open, `Dns::resolveAsync` calls `\Tyhp\Ext::dnsStart` and registers the pipe with `EventLoop::addReadStream` (`packages/tyhp/async/tyhp_src/EventLoop.tyhp`). The loop already waits on read streams. This does not add a scheduler.

When the pipe is readable, `dnsTake` runs on the main thread. `ok` fulfills the promise with the address list. `unresolved` rejects with `IoException` and the message `dnsTake` returned, the same message as `DnsLookup::run`. `cancelled` rejects with `OperationCancelledException`. A token that is already cancelled, or that cancels while the thread is running, calls `dnsCancel`. The thread still finishes. A late result is not written into the promise.

This path is preferred over `Offload`, including when a worker pool is up. The `Offload` path is unchanged and remains the fallback when the gate is closed. A failure on that fallback still rejects with `ParallelException` when `Offload::usesWorkers()` is true, as `Dns::resolveAsync` documents today. The extension path is in-process, so its failure is `IoException`.

`DnsLookup`, PDO, and user `OffloadJob` classes stay as they are. HTTP, sockets, Redis, Postgres, MySQL, processes, and files stay in PHP.

## Docs

After the APIs exist, update the docs and the AI guide so a PHP caller sees the same `fn (...) => ...` argument, and so the install line is `pie install tyhp/ext-tyhp`. Describe the current contract. The extension is a faster path for the same result. A missing module is silent.

## Tests

Run each package suite twice where the new branch exists: module loaded, and module absent (`php -n`, or a process that does not load `tyhp`).

- `Type::is` and `Type::parse`: the same values, including a node the extension declines.
- `Generic::bind`: a static binder hits the cache, `T::int` and `Type::int()` share it, an instance binder does not, and a one-shot binder still throws `\LogicException` on the second call.
- `Json::decode`: the same decoded struct or the same `\JsonException` / `IncompatibleTypeException`.
- `fromClosure`: the opcode path and the source path build trees that evaluate equally, on PHP 8.2 through 8.5, with opcache on and off. A rejected body throws on both paths. A property path rejects an index.
- Compiler fixtures: an inline `fn` in `.tyhp` is still emitted as a fluent tree. A PHP-shaped signature in the fixture contains the `\Closure` union and the prologue.
- `Dns::resolveAsync`: the same addresses as `DnsLookup::run`. Cancellation rejects with `OperationCancelledException` and does not apply a late result. With the module absent, the call still goes through `Offload`.

## Done

This plan is done when those suites agree with and without the module, the three packages suggest `ext-tyhp` and do not require it, and `tyhp build` of an expression-tree call still emits one PHP file that runs on 8.2 either way.
