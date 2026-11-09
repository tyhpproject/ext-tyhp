# ext-tyhp implementation plan

This plan builds the extension in this repository. It does not edit the Tyhp compiler or any `tyhp/*` package. Those edits are `IMPLEMENTATION_PLAN_TYHP.md`, and they start after the API below is implemented and the tests here pass.

The behavior this extension has to match is `DESIGN.md`. This file is the order of work.

## What this repository ships

A PHP module named `tyhp`. `extension_loaded('tyhp')` is true once it is loaded. `phpversion('tyhp')` returns this extension's version, a semver string starting at `0.1.0`, independent of the `tyhp/core` version.

The module exports the functions below. They are the whole surface. Packages will call them later. Tests in this repository call them directly and do not load `tyhp/core`, `tyhp/lambda`, or `tyhp/io`.

| Function | Contract |
| --- | --- |
| `\Tyhp\Ext::typeIs(object $type, mixed $value): ?bool` | Walks a `\Tyhp\Type` object the way `Type::is` does. `true` or `false` is the answer. `null` means this build does not understand that node, and the caller uses its PHP walk. |
| `\Tyhp\Ext::typeParse(string $spelling): ?array` | Parses a type spelling into an intermediate graph. `null` means this build does not understand the spelling. The caller builds the `\Tyhp\Type`. |
| `\Tyhp\Ext::bindGet(mixed $target, array $typeArguments): ?\Closure` | Returns the interned closure for a static method or function. `null` on a miss, for an instance method, and when an argument cannot be normalized. |
| `\Tyhp\Ext::bindPut(mixed $target, array $typeArguments, \Closure $bound): void` | Stores a closure under the same key `bindGet` uses. No-op for an instance method or an argument that cannot be normalized. |
| `\Tyhp\Ext::closureTree(\Closure $fn, string $mode): array` | `$mode` is `expression` or `property-path`. Returns the intermediate tree below. Throws `\InvalidArgumentException` for an opcode outside the whitelist, for a `property-path` body that is not a property chain, and for a nested `function () {}`. |
| `\Tyhp\Ext::dnsStart(string $host): array` | Starts the lookup on a side thread. Returns `['id' => int, 'pipe' => resource]`. The pipe becomes readable when the thread has finished. The thread uses `malloc`, never `emalloc`, and never calls into Zend. |
| `\Tyhp\Ext::dnsTake(int $id): array` | After the pipe is readable: `['ok' => true, 'addresses' => list<string>]` or `['ok' => false, 'reason' => 'unresolved' \| 'cancelled', 'message' => string]`. The unresolved message matches `DnsLookup`: `Could not resolve host <host>.` |
| `\Tyhp\Ext::dnsCancel(int $id): void` | The next `dnsTake` reports `cancelled`. The thread is not killed. A result that arrives later is discarded. |

`typeIs` reads the private fields of a `Tyhp\Type` instance by name (`kind`, `name`, `typeArgs`, `isNullable`, `structFields`, `structRequiredKeys`, `forward`, `lazyInner`, and the fields `follow()` needs). The test suite defines its own `Tyhp\Type` with those fields. It is a stand-in so this repository can build the matcher before `tyhp/core` is involved. The real class uses the same field names.

`closureTree` does not construct `Expression` or `PropertyPath` objects and does not apply the expected callable shape. Those stay in `tyhp/lambda`. The array it returns uses these `kind` values: `param`, `const`, `capture`, `prop`, `nullsafe_prop`, `index`, `binary`, `unary`, `ternary`, `coalesce`, `cast`, `instanceof`, `new`, `call`, `nullsafe_call`, `static_call`, `array`, `lambda`. A `capture` node carries the bound value copied at the call. A binary operator is returned in source order when both operands are computed (`$u->a > $u->b` and `$u->b < $u->a` each keep their written sides), including when the compiler or the VM stored the operands swapped; when one operand is a literal or capture, the computed operand is the left one (`1 < $u->a` is `$u->a > 1`). Constant folding stays folded: `1 + 2` is a `const` of `3`, unary `-` is multiply by `-1`, and a visible `Foo::NAME` or `Foo::class` is the literal. `<>` and `!=` are both `!=`. `and` / `or` are `&&` / `||`.

