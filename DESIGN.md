# ext-tyhp

`ext-tyhp` is an optional PHP extension. It speeds up a few Tyhp runtime operations, and it runs a few blocking C calls beside the event loop. Tyhp does not require it, and `tyhp build` does not emit different PHP when it is installed.

The module name is `tyhp`. Userland detects it with `extension_loaded('tyhp')` and reads its version with `phpversion('tyhp')`. The PHP runtime packages keep a complete implementation of every operation below. When the extension is loaded and new enough, those packages call into it. When it is absent, old, or declines a particular value, the PHP implementation runs and the result is the same.

This is the same idea as `tyhp/decimal` choosing `ext-decimal` when it is present, with one difference: decimal's pure-PHP backend can lose precision, so that package warns. `ext-tyhp` only changes speed. A missing extension is silent.

Packages suggest `ext-tyhp`. They do not require it. Shared hosts and locked PHP images are the common case, and they must keep working.

## Contract

- Emitted PHP is legal PHP 8.2 or newer, the same floor Tyhp already targets. It runs unchanged with or without the extension.
- The compiler keeps producing that PHP. The extension is not a second parser and it is not an alternate emit target.
- Each accelerated operation has one PHP entry point. Callers do not branch on the extension themselves.
- Userland checks `extension_loaded('tyhp')` once per process and remembers the result.
- If `phpversion('tyhp')` is older than the runtime package expects, the package ignores the extension for that process.
- An entry point may also decline a single value it does not understand (a newer type-graph node, for example). That call falls back to PHP. Other calls may still use the extension.
- Behavior matches the PHP implementation. A different result is a bug in the extension, including which exception is thrown.
- Opcache stores opcodes, not a side table filled during compile. Anything the extension learns while a file is compiling has to be written into that file's opcodes. On a cache hit the compile hook does not run again.

The PHP floor and the minors Tyhp still emits (8.2 through the current supported minor, ZTS and non-ZTS, macOS, Linux, and Windows) are why the surface stays small. AST layouts move between minors. A short list of runtime helpers can track that. A second compiler cannot.

## Recommended functionality

Four operations make the existing runtime faster, and one runs a blocking libc call beside the event loop. In each case the PHP function already exists or is a small addition with a PHP fallback, and the extension replaces the expensive part.

### Type checks

`\Tyhp\Type::is` and `\Tyhp\Type::parse`, including the struct, generic, union, and alias walks those functions perform.

The compiler erases hints, so these checks remain at runtime for `is`, JSON decoding, and generic binders. The C matcher walks the same `\Tyhp\Type` objects the PHP matcher walks. It does not invent a second type format. A node shape it does not know is a per-call fallback.

### JSON decode

`Json::decode` checks a decoded value against a `Type`. Large payloads are where a C walker pays for itself. Same function, same exceptions (`\JsonException` for invalid JSON, `\Tyhp\Exceptions\IncompatibleTypeException` when the value does not match). The extension handles the match walk. PHP still owns the public entry point.

### Generic bind

Story 49 emits two layouts, and `Generic::bind` stays the path for a layout-1 library, a class or callable chosen at runtime, and an `#[\Tyhp\InferGeneric]` method when the caller passes explicit types. A binder-first call the compiler can see is emitted as the call itself and does not go through `Generic::bind`:

```php
Json::decode(T::struct(['id' => T::int, 'name' => T::string]))($json);
$box = Box::new(T::int)(42);
```

Layout 2 is that shape. The declared method is the binder: its parameters are the type arguments (`\Tyhp\TypeLike|string|null`, normalized with `Type::arg`), and it returns the value closure. No `decode__tyhpGeneric` is emitted. The stamp is `#[\Tyhp\GenericRuntime(erased: false, binder: "decode", layouts: [2])]`.

Layout 1 is the older pair, and it is what `#[\Tyhp\InferGeneric]` still emits: the plain method, plus a `name__tyhpGeneric` companion. `Promise::all($promises)` is the plain call. `Generic::bind(Promise::resolved(...), T::int)(1)` uses the companion. Erased generics stay plain methods and are not specialized.

