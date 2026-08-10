#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Canonical host gate: every command gets its own durable run log.
set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
RUN_LOGGED=$REPO_ROOT/framework/tools/run-logged.sh

if [[ ${1:-} != --log-root || -z ${2:-} || $# -ne 2 ]]; then
  printf 'usage: %s --log-root ABSOLUTE_DIRECTORY\n' "${0##*/}" >&2
  exit 2
fi
LOG_ROOT=$2
case $LOG_ROOT in /*) ;; *)
  printf 'run-safe-gates: log root must be absolute\n' >&2
  exit 2
  ;;
esac

cd -- "$REPO_ROOT"
passed=0
run_gate() {
  local gate_id=$1
  shift
  printf '[safe-gates] START %s\n' "$gate_id"
  "$RUN_LOGGED" --log-root "$LOG_ROOT" -- "$@"
  passed=$((passed + 1))
  printf '[safe-gates] PASS %s\n' "$gate_id"
}

# Infrastructure must pass before any filesystem/process-heavy suite.
run_gate test-infrastructure \
  python3 -B framework/tests/test_infrastructure.py
run_gate bootstrap-static-safety \
  bash framework/nxbootstrap/tests/test-safety-static.sh
run_gate tooling-filesystem \
  bash framework/tests/test-tools.sh

run_gate catalog \
  python3 -B framework/catalog/tests/test_catalog.py
run_gate nxport-contract \
  python3 -B framework/nxbootstrap/tests/test-manifest-contract.py
run_gate portmaster-contract \
  python3 -B framework/portmaster/tests/test_portmaster_contract.py
run_gate nxcompat-host \
  bash framework/nxcompat/tests/run-host.sh
run_gate nxcompat-m12-audit \
  python3 -B framework/nxcompat/tests/test_m12_audit.py
run_gate nxobs-m12a-host \
  python3 -B framework/nxobs/tests/test_m12a_observability.py
run_gate nxgl-m13-host \
  bash framework/nxgl/tests/run-m13-host.sh
run_gate nxgl-m13-audit \
  python3 -B framework/nxgl/tests/test_m13_audit.py
run_gate nxaudio-m14-host \
  bash framework/nxaudio/tests/run-host.sh
run_gate nxaudio-m14-audit \
  python3 -B framework/nxaudio/tests/test_m14_audio_contract.py
run_gate nxextract-python \
  python3 -B -m unittest discover \
    -s suportando_outros_devices/extrator-universal/tests -p 'test_*.py'
run_gate nxextract-runtime-env \
  bash suportando_outros_devices/extrator-universal/tests/test_runtime_env.sh
run_gate nxextract-audit \
  python3 -B framework/nxextract/tests/test_m07_audit.py
run_gate nxextract-release \
  bash suportando_outros_devices/extrator-universal/tools/check-release.sh \
    --require-ui
run_gate nxabi-units \
  python3 -B framework/nxabi/tests/test_nxabi.py
run_gate nxabi-m17-gate \
  bash framework/nxabi/tools/nx-abi-gate.sh
run_gate nxabi-m17-closure \
  python3 -B framework/nxabi/tests/test_m17_closure.py
run_gate nxloader-audit \
  python3 -B framework/nxloader/tests/test_m08_audit.py
run_gate nxloader-m09-audit \
  python3 -B framework/nxloader/tests/test_m09_audit.py
run_gate nxloader-m10-audit \
  python3 -B framework/nxloader/tests/test_m10_audit.py
run_gate nxloader-m11-audit \
  python3 -B framework/nxloader/tests/test_m11_audit.py
run_gate nxandroid-profile-audit \
  python3 -B framework/nxandroid/tests/test_m11_adapter_profiles.py
run_gate nxandroid-m11-audit \
  python3 -B framework/nxandroid/tests/test_m11_audit.py
run_gate nxandroid-m16-contract \
  python3 -B framework/nxandroid/tests/test_m16_adapter_contract.py
run_gate nxloader-host \
  bash framework/nxloader/tests/run-host.sh
run_gate nxloader-armv7-cross \
  bash framework/nxloader/tests/run-armv7-cross.sh
run_gate nxloader-aarch64-cross \
  bash framework/nxloader/tests/run-aarch64-cross.sh
run_gate nxloader-m11-armv7-lifecycle-cross \
  bash framework/nxloader/tests/run-m11-armv7-lifecycle-cross.sh
run_gate nxloader-m11-aarch64-lifecycle-cross \
  bash framework/nxloader/tests/run-m11-aarch64-lifecycle-cross.sh
run_gate nxandroid-host \
  bash framework/nxandroid/tests/run-host.sh
run_gate nxinput-m15-contract \
  python3 -B framework/nxinput/tests/test_m15_input_contract.py
run_gate bootstrap-isolated \
  bash framework/nxbootstrap/tests/run-isolated.sh
run_gate nxrelease-m18-closure \
  python3 -B framework/nxrelease/tests/test_m18_closure.py
run_gate nxrelease \
  bash framework/nxrelease/tests/test_nxrelease.sh
run_gate nxgenerator-host \
  python3 -B framework/nxgenerator/tests/test_nxgenerator.py
run_gate nxgenerator-m19-closure \
  python3 -B framework/nxgenerator/tests/test_m19_closure.py
run_gate m20-closure \
  python3 -B framework/tests/test_m20_closure.py
run_gate chrono-m21-audit \
  python3 -B ports/chrono/tests/test_m21_pilot.py
run_gate chrono-m21-host \
  bash ports/chrono/tests/run-m21-host.sh
run_gate chrono-m22-audit \
  python3 -B ports/chrono/tests/test_m22_physical.py
run_gate chrono-m23-audit \
  python3 -B ports/chrono/tests/test_m23_promotion.py
run_gate chrono-m24-closure \
  python3 -B ports/chrono/tests/test_m24_closure.py
run_gate diff-check \
  git diff --check -- framework suportando_outros_devices publicando_ports
run_gate m20-green-checkpoint \
  bash framework/tests/capture-green-checkpoint.sh \
  --checkpoint-root "$LOG_ROOT/checkpoints"

printf '[safe-gates] ALL PASS count=%s hardware_ran=0 device_access=0 ' "$passed"
printf 'external_guest_initializers_executed=0 '
printf 'external_guest_jni_onload_executed=0 synthetic_lifecycle_runs=4\n'
