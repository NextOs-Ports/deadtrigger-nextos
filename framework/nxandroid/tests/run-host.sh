#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Canonical host-only nxandroid gate. It executes mock-owned C tests and never
# loads guest code, sends signals, accesses devices or opens the network.
set -euo pipefail

NXANDROID_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
NXANDROID_BUILD_JOBS=${NXANDROID_BUILD_JOBS:-2}

case $NXANDROID_BUILD_JOBS in
  ''|*[!0-9]*|0)
    printf 'nxandroid-host: invalid build job count\n' >&2
    exit 2
    ;;
esac
if (( NXANDROID_BUILD_JOBS > 8 )); then
  printf 'nxandroid-host: build job count exceeds safe maximum 8\n' >&2
  exit 2
fi

for required_command in cmake ctest gcc clang find grep sha256sum awk \
                        basename tee; do
  command -v "$required_command" >/dev/null 2>&1 || {
    printf 'nxandroid-host: missing required command: %s\n' \
      "$required_command" >&2
    exit 77
  }
done

NXANDROID_WORK=$(mktemp -d /tmp/nxandroid-host.XXXXXX)
[[ -d $NXANDROID_WORK && ! -L $NXANDROID_WORK ]] || {
  printf 'nxandroid-host: mktemp did not create a safe work tree\n' >&2
  exit 1
}
cleanup() {
  local status=$?
  trap - EXIT
  case $NXANDROID_WORK in
    /tmp/nxandroid-host.??????)
      find "$NXANDROID_WORK" -depth -delete 2>/dev/null || status=1
      ;;
    *)
      printf 'nxandroid-host: refused cleanup outside owned mktemp tree\n' >&2
      status=1
      ;;
  esac
  if [[ -e $NXANDROID_WORK ]]; then
    printf 'nxandroid-host: cleanup_failed=1\n' >&2
    status=1
  else
    printf 'nxandroid-host: work_tree_cleaned=1\n'
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
cd -- "$NXANDROID_WORK"

assert_claim_once() {
  local output=$1 claim=$2 count
  count=$(grep -Ec "(^|[[:space:]])${claim}$" "$output" || true)
  [[ $count -eq 1 ]] || {
    printf 'nxandroid-host: expected claim exactly once: %s (found %s)\n' \
      "$claim" "$count" >&2
    return 1
  }
}

sanitizer_environment() {
  export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1:quarantine_size_mb=32
  export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
  export LSAN_OPTIONS=exitcode=23:report_objects=1
}

install_smoke() {
  local label=$1 compiler=$2 build_dir=$3
  local prefix=$NXANDROID_WORK/install-$label
  local executable=$NXANDROID_WORK/install-smoke-$label
  local -a libraries=()

  cmake --install "$build_dir" --prefix "$prefix"
  [[ -f $prefix/include/nxandroid.h ]] || {
    printf 'nxandroid-host: %s installed header missing\n' "$label" >&2
    return 1
  }
  mapfile -t libraries < <(
    find "$prefix" -type f -name libnxandroid.a -print
  )
  [[ ${#libraries[@]} -eq 1 ]] || {
    printf 'nxandroid-host: %s installed library count=%s\n' \
      "$label" "${#libraries[@]}" >&2
    return 1
  }

  "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$prefix/include" -x c - -x none "${libraries[0]}" \
    -o "$executable" <<'EOF'
#include <string.h>
#include <nxandroid.h>

int main(void) {
  return strcmp(nxandroid_result_string(NXANDROID_OK), "ok") != 0;
}
EOF
  sanitizer_environment
  "$executable"
  printf 'nxandroid-host: install_smoke_%s=PASS library_sha256=%s\n' \
    "$label" "$(sha256sum -- "${libraries[0]}" | awk '{print $1}')"
}

run_compiler_gate() {
  local label=$1 compiler=$2
  local build_dir=$NXANDROID_WORK/build-$label
  local output=$NXANDROID_WORK/ctest-$label.log

  cmake -S "$NXANDROID_ROOT" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$compiler" \
    -DNXANDROID_BUILD_TESTS=ON \
    -DNXANDROID_BUILD_SIGNAL_TEST=OFF \
    -DNXANDROID_ENABLE_SANITIZERS=ON \
    -DNXANDROID_WARNINGS_AS_ERRORS=ON
  cmake --build "$build_dir" --parallel "$NXANDROID_BUILD_JOBS"
  sanitizer_environment
  ctest --test-dir "$build_dir" --output-on-failure --verbose 2>&1 | \
    tee "$output"
  assert_claim_once "$output" nxandroid_tests=PASS
  assert_claim_once "$output" guest_code_executed=0
  assert_claim_once "$output" device_access=0
  assert_claim_once "$output" network_access=0
  assert_claim_once "$output" signals_used=0
  assert_claim_once "$output" contexts_completed=1000
  install_smoke "$label" "$compiler" "$build_dir"
  printf 'nxandroid-host: %s_asan_ubsan_lsan=PASS contexts=1000\n' "$label"
}

run_clang_analyzer() {
  local source
  for source in "$NXANDROID_ROOT/src/nxandroid.c" \
                "$NXANDROID_ROOT/src/nxandroid_imports.c"; do
    clang --analyze -std=c99 -Wall -Wextra -Wpedantic -Wshadow \
      -Wstrict-prototypes -Werror -I"$NXANDROID_ROOT/include" "$source"
  done
  printf 'nxandroid-host: clang_analyze=PASS\n'
}

run_gcc_analyzer_if_supported() {
  local probe=$NXANDROID_WORK/gcc-analyzer-probe.o
  local source object
  if printf 'int nxandroid_analyzer_probe(void) { return 0; }\n' | \
      gcc -std=c99 -x c - -c -fanalyzer -o "$probe" >/dev/null 2>&1; then
    for source in "$NXANDROID_ROOT/src/nxandroid.c" \
                  "$NXANDROID_ROOT/src/nxandroid_imports.c"; do
      object=$NXANDROID_WORK/$(basename "${source%.c}").analyzer.o
      gcc -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
        -Werror -fanalyzer -I"$NXANDROID_ROOT/include" -c "$source" \
        -o "$object"
    done
    printf 'nxandroid-host: gcc_fanalyzer=PASS supported=1\n'
  else
    printf 'nxandroid-host: gcc_fanalyzer=SKIP supported=0\n'
  fi
}

run_compiler_gate gcc gcc
run_compiler_gate clang clang
run_clang_analyzer
run_gcc_analyzer_if_supported

printf 'nxandroid_host_gate=PASS\n'
printf 'nxandroid-host: contexts_completed=2000 guest_code_executed=0 '
printf 'device_access=0 network_access=0 signals_used=0\n'
printf 'nxandroid-host: guest_initializers_executed=0 '
printf 'guest_jni_onload_executed=0 hardware_ran=0 build_jobs=%s\n' \
  "$NXANDROID_BUILD_JOBS"