The extension accelerates `Generic::bind` for both layouts. It reads the stamp, normalizes each argument the way `Type::arg` does (`Type`, `BuiltIn`, class-string), and interns the bound closure for the process. The cache key is the target plus those normalized types, so `T::int` and `Type::int()` share an entry. Layout 2 calls the declared method. Layout 1 calls the companion. Static methods and functions may be cached. Instance methods may not: the closure captures `$this`. A one-shot binder still throws the same `\LogicException` on a second `new()` or `__invoke`.

`Box<int>` is still the class `Box` with a `Type` attached. Null binder arguments still infer on every value call, inside the closure Story 49 emits. The cache stores that closure. It does not skip the inference.

### Closures passed where Tyhp passes a tree

A Tyhp parameter typed `Expression<…>` or `PropertyPath<…>` still requires an inline `fn` in `.tyhp`, and the compiler still rewrites that argument into a tree before the call is emitted. The body of the function sees the tree. A closure variable at a Tyhp call site stays an error.

The emitted PHP signature of that parameter also accepts `\Closure`. The compiler inserts a prologue so a closure becomes a tree before the rest of the body runs. PHP callers write the same argument Tyhp callers write:

```php
$adults = $query->where(fn ($u) => $u->age > $minAge);
$names = $query->select(fn ($u) => $u->firstName);
$author = $query->orderBy(fn ($u) => $u->author->name);
```

The prologue the compiler emits is the whole runtime check:

```php
public function where(Expression|\Closure $predicate): static
{
    if ($predicate instanceof \Closure) {
        $predicate = \Tyhp\Expression::fromClosure($predicate, /* expected callable shape */);
    }
    // $predicate is an Expression from here
}
```

A `PropertyPath` parameter calls `PropertyPath::fromClosure` instead, and a body that is not a property chain fails there. The expected shape in the prologue comes from the Tyhp parameter type, so `fn ($u) => …` with no parameter type still records `User` for `Expression<callable(User): bool>`. A Tyhp-compiled call passes a tree and skips `fromClosure`.

`fromClosure` reads the closure's opcodes when the extension is loaded. The op_array is what remains after the AST is freed, including under opcache, and the extension does not have to have been loaded when the file was compiled. Without the extension, `tyhp/lambda` tokenizes the closure's source span. That source parse is the PHP-only path.

A property path is a chain of property fetches from the parameter, including `?->`. On PHP 8.5, `fn ($u) => $u->author->name` is `RECV`, two `FETCH_OBJ_R`s, and `RETURN`. `fn ($u) => $u?->author?->name` puts `JMP_NULL` around each fetch. An index (`$u->tags[0]`, opcode `FETCH_DIM_R`) is a valid expression and not a property path, so `PropertyPath::fromClosure` throws.

