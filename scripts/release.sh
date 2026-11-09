#!/bin/bash
# Publish the GitHub Release for tyhpproject/ext-tyhp.
# The tag is the module version with no "v" prefix. Publishing the release
# starts the Windows and Unix workflows, which attach the PIE zip assets.
# Pushing main does not create a release.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly PROJECT_ROOT
readonly RELEASE_REPO="tyhpproject/ext-tyhp"
readonly VERSION_HEADER="${PROJECT_ROOT}/src/php_tyhp.h"
readonly CHANGELOG="${PROJECT_ROOT}/CHANGELOG.md"

DRY_RUN=0
NOTES_FILE=""

usage() {
  cat <<'EOF'
Usage: scripts/release.sh [options]

Publishes the GitHub Release for the version already written in
src/php_tyhp.h (PHP_TYHP_VERSION). The tag is that version with no "v"
prefix, for example 0.1.0. PIE uses the tag text in its zip names.

Pushing main runs the tests and does not create a release. This script
creates the release as published. The Windows and Unix workflows then
attach the zip assets. It does not upload those zips itself, and it does
not register the package on Packagist.

Before running it, move the [Unreleased] changelog entries under
## [X.Y.Z] - YYYY-MM-DD, leave an empty ## [Unreleased] above that, and
commit. The working tree must be clean, on main, and at origin/main.

To release a new version, change PHP_TYHP_VERSION, the version test, and
the changelog, commit, then run this script again.

Options:
  --dry-run   Check the tree, changelog, and tag, and print the release
              notes. Does not create a tag or a GitHub Release.
  -h, --help  Show this help.
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --dry-run)
        DRY_RUN=1
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "error: unknown argument: $1 (see --help)" >&2
        exit 1
        ;;
    esac
    shift
  done
}

cleanup() {
  if [[ -n "${NOTES_FILE}" && -f "${NOTES_FILE}" ]]; then
    rm -f "${NOTES_FILE}"
  fi
}

require_tool() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required tool: $1" >&2
    exit 1
  fi
}

module_version() {
  perl -ne 'if (/^#define PHP_TYHP_VERSION "([^"]+)"/) { print $1; exit }' "$VERSION_HEADER"
}

