# Changelog

Notable changes to the optional `tyhp` PHP extension (`tyhp/ext-tyhp`). The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.1.0] - 2026-11-09

### Added

- The `tyhp` module, version `0.1.0`, for PHP 8.2 and newer. `extension_loaded('tyhp')` detects it and `phpversion('tyhp')` reads the version. No Tyhp package requires it. When it is absent, Tyhp keeps working, and `tyhp build` emits the same PHP.
- `\Tyhp\Ext::typeIs(object $type, mixed $value): ?bool`. `null` means this build declines the node.
- `\Tyhp\Ext::typeParse(string $spelling): ?array`. `null` means this build declines the spelling.
- `\Tyhp\Ext::bindGet(mixed $target, array $typeArguments): ?\Closure` and `\Tyhp\Ext::bindPut(mixed $target, array $typeArguments, \Closure $bound): void`. They keep an intern map. Instance methods are not stored.
- `\Tyhp\Ext::closureTree(\Closure $fn, string $mode): array`. `$mode` is `expression` or `property-path`. An opcode it does not accept throws `\InvalidArgumentException`.
- `\Tyhp\Ext::dnsStart(string $host): array`, `\Tyhp\Ext::dnsTake(int $id): array`, and `\Tyhp\Ext::dnsCancel(int $id): void`. Lookups run on a side thread that does not enter Zend.