Expressions are the same kind of walk over a whitelist. `&&` is `JMPZ_EX`, `||` is `JMPNZ_EX`, `??` is `COALESCE`, `?:` is `JMP_SET`, and a ternary is `JMPZ` plus `QM_ASSIGN`. Arithmetic is `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `CONCAT`. A nested arrow function is `DECLARE_LAMBDA_FUNCTION`; the child op_array is walked the same way, and a `function () {}` child throws. Captured values such as `$minAge` are the closure's bound variables. Method and static calls are `INIT_METHOD_CALL` or `INIT_STATIC_METHOD_CALL`, then sends, then `DO_FCALL`. `new` is `NEW` plus the same sends.

The compiler and the opcache optimizer do not emit one shape. The same scenarios on PHP 8.2.29, 8.3.36, 8.4.27, and 8.5.12 produced the same opcodes, before the opcache optimizer and after it. From 8.4 the dump names a closure `{closure:valid():13}` instead of `{closure}`. That name is not an opcode. The walker accepts these forms and throws on any other opcode:

- `a > b` and `a >= b` are `IS_SMALLER` and `IS_SMALLER_OR_EQUAL` with the operands swapped, so `a < b` and `b > a` are the same opcode. When an op_array is linked, the VM also reorders the operands of `==`, `!=`, `===`, `!==`, `*`, `&`, `|`, `^`, and `xor` so that the operand with the higher type comes first (constant, then temporary, then variable, then compiled variable), so `1 == $x` and `$x == 1` are the same opcode. `<=>` and every other binary operator keep source order. `<>` compiles to the same `IS_NOT_EQUAL` as `!=`. `and` / `or` compile to the same jumps as `&&` / `||`.
- Source order is recovered from evaluation order. When both operands are computed values, the one produced first was written first, so `$u->a < $u->b` and `$u->b > $u->a` come back as written. When one operand is a literal, a variable, or a capture, the opcodes do not say which side it was written on, and the tree puts the computed operand on the left and the literal on the right. `1 < $u->a` is therefore returned as `$u->a > 1`.
- `!== true`, `!== false`, and `!== null` are `TYPE_CHECK` with the inverse mask, and `=== true`, `=== false`, and `=== null` are `TYPE_CHECK` with a single bit.
- A closure whose only `RETURN` is the implicit one at the end of the body (`function () {}`) is rejected. An arrow function ends in an explicit `RETURN`, so `fn () => null` is accepted.
- The optimizer can reshape a body further: it threads the jumps of `&&` / `||` mixes, drops the `QM_ASSIGN` that ends each arm of a ternary, inlines a call to a static method that does nothing, and, for operands it knows to be integers, reorders `+` and rewrites `2 * $x` as `$x + $x`. The walker reads what the opcodes say and rejects a body it cannot reconstruct.
- `1 + 2` is already `3`. `Foo::NAME` and `Foo::class` become the literal when the compiler can see them. Unary `-` is `MUL` by `-1`, and unary `+` is `MUL` by `1`. `(bool)` is `BOOL`, while `(int)` is `CAST`. `=== true` and `=== null` are `TYPE_CHECK`.
- Before the optimizer, a call argument is `FETCH_OBJ_FUNC_ARG` and `SEND_FUNC_ARG`. After it, the same call is `FETCH_OBJ_R` and `SEND_VAR`. Unoptimized closures also end with a dead `RETURN null`.
- A name that is not a string literal (`$u->$m`) is `FETCH_OBJ_R` from a variable. A named argument is a send whose operand is the name. Unpacking is `SEND_UNPACK`. A first-class callable is `CALLABLE_CONVERT`. Array spread is `ADD_ARRAY_UNPACK`. Assignment, `++`, `isset`, `empty`, `clone`, `@`, `include`, `throw`, `match`, and `$u::class` each have their own opcode. `strlen($u->name)` is the `STRLEN` opcode, not a call. None of these are on the whitelist, so `fromClosure` throws.

`.tyhp` files are not parsed again. Their trees already carry the types the checker proved, including generic property types such as `array<Post>`.

### Blocking libc calls from `tyhp/io`

When a blocking `tyhp/io` operation is a straight C call, `tyhp/io` uses `ext-tyhp` if it is loaded and falls back to today's `Offload` path if it is not. `Offload` still means a `tyhp/parallel` worker when the pool is available, and the loop thread when it is not. The extension comes first, ahead of the worker, because the worker is the expensive way to make that call.

The libc call takes the same time either way. A DNS lookup does not resolve faster on a thread than in a worker. What gets cheaper is everything wrapped around it. The worker holds a PHP process for the whole wait: serialize the job, write a socket frame, run PHP, serialize the result, write it back. The extension copies the inputs onto a thread, calls libc, and wakes `EventLoop` with one byte on a pipe. The worker stays free for real PHP tasks, and the loop keeps running when the pool is missing or busy.

`Dns::resolveAsync` is the only `Offload` job that qualifies today. The lookup is `gethostbynamel` and `dns_get_record`. The thread uses `malloc`, never `emalloc`, and never calls a PHP function or touches a zval, so this is safe on the usual non-thread-safe (NTS) build. The main thread, back inside `stream_select`, reads the addresses and fulfills the promise.

The addresses, and the `IoException` when nothing resolves, match the PHP lookup. A cancelled token rejects the promise with `OperationCancelledException` on the main thread. The thread itself cannot be killed. The lookup may finish afterward, and that result is discarded. A resolver call that never returns stays running until libc returns.

The other `tyhp/io` clients stay in PHP. HTTP (`curl_multi`), sockets, Redis, Postgres (libpq polling), MySQL statements (`MYSQLI_ASYNC`), child processes (`proc_open` pipes), and files (256 KiB chunks with a yield between them) are already waiting inside C, on a file descriptor `EventLoop` polls. Another copy of those clients in this extension would not shorten that wait. `tyhp/async` keeps the fiber scheduler.

PDO stays on the worker. `PdoJob` runs the PDO extension and caches connections there. That is engine state, and the side thread cannot enter the Zend engine. A user `OffloadJob` stays on the worker for the same reason: its `run()` is PHP. MySQL's connect handshake blocks today and stays in `mysqli`.

The pool serves the long-running CLI loop. Results are applied on the main thread. On shutdown the pool stops taking work and joins its threads, and it does not write into request memory after that request is gone.

More libc calls can join this pool later, one at a time, each with `Offload` as the fallback. A call qualifies when today's path either blocks the loop or occupies a PHP worker for a function C can run itself, and that function is safe off the main thread.

## What the compiler keeps

These stay in `tyhp build`. The extension never redoes them, which is why the emitted file has one meaning on every PHP install:

- Parsing `.tyhp`, including generics, `operator`, `extension`, `async`, and expression-tree rewriting.
- Lexical `use extension` and `global use extension`.
- Splicing a one-line extension method into the caller, and emitting a static call for a longer one.
- Rewriting operators to the reserved methods (`__add`, and the rest).
- Emitting PHP that runs on 8.2+ with no extension.

## Where the work lives

| Piece | Home |
| --- | --- |
| C extension, C matcher, bind cache, closure opcode walker behind `fromClosure` | this repo |
| DNS lookup thread and the pipe that wakes `EventLoop` | this repo, called from `tyhp/io` |
| PHP fallback, process-lifetime dispatch, version gate, source tokenizer behind `fromClosure` | `tyhp/core` and `tyhp/lambda` |
| `Expression\|\Closure` and `PropertyPath\|\Closure` hints, and the prologue that calls `fromClosure` | the Tyhp compiler |
| Fiber scheduler; HTTP, sockets, Redis, Postgres, MySQL, processes, files; PDO and other PHP offload jobs | `tyhp/async`, `tyhp/io`, `tyhp/parallel` |
| `.tyhp` parsing, and the compile-time rewrite of an inline `fn` into a tree | the Tyhp compiler |

`fromClosure` lives in `tyhp/lambda`. With the extension it walks opcodes. Without the extension it tokenizes the closure's source. The compiler emits the widened hint and the prologue; this repo does not. An opcode outside the whitelist throws, and the PHP caller uses `_expr` or `_path`.

## First build

`IMPLEMENTATION_PLAN.md` builds this extension. It does not edit the compiler or the runtime packages. `IMPLEMENTATION_PLAN_TYHP.md` wires those after the extension's API is in place. The product order is:

1. Module entry, version reported through `phpversion('tyhp')`, and a userland dispatch flag in `tyhp/core`.
2. Type matcher behind `Type::is` and `Type::parse`.
3. `Generic::bind` intern cache.
4. JSON decode's match walk.
5. Closure opcode walker behind `Expression::fromClosure` and `PropertyPath::fromClosure`, with the source tokenizer in `tyhp/lambda` when the extension is absent. Checked on each PHP minor Tyhp supports, including before and after the opcache optimizer. An opcode outside the whitelist throws.
6. DNS lookup thread in `tyhp/io`. When the extension is loaded this is preferred over `Offload`, including when a worker pool is up. `Offload` remains the fallback.

## Later, only if a profile says so

`Expression::hydrate($blob, $fn)`. The compiler would emit a compact blob beside the fluent chain's backing closure, PHP would unpack it, and the extension would unpack it in C. That is deserialization of a tree the compiler already built. It is worth doing only if the fluent chain shows up as hot. It is not a parser.

Two related ideas are developer tools, not part of this helper, and can be separate projects if they are ever built. A dev-only assignment hook that checks erased generic properties from `#[\Tyhp\PropertyType]` changes when failures appear, so it does not belong in a silent accelerator. A source map that points stack traces at `.tyhp` lines can start as a userland exception handler; debugger stepping is a different problem.

