# ext-tyhp

Optional PHP extension for the [Tyhp](https://github.com/tyhpproject/tyhp) runtime. The module name is `tyhp`.

`extension_loaded('tyhp')` detects it. `phpversion('tyhp')` reads the module version (`0.1.0`). It needs PHP 8.2 or newer. No Tyhp package requires it. When it is absent, Tyhp keeps working, and `tyhp build` emits the same PHP.

The design is in [DESIGN.md](DESIGN.md). Licensed under the [Apache License 2.0](LICENSE.txt).

## Install

The Packagist name is `tyhp/ext-tyhp`. The Composer type is `php-ext`. Install it with PIE, not `composer require`:

```bash
pie install tyhp/ext-tyhp
```

## Build matrix

The build matrix is the GitHub Actions workflows [`.github/workflows/unix.yml`](.github/workflows/unix.yml) and [`.github/workflows/windows.yml`](.github/workflows/windows.yml).

Unix builds macOS arm64 (NTS) and Linux glibc and musl on x64 and arm64 (NTS) for PHP 8.2, 8.3, 8.4, and 8.5, plus Linux glibc x64 ZTS for PHP 8.5. Windows builds PHP 8.2 through 8.5 for x64 and x86, NTS and ZTS, and runs the tests with opcache off and on.

macOS x64 is not a prebuilt target. Intel Macs compile from source with PIE.

## API

`\Tyhp\Ext` exposes these static methods:

- `\Tyhp\Ext::typeIs(object $type, mixed $value): ?bool` — `null` means this build declines the node.
- `\Tyhp\Ext::typeParse(string $spelling): ?array` — `null` means this build declines the spelling.
- `\Tyhp\Ext::bindGet(mixed $target, array $typeArguments): ?\Closure` and `\Tyhp\Ext::bindPut(mixed $target, array $typeArguments, \Closure $bound): void` keep an intern map. Instance methods are not stored.
- `\Tyhp\Ext::closureTree(\Closure $fn, string $mode): array` — `$mode` is `expression` or `property-path`. An opcode it does not accept throws `\InvalidArgumentException`.
- `\Tyhp\Ext::dnsStart(string $host): array`, `\Tyhp\Ext::dnsTake(int $id): array`, and `\Tyhp\Ext::dnsCancel(int $id): void` run DNS on a side thread. The thread does not enter Zend.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).
