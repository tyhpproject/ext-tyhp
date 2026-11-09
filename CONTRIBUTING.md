# Contributing to ext-tyhp

Issues and pull requests are welcome. Read this before opening either.

## Legal

You will need to complete a Contributor License Agreement (CLA). Briefly, this agreement testifies that you are granting us permission to use the submitted change according to the terms of the project's license, and that the work being submitted is under appropriate copyright. Upon submitting a pull request, you will automatically be given instructions on how to sign the CLA.

This extension is under the [Apache License 2.0](LICENSE.txt).

## Build and test

`scripts/build-local.sh` builds the extension and runs `make test` on each installed Homebrew PHP 8.2, 8.3, 8.4, and 8.5, once with opcache off and once with opcache on. It looks for those builds at `/opt/homebrew/opt/php@8.2` through `php@8.5`, skips a version whose `php-config` is missing, and exits if none were tested.

```bash
scripts/build-local.sh
```

Tests are `.phpt` files in `tests/`.

Windows and the rest of the matrix are the GitHub Actions workflows [`.github/workflows/windows.yml`](.github/workflows/windows.yml) and [`.github/workflows/unix.yml`](.github/workflows/unix.yml). Unix covers macOS arm64 (NTS, PHP 8.2–8.5), Linux glibc and musl on x64 and arm64 (NTS, PHP 8.2–8.5), and Linux glibc x64 ZTS on PHP 8.5. Windows covers PHP 8.2–8.5 on x64 and x86, NTS and ZTS, with opcache off and on. macOS x64 is not in that matrix.

## Pull requests

- Target `main`.
- Keep changes focused. Do not mix an unrelated refactor with a bug fix.
- Add or update a `.phpt` test under `tests/` when behavior changes.

## Code of conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

Contributions are under the [Apache License 2.0](LICENSE.txt).