## Rejected

### Operators in the VM

Class-owned `operator +` could be dispatched by PHP 8's object handler `do_operation`, and an opcode handler could catch a scalar on the left (`$n + $money`). That hook runs only if the file still contains `+`.

The compiler emits `\Money::__add($a, $b)`. That is a direct static call, and it is cheaper than a hook on every addition in the process. Leaving the raw `+` in the file so a hook can see it makes stock PHP fatal on `Money + Money`. Supporting both installs would mean two emit modes. The default mode would still be today's rewrite, and this extension would do nothing for code compiled that way.

Extension operators are also lexical. They apply in the file that declares them, in a file that names them with `use extension`, and everywhere only after `global use extension`. An opcode hook is process-wide. It cannot see that this file left the extension inactive, and it would change `+` for any plain PHP that used the same class.

### Extension methods on scalars and on objects the program does not own

`$s->length()` parses. Stock PHP then throws "call to a member function on string." An AST rewrite can turn that into a function call. Methods added to a class from outside it require patching that class for the whole process.

Compiled Tyhp already lowers these per file. A `=>` method is spliced into the caller and costs nothing at runtime. A longer method becomes a static call such as `\MoneyFormatting::format($money, 'USD')`. Both follow `use extension`. Emitting `$s->length()` would fatal without this extension, so it cannot be the default output. A process-wide registry of scalar methods would also ignore `use extension`.