`bindGet` and `bindPut` are an intern map. They do not call a binder. The key is the target plus each argument normalized the way `Type::arg` normalizes `Type`, `BuiltIn`, and a class-string, so `T::int` and `Type::int()` share an entry. An instance method is not stored, because that closure captures `$this`. A one-shot binder's `\LogicException` stays inside the closure the PHP binder created. The map returns that same closure.

JSON decoding is not a separate C entry point. `Json::decode` already checks the value with `Type::is`. `typeIs` is that walk. The tests include a struct, a generic, a union, and an alias large enough to stand in for a decoded payload.

## Layout

```text
config.m4
config.w32             Windows configure script, required by the Windows builder
composer.json          PIE package metadata, not a library people composer-require
src/                   module entry, type walk, bind key, opcode walk, DNS thread
tests/                 .phpt files, plus the stand-in Type class
scripts/build-local.sh loops the PHP builds this machine can load
.github/workflows/     the release matrix, added when the API tests pass locally
```

`config.m4` builds a normal PHP module. There are no bundled libraries. DNS uses libc (`getaddrinfo` / the same lookups `gethostbynamel` and `dns_get_record` use) and pthreads. On Windows the thread uses the Win32 equivalents and still returns the IPv4 list followed by IPv6, matching `DnsLookup::run`.

`support-zts` and `support-nts` are both true. The side thread's rule is the same in both builds: it does not enter the Zend executor.

## Steps

1. **Module entry.** `zend_module_entry` name `tyhp`, version `0.1.0`. A `.phpt` asserts `extension_loaded('tyhp')` and `phpversion('tyhp')`. No ini settings.

2. **`typeIs`.** Implement the kinds `Type::is` walks: scalar, class, union, intersection, generic, struct, alias, nullable, and the forward / lazy follow. An unrecognized `kind` returns `null` rather than guessing. Tests cover a matching value, a failing value, and one unknown kind. Include the struct-shaped payload case in this step so the JSON walk is covered.

3. **`typeParse` and the bind map.** `typeParse` returns a graph for the spellings `Type::parse` accepts (scalars, nullable, union, generic, struct, alias, class-string) and `null` for a spelling it does not know. `bindPut` then `bindGet` returns the same closure. `T::int` and `Type::int()` hit one slot. An instance-method target is not stored, and `bindGet` stays `null` for it. An unknown argument is not stored.

4. **`closureTree`.** Walk the closure's `op_array`, including a nested arrow function's child op_array. Accept the opcode shapes in `DESIGN.md`, including both call-argument forms (`FETCH_OBJ_FUNC_ARG` / `SEND_FUNC_ARG`, and `FETCH_OBJ_R` / `SEND_VAR`) and a trailing `RETURN null`. Property-path mode accepts only a chain of `prop` and `nullsafe_prop` from the parameter. Every other opcode throws `\InvalidArgumentException`. Run the suite twice: `php -n`, and PHP with opcache enabled on the CLI, on each minor below.

5. **DNS thread.** `dnsStart` returns immediately. A `.phpt` waits on the pipe with `stream_select`, then `dnsTake`. Compare the address list with a PHP oracle in the same test that calls `gethostbynamel` and `dns_get_record` the way `DnsLookup::run` does. A second test cancels before the result is taken and asserts `reason` is `cancelled`. A lookup of a name that does not resolve returns `unresolved` and the message above. The test does not load `tyhp/io`.

## PHP versions

The floor is PHP 8.2. The matrix is every minor Tyhp still emits:

| Minor | Windows compiler | Notes |
| --- | --- | --- |
| 8.2 | VS16 (Visual Studio 2019) | |
| 8.3 | VS16 | |
| 8.4 | VS17 (Visual Studio 2022) | Closure dump names gain the `{closure:fn():line}` label. The opcodes do not change. |
| 8.5 | VS17 | Same opcodes as 8.2 for the closure scenarios in `DESIGN.md`. |

A new minor is added when Tyhp supports it. PHP 8.1 is out of range. The module API number is per minor, so a binary built on 8.2.29 loads on a later 8.2 patch. Each minor still gets its own binary. NTS and ZTS are different binaries. Debug PHP is not a shipping target.

