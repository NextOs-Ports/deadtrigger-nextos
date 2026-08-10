#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Static regression gate for the 2026-08-08 host-session incident.
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SOURCE=$PROJECT_ROOT/nxbootstrap.sh
DEFAULT_TEST=$PROJECT_ROOT/tests/test-nxbootstrap.sh
GENERATOR_TEST=$PROJECT_ROOT/tests/test-generator.sh
ISOLATED_RUNNER=$PROJECT_ROOT/tests/run-isolated.sh

fail() {
  printf 'nxbootstrap safety gate failed: %s\n' "$*" >&2
  exit 1
}

for removed_name in \
  nxbootstrap_name_matches \
  nxbootstrap_process_has_port_identity \
  nxbootstrap_process_matches \
  nxbootstrap_matching_processes \
  nxbootstrap_collect_process_tree \
  nxbootstrap_live_tracked_entries \
  nxbootstrap_signal_tracked_entries \
  nxbootstrap_sweep_verified_instances \
  nxbootstrap_sweep_old_instance; do
  if rg -n --fixed-strings "$removed_name" "$SOURCE" >/dev/null; then
    fail "removed host-process matcher returned: $removed_name"
  fi
done

if rg -n '^[[:space:]]*for[[:space:]].*\[0-9\]\*' "$SOURCE" >/dev/null; then
  fail 'production bootstrap enumerates the host PID namespace'
fi

if rg -n 'NXBOOTSTRAP_SKIP_PROCESS_SWEEP|NXBOOTSTRAP_SWEEP_' \
    "$SOURCE" "$DEFAULT_TEST" "$GENERATOR_TEST" \
    "$PROJECT_ROOT/templates" "$PROJECT_ROOT/tools" \
    "$PROJECT_ROOT/README.md" >/dev/null; then
  fail 'obsolete process-sweep controls remain in nxbootstrap'
fi

if rg -n '(^|[^[:alnum:]_])(pkill|killall|systemctl|loginctl|shutdown|reboot|poweroff)([^[:alnum:]_]|$)' \
    "$SOURCE" "$PROJECT_ROOT/templates" "$PROJECT_ROOT/tools" >/dev/null; then
  fail 'production bootstrap contains a broad process/session/system command'
fi

if rg -n 'eval[[:space:]]' \
    "$SOURCE" "$PROJECT_ROOT/templates" "$PROJECT_ROOT/tools" >/dev/null; then
  fail 'production bootstrap executes an eval command'
fi
# $ESUDO is the canonical PortMaster privilege helper supplied by
# control.txt; the generated launcher may use it (chmod of exec bits and
# TTY/uinput nodes, the fleet-proven pattern), and the generator embeds
# those launcher blocks. The retired library still must not.
if rg -n '\$\{?ESUDO' "$SOURCE" >/dev/null; then
  fail 'production bootstrap executes an ambient privilege command'
fi

mapfile -t kill_lines < <(
  rg -n '^[[:space:]]*(builtin[[:space:]]+)?kill[[:space:]]' "$SOURCE"
)
[[ ${#kill_lines[@]} -eq 1 ]] ||
  fail "unexpected number of production kill invocations: ${#kill_lines[@]}"
[[ ${kill_lines[0]} == *'builtin kill -"$signal" "$pid"'* ]] ||
  fail 'the only real signal path is not the exact-child helper'

if rg -n '^[[:space:]]*(builtin[[:space:]]+)?kill[[:space:]]' \
    "$DEFAULT_TEST" "$GENERATOR_TEST" >/dev/null; then
  fail 'the default test suite can send a process signal'
fi

for guarded_test in "$DEFAULT_TEST" "$GENERATOR_TEST"; do
  if ! rg -n 'nxbootstrap_require_private_pid_namespace' \
      "$guarded_test" >/dev/null; then
    fail "process-bearing test lacks the namespace guard: $guarded_test"
  fi
done

if ! rg -n --fixed-strings \
    'unshare --user --map-root-user --pid --fork --kill-child=KILL' \
    "$ISOLATED_RUNNER" >/dev/null; then
  fail 'isolated runner does not create the required private namespaces'
fi

for required_token in \
  "trap 'nxbootstrap_on_signal 129' HUP" \
  'NXBOOTSTRAP_CLEANED=1' \
  'nxbootstrap_open_fresh_log_fd' \
  "stat -L -c '%d:%i'" \
  'nxbootstrap_file_link_count' \
  'nxbootstrap_validate_elf_contract' \
  "'libSDL2-2.0.so*'"; do
  rg -n --fixed-strings "$required_token" "$SOURCE" >/dev/null ||
    fail "mandatory adversarial contract is missing: $required_token"
done

# 0.6.0 launcher safety tokens: guarded port-env.sh sourcing, single-instance
# lock, signal-forwarding cleanup and console reset before pm_finish.
for template_token in \
  '[ ! -L "$GAMEDIR/port-env.sh" ]' \
  'flock -n 9' \
  "trap '' INT TERM HUP" \
  "printf '\\033c'"; do
  rg -n --fixed-strings "$template_token" "$PROJECT_ROOT/templates" >/dev/null ||
    fail "generated wrapper safety contract is missing: $template_token"
done

if ! rg -n '^NXBOOTSTRAP_PROC_ROOT=/proc$' "$SOURCE" >/dev/null; then
  fail 'production procfs root is not reset to /proc while sourcing'
fi

if rg -n '\$\{NXBOOTSTRAP_PROC_ROOT:-' "$SOURCE" >/dev/null; then
  fail 'production still accepts an ambient procfs fallback expression'
fi

(
  # Sourcing defines functions only; the subshell prevents state from escaping.
  # Exercise the runtime path, not just the mirrored text checked by Python.
  # shellcheck disable=SC1090
  source "$SOURCE"
  nxbootstrap_capability_known host.portmaster
  nxbootstrap_validate_named_list capability \
    $'host.portmaster\ngraphics.gles2\ninput.controller-api'
  if nxbootstrap_capability_known host.unregistered-capability ||
     nxbootstrap_validate_named_list capability host.rocknix ||
     nxbootstrap_validate_named_list capability host.muos ||
     nxbootstrap_validate_named_list capability host.personal-name ||
     nxbootstrap_validate_named_list capability host.ipv4-address; then
    exit 1
  fi
) || fail 'runtime capability allowlist accepted an unknown/identity name'

bash -n "$SOURCE" "$DEFAULT_TEST" "$GENERATOR_TEST" \
  "$PROJECT_ROOT/tests/private-pid-namespace.sh" \
  "$PROJECT_ROOT/tests/isolated-suite.sh" \
  "$PROJECT_ROOT/tests/test-runner-interruption.sh" \
  "$PROJECT_ROOT/tests/test-namespace-watchdog.sh" \
  "$ISOLATED_RUNNER" "$PROJECT_ROOT/templates/launcher.sh.in" "$0"
printf 'nxbootstrap static safety gate passed\n'