### Reified generics, and angle brackets in `.php` files

An extension cannot add a zval type tag, and the PHP parser does not grow new syntax from an extension. `Box<int>` and `foo<int>($x)` are tokenized as comparisons before any extension hook runs. Defining a class per instantiation (`Box@int`) breaks `instanceof Box` and opcache.

A preprocessor installed in front of `zend_compile_file` could rewrite angle brackets before parse. That hook is also where opcache installs itself, and the preprocessor would have to leave real comparisons (`$a < $b`) alone. That is a second Tyhp parser, written in C, for a call shape PHP can already express as `Box::new(T::int)(42)`.

### A struct zval, and a decimal implementation

Structs compile to arrays. Reading a field is a hash lookup on a copy-on-write array. An object stand-in would be slower, and it would still be an object, because extensions cannot add zval types.

`tyhp/decimal` already picks `ext-decimal`, then `ext-bcmath`, then `ext-gmp`, then pure PHP. Decimal arithmetic stays on that ladder.

### A C implementation of async

`tyhp/async` schedules work on PHP fibers. A C event loop would be a second scheduler next to that one, and next to whatever PHP adds later. The extension does not replace `_async` or `_await`.

The DNS thread above wakes this same loop through a pipe. It does not run fibers. PHP tasks stay on `tyhp/parallel`. Booting a Zend executor on a side thread would require a thread-safe PHP build, would duplicate `ext-parallel`, and a crash on that thread would take the loop process with it. `ext-tyhp` does not boot a second PHP executor.

### A C autoloader

With opcache warm, the remaining autoload cost is the callback and the classmap. Preload removes that cost for the code a project marks as hot. A C classmap would have to follow every package layout and would buy little over Composer's static loader.

### Parsing `.tyhp` lambdas a second time

The compiler builds expression trees from the Tyhp AST, with the types of parameters and properties already attached. Tokenizing or re-parsing the emitted closure drops those types and has to stay aligned with the checker forever. `Expression::capture` exists for `.php` files the compiler never saw. Trees that started as `.tyhp` stay on the fluent chain.
