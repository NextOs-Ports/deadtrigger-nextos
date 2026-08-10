#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
UI="$PROJECT_ROOT/ui/build/nxextract-ui"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxextract-elf-gate.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT INT TERM

fail() {
  printf 'NXExtract ELF gate regression failed: %s\n' "$*" >&2
  exit 1
}

if [ ! -f "$UI" ]; then
  printf 'NXExtract ELF gate regression skipped: UI build is absent\n'
  exit 0
fi

cp "$UI" "$TEST_ROOT/ui"
"$PROJECT_ROOT/tools/check-glibc.sh" "$TEST_ROOT/ui" >/dev/null ||
  fail "a private regular copy of the canonical UI was rejected"

ln -s "$TEST_ROOT/ui" "$TEST_ROOT/ui-symlink"
if "$PROJECT_ROOT/tools/check-glibc.sh" "$TEST_ROOT/ui-symlink" \
     >/dev/null 2>&1; then
  fail "a symlinked ELF was accepted"
fi

ln "$TEST_ROOT/ui" "$TEST_ROOT/ui-hardlink"
if "$PROJECT_ROOT/tools/check-glibc.sh" "$TEST_ROOT/ui-hardlink" \
     >/dev/null 2>&1; then
  fail "a hard-linked ELF was accepted"
fi
rm "$TEST_ROOT/ui-hardlink"

: >"$TEST_ROOT/empty"
if "$PROJECT_ROOT/tools/check-glibc.sh" "$TEST_ROOT/empty" >/dev/null 2>&1; then
  fail "an empty file was accepted as an ELF"
fi

printf 'not-an-elf\n' >"$TEST_ROOT/text"
if "$PROJECT_ROOT/tools/check-glibc.sh" "$TEST_ROOT/text" >/dev/null 2>&1; then
  fail "a text file was accepted as an ELF"
fi

printf 'NXExtract ELF gate regression tests passed\n'