Windows arm64 is not a target. Official Windows PHP builds are x86 and x64.

## What this machine can build

This Mac is an Apple M3 Pro, arm64. Rosetta is not installed (`arch -x86_64` cannot run). Docker (OrbStack) can build `linux/arm64` and `linux/amd64`.

| Target | On this machine |
| --- | --- |
| macOS arm64, NTS, 8.2.29, 8.3.36, 8.4.27, 8.5.12 | Yes. Homebrew PHP is already installed. `scripts/build-local.sh` runs `phpize`, `./configure --with-php-config=…`, `make`, and `make test` once per binary under `/opt/homebrew/opt/php@8.*`. |
| macOS arm64, ZTS | Yes, after a ZTS PHP is compiled from source for that minor. Homebrew does not ship ZTS. |
| macOS x64, either thread mode | Not a prebuilt target. Intel Macs compile from source with PIE against their own PHP. No `macos-15-intel` job. |
| Linux glibc and musl, arm64 and x64, NTS | Yes, in Docker, using the official `php:8.x-cli` and `php:8.x-cli-alpine` images. amd64 is emulated. |
| Linux ZTS | Yes, in a container whose PHP was configured `--enable-zts`. The default official images are NTS. |
| Windows x86 and x64, NTS and ZTS | No. Official PHP loads an MSVC DLL (VS16 or VS17, matching the minor). A MinGW DLL built here will not load. Windows binaries come from CI. |

Day-to-day development is the four Homebrew NTS builds. One green `make test` on those four is the bar before opening a PR. ZTS, Linux, and Windows are the CI matrix, and a ZTS arm64 PHP can be built here when a ZTS failure has to be reproduced.

## Distribution