changelog_section() {
  local version="$1"
  VER="$version" perl -ne '
    if (index($_, "## [$ENV{VER}]") == 0) { $in = 1; next }
    if ($in && /^## /) { exit }
    print if $in;
  ' "$CHANGELOG"
}

check_changelog() {
  local version="$1"
  local heading_line unreleased_line date body

  if [[ ! -f "$CHANGELOG" ]]; then
    echo "error: CHANGELOG.md is missing." >&2
    exit 1
  fi
  heading_line="$(VER="$version" perl -ne 'if (index($_, "## [$ENV{VER}]") == 0) { print "$.:$_"; exit }' "$CHANGELOG")"
  unreleased_line="$(perl -ne 'if (/^## \[Unreleased\]\s*$/) { print $.; exit }' "$CHANGELOG")"
  if [[ -z "$heading_line" ]]; then
    echo "error: CHANGELOG.md has no '## [${version}] - YYYY-MM-DD' section." >&2
    echo "Move the [Unreleased] entries under '## [${version}] - $(date +%Y-%m-%d)', leave an empty '## [Unreleased]' above it, and commit." >&2
    exit 1
  fi
  if ! [[ "${heading_line#*:}" =~ ^##\ \[[^]]+\]\ -\ ([0-9]{4}-[0-9]{2}-[0-9]{2})[[:space:]]*$ ]]; then
    echo "error: CHANGELOG.md heading '${heading_line#*:}' should be '## [${version}] - YYYY-MM-DD' (no v prefix)." >&2
    exit 1
  fi
  date="${BASH_REMATCH[1]}"
  if [[ -z "$unreleased_line" ]]; then
    echo "error: CHANGELOG.md has no '## [Unreleased]' heading above '## [${version}]'." >&2
    exit 1
  fi
  if [[ "$unreleased_line" -gt "${heading_line%%:*}" ]]; then
    echo "error: '## [Unreleased]' must come before '## [${version}]' in CHANGELOG.md." >&2
    exit 1
  fi
  body="$(changelog_section "$version" | perl -ne 'print if /\S/')"
  if [[ -z "$body" ]]; then
    echo "error: the '## [${version}]' section in CHANGELOG.md is empty." >&2
    exit 1
  fi
  if [[ "$date" != "$(date +%Y-%m-%d)" ]]; then
    echo "warning: the CHANGELOG date for ${version} is ${date}, not today." >&2
  fi
}

confirm_release() {
  local answer
  printf 'Continue? [y/N] '
  read -r answer
  if [[ "$answer" != "y" && "$answer" != "Y" ]]; then
    echo "Release canceled."
    exit 0
  fi
}

parse_args "$@"
trap cleanup EXIT

require_tool git
require_tool gh
require_tool perl

cd "$PROJECT_ROOT"

if ! gh auth status --hostname github.com >/dev/null 2>&1; then
  echo "error: gh is not logged in to github.com (gh auth login)." >&2
  exit 1
fi

branch="$(git rev-parse --abbrev-ref HEAD)"
if [[ "$branch" != "main" ]]; then
  echo "error: releases are published from main (current branch: ${branch})." >&2
  exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
  echo "error: the working tree is not clean. Commit or stash before releasing." >&2
  exit 1
fi

origin_url="$(git remote get-url origin)"
if [[ "$origin_url" != *github.com[:/]tyhpproject/ext-tyhp* ]]; then
  echo "error: origin is '${origin_url}', expected github.com/tyhpproject/ext-tyhp." >&2
  exit 1
fi

version="$(module_version)"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
  echo "error: PHP_TYHP_VERSION '${version}' is not a semver version." >&2
  exit 1
fi

check_changelog "$version"

echo "Fetching origin/main"
git fetch origin main

head_sha="$(git rev-parse HEAD)"
origin_sha="$(git rev-parse origin/main)"
if [[ "$head_sha" != "$origin_sha" ]]; then
  echo "error: HEAD ${head_sha} is not origin/main ${origin_sha}. Push or pull before releasing." >&2
  exit 1
fi

if git ls-remote --exit-code --tags origin "refs/tags/${version}" >/dev/null 2>&1; then
  echo "error: tag ${version} already exists on origin." >&2
  exit 1
fi

if gh release view "$version" --repo "$RELEASE_REPO" >/dev/null 2>&1; then
  echo "error: GitHub release ${version} already exists." >&2
  exit 1
fi

NOTES_FILE="$(mktemp "${TMPDIR:-/tmp}/ext-tyhp-release-notes.XXXXXX")"
changelog_section "$version" | perl -0777 -pe 's/\A\s+//; s/\s+\z/\n/' > "$NOTES_FILE"

args=(
  gh release create "$version"
  --repo "$RELEASE_REPO"
  --target "$head_sha"
  --title "tyhp ${version}"
  --notes-file "$NOTES_FILE"
)
if [[ "$version" == *-* ]]; then
  args+=(--prerelease)
fi

echo "Version: ${version}"
echo "Tag:     ${version} (no v prefix)"
echo "Commit:  ${head_sha}"
echo "Release notes:"
cat "$NOTES_FILE"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "Dry run. Would run:"
  printf '  %q' "${args[@]}"
  printf '\n'
  exit 0
fi

echo "This publishes the release. The workflows then attach the zip assets."
echo "Leave GitHub immutable releases off, or those uploads are rejected."
confirm_release

"${args[@]}"
echo "Published https://github.com/${RELEASE_REPO}/releases/tag/${version}"
echo "The Windows and Unix workflows are attaching the PIE zips to that release."
echo "Packagist picks up the tag through its GitHub hook. This script does not call Packagist."
