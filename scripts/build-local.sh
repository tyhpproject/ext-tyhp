#!/bin/bash
# Build and test the tyhp extension on each installed Homebrew PHP.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

php_configs=(
  /opt/homebrew/opt/php@8.2/bin/php-config
  /opt/homebrew/opt/php@8.3/bin/php-config
  /opt/homebrew/opt/php@8.4/bin/php-config
  /opt/homebrew/opt/php@8.5/bin/php-config
)

clean_build() {
  local phpize_bin="${1:-}"
  if [[ -f Makefile ]]; then
    make distclean
  fi
  if [[ -n "$phpize_bin" && -x "$phpize_bin" ]]; then
    "$phpize_bin" --clean
  fi
  rm -rf autom4te.cache build .libs modules src/.libs
  rm -f config.cache config.log config.status config.nice config.h config.h.in \
    configure configure.ac libtool Makefile Makefile.fragments Makefile.objects \
    run-tests.php run-tests.log tmp-php.ini confdefs.h stamp-h1 libphp.la \
    src/*.lo src/*.o src/*.dep src/*.la
  # Older phpize versions leave empty include/ and main/ directories behind.
  # rmdir only removes them while empty, so real sources are never touched.
  rmdir include main 2>/dev/null || true
}

previous_phpize=""
tested=0

for php_config in "${php_configs[@]}"; do
  if [[ ! -e "$php_config" ]]; then
    echo "skip: $php_config is missing"
    continue
  fi
  if [[ ! -x "$php_config" ]]; then
    echo "skip: $php_config is not executable"
    continue
  fi

  bindir="$(dirname "$php_config")"
  phpize_bin="${bindir}/phpize"
  php_bin="${bindir}/php"
  if [[ ! -x "$phpize_bin" ]]; then
    echo "skip: phpize is missing at $phpize_bin"
    continue
  fi

  version="$("$php_config" --version)"
  echo "==== PHP ${version} (${php_config}) ===="

  clean_build "$previous_phpize"

  "$phpize_bin"
  ./configure --with-php-config="$php_config"
  make

  echo "---- PHP ${version} opcache off ----"
  NO_INTERACTION=1 REPORT_EXIT_STATUS=1 make test

  # opcache.file_update_protection=0: opcache neither caches nor optimizes a
  # script modified in the last two seconds, and run-tests.php writes each
  # .phpt script just before running it. Without this flag the "opcache on"
  # run never optimizes the closures under test.
  ext_dir="$("$php_config" --extension-dir)"
  php_api="$(basename "$ext_dir")"
  prefix="$("$php_config" --prefix)"
  opcache_so="${prefix}/lib/php/${php_api}/opcache.so"
  if [[ -f "$opcache_so" ]]; then
    echo "---- PHP ${version} opcache on (${opcache_so}) ----"
    TEST_PHP_ARGS="-d zend_extension=${opcache_so} -d opcache.enable=1 -d opcache.enable_cli=1 -d opcache.file_update_protection=0" \
      NO_INTERACTION=1 REPORT_EXIT_STATUS=1 make test
  else
    # Some Homebrew builds (PHP 8.5) compile opcache into the binary.
    # make test passes -n, which leaves that extension registered and
    # opcache.enable_cli off, so these flags turn the optimizer on.
    echo "---- PHP ${version} opcache on (built-in) ----"
    TEST_PHP_ARGS="-d opcache.enable=1 -d opcache.enable_cli=1 -d opcache.file_update_protection=0" \
      NO_INTERACTION=1 REPORT_EXIT_STATUS=1 make test
  fi

  "$php_bin" -n -d "extension_dir=${root}/modules" -d "extension=tyhp.so" -r \
    'printf("RESULT php=%s extension_loaded=%s phpversion=%s\n", PHP_VERSION, extension_loaded("tyhp") ? "true" : "false", phpversion("tyhp"));'

  previous_phpize="$phpize_bin"
  tested=$((tested + 1))
done

if [[ "$tested" -eq 0 ]]; then
  echo "no Homebrew PHP builds were tested" >&2
  exit 1
fi

clean_build "$previous_phpize"
echo "tested ${tested} PHP builds"
