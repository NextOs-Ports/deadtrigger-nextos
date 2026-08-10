#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Suite body invoked only by namespace-watchdog.py inside run-isolated.sh.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

bash "$TEST_DIR/test-generator.sh"
bash "$TEST_DIR/test-launcher-behavior.sh"
python3 -B "$TEST_DIR/test-manifest-contract.py"
bash "$TEST_DIR/test-runner-interruption.sh"
bash "$TEST_DIR/test-namespace-watchdog.sh"
python3 -B \
  "$TEST_DIR/../../nxloader/tests/test_m10_aarch64_guest_gate_runner.py"
python3 -B \
  "$TEST_DIR/../../nxandroid/tools/inventory_m11_guests.py"

# nxandroid's process-bearing signal fixture reuses this already sealed
# namespace. Its build/output tree is unique to this suite invocation and is
# removed by the EXIT trap on both success and failure.
NXANDROID_SIGNAL_WORK_ROOT=$(mktemp -d \
  /tmp/nxandroid-signal-isolated.XXXXXX)
[[ -d $NXANDROID_SIGNAL_WORK_ROOT && ! -L $NXANDROID_SIGNAL_WORK_ROOT ]] || {
  printf 'nxbootstrap isolated suite: unsafe nxandroid signal work root\n' >&2
  exit 1
}
export NXANDROID_SIGNAL_WORK_ROOT

cleanup_nxandroid_signal_work() {
  local status=$?
  trap - EXIT
  case $NXANDROID_SIGNAL_WORK_ROOT in
    /tmp/nxandroid-signal-isolated.??????)
      if [[ ! -d $NXANDROID_SIGNAL_WORK_ROOT ||
            -L $NXANDROID_SIGNAL_WORK_ROOT ]]; then
        printf 'nxbootstrap isolated suite: signal work root changed type\n' \
          >&2
        status=1
      elif ! find "$NXANDROID_SIGNAL_WORK_ROOT" -depth -delete; then
        printf 'nxbootstrap isolated suite: signal work cleanup failed\n' >&2
        status=1
      elif [[ -e $NXANDROID_SIGNAL_WORK_ROOT ||
              -L $NXANDROID_SIGNAL_WORK_ROOT ]]; then
        printf 'nxbootstrap isolated suite: signal work root remains\n' >&2
        status=1
      else
        printf 'nxbootstrap isolated suite: nxandroid_signal_work_cleaned=1\n'
      fi
      ;;
    *)
      printf 'nxbootstrap isolated suite: refused unsafe signal cleanup path\n' \
        >&2
      status=1
      ;;
  esac
  unset NXANDROID_SIGNAL_WORK_ROOT
  exit "$status"
}
trap cleanup_nxandroid_signal_work EXIT

bash "$TEST_DIR/../../nxandroid/tests/run-signal-isolated.sh" --inside-suite