People get the extension through [PIE](https://github.com/php/pie), the installer that replaced PECL. PECL is not a target.

The Packagist package name is `tyhp/ext-tyhp`. `composer.json` in this repository sets `"type": "php-ext"` and:

```json
{
  "name": "tyhp/ext-tyhp",
  "type": "php-ext",
  "require": {
    "php": ">=8.2"
  },
  "php-ext": {
    "extension-name": "tyhp",
    "support-zts": true,
    "support-nts": true,
    "download-url-method": ["pre-packaged-binary", "composer-default"]
  }
}
```

`extension-name` has to be set. PIE would otherwise derive `ext-tyhp` from the package name, and that is not a legal module name. The loaded module stays `tyhp`.

`pie install tyhp/ext-tyhp` installs into the PHP that invoked PIE. On macOS and Linux, PIE uses a release asset when the platform matches and compiles from source when it does not (`composer-default`). On Windows, PIE does not compile. A Windows user needs the matching DLL asset or the install fails.

`composer require tyhp/ext-tyhp` does not enable the module. Composer downloads PHP packages into a project. It does not run `phpize` or edit `php.ini`. The `php-ext` type is for PIE. The virtual package name `ext-tyhp` in a project's `require` or `suggest` means "this PHP has the `tyhp` module loaded." It does not install that module. Suggesting `ext-tyhp` from the runtime packages is done in `IMPLEMENTATION_PLAN_TYHP.md`.

Prebuilt assets are attached to the GitHub release for the tag. PIE looks up Windows DLLs only on GitHub. Asset names follow the PIE version pinned in CI. As of the PIE 1.4 maintainer docs, Windows archives look like `php_tyhp-0.1.0-8.3-nts-vs16-x86_64.zip`, and non-Windows archives look like `php_tyhp-0.1.0_php8.3-arm64-linux-glibc.zip` (glibc or musl, darwin or linux, `nts` or `zts`). The workflow reads the naming section of the pinned PIE release before the first tagged build and uses those patterns. `php/pie-ext-binary-builder` produces the non-Windows assets for the cells this plan actually builds: macOS arm64, and Linux glibc and musl. There is no macOS x64 asset. Windows assets come from the workflow in the CI section.

libc is the only link dependency, so a prebuilt binary is appropriate. PIE warns against prebuilt binaries that need a third-party shared library. This extension does not.

A missing asset is a source install on macOS and Linux, which is why `composer-default` stays in the list. That path needs a compiler and the PHP headers. Shared hosts that cannot load a custom module keep working, because nothing requires the extension.

## CI

Two workflows. Neither builds macOS x64.

Unix cells, after the local Homebrew suite is green:

- macOS arm64, NTS, for 8.2 through 8.5, on a GitHub `macos` runner. This machine can do the same build.
- Linux glibc and musl, arm64 and x64, NTS, via the official PHP images.
- Linux ZTS for one minor on glibc x64, as a smoke test that the thread still stays out of Zend.

`php/pie-ext-binary-builder` uploads those assets on a release. Each cell runs `make test`.

### Windows

`.github/workflows/windows.yml` uses [`php/php-windows-builder`](https://github.com/php/php-windows-builder). That action runs on GitHub's `windows-2022` runners and picks VS16 or VS17 from the PHP version. PIE does not compile on Windows, so this workflow is the only Windows install path.

The matrix is fixed. Sixteen jobs: PHP 8.2, 8.3, 8.4, and 8.5, times `x64` and `x86`, times `nts` and `ts`. `extension-matrix` is given those lists so it does not infer older minors from `composer.json`.

`config.w32` enables the `tyhp` extension and lists the same sources as `config.m4`. The DNS thread is pthreads on Unix and a Win32 thread on Windows. `config.w32` links `ws2_32`. No extra libraries are passed to the action.

Pull requests and pushes to `main` build and run the `.phpt` suite with opcache off and on (`test-opcache-mode: both`). They do not upload. `fail-fast` is false, so one broken cell does not cancel the other fifteen.

A GitHub release runs the same matrix, then `php/php-windows-builder/release@v1` attaches each ZIP to that release. Asset names follow PIE: `php_tyhp-<tag>-<minor>-<nts|ts>-<vs16|vs17>-<x86_64|x86>.zip`, with the DLL inside named the same way. The action writes the compiler tag. If the repository uses immutable releases, the release is still a draft when the upload runs.

```yaml
name: Windows
on:
  pull_request:
  push:
    branches: [main]
  release:
    types: [published]
permissions:
  contents: write
jobs:
  get-extension-matrix:
    runs-on: ubuntu-latest
    outputs:
      matrix: ${{ steps.extension-matrix.outputs.matrix }}
    steps:
      - uses: actions/checkout@v5
      - id: extension-matrix
        uses: php/php-windows-builder/extension-matrix@v1
        with:
          php-version-list: '8.2, 8.3, 8.4, 8.5'
          arch-list: 'x64, x86'
          ts-list: 'nts, ts'
  build:
    needs: get-extension-matrix
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix: ${{ fromJson(needs.get-extension-matrix.outputs.matrix) }}
    steps:
      - uses: actions/checkout@v5
      - uses: php/php-windows-builder/extension@v1
        with:
          php-version: ${{ matrix.php-version }}
          arch: ${{ matrix.arch }}
          ts: ${{ matrix.ts }}
          args: --enable-tyhp
          run-tests: true
          test-opcache-mode: both
  release:
    if: github.event_name == 'release'
    needs: build
    runs-on: ubuntu-latest
    steps:
      - uses: php/php-windows-builder/release@v1
        with:
          release: ${{ github.event.release.tag_name }}
          token: ${{ secrets.GITHUB_TOKEN }}
```

The `args: --enable-tyhp` flag is required because `config.w32` defaults the extension off.

This file is added when the extension compiles and `make test` passes on the four Homebrew builds. Until then there is nothing for the action to configure.

## Done

This plan is done when:

- `make test` passes on the four Homebrew NTS builds on this machine, with and without opcache.
- `typeIs`, `typeParse`, `bindGet`, `bindPut`, `closureTree`, and the DNS functions match the contracts above.
- A tag build produces PIE-named assets for the Unix cells and for all sixteen Windows cells, and `pie install tyhp/ext-tyhp` loads `tyhp` on this Mac from that release. macOS x64 is installed from source.

Wiring `Type::is`, `Generic::bind`, `Json::decode`, `fromClosure`, the compiler prologue, and `Dns::resolveAsync` is the other plan.
