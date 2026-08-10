#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Behavioral gate for the generated 0.6.0 launcher: it LAUNCHES the launcher
# and measures what a device would see (log, exit status, lock, signals,
# physical GAMEDIR, mapping contract). String checks live in test-generator.sh;
# this file exists so those strings can never go green on their own.
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
PROJECT_ROOT=$(cd -- "$TEST_DIR/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$TEST_DIR/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?

TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-behavior.XXXXXX")
cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-behavior.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'launcher behavior test failed: %s\n' "$*" >&2
  if [[ -n ${LOG-} && -f $LOG ]]; then
    printf '%s\n' '--- launcher log ---' >&2
    sed -n '1,240p' "$LOG" >&2 || true
    printf '%s\n' '--- end launcher log ---' >&2
  fi
  exit 1
}

wait_for_file() {
  local file=$1 attempt
  for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -e $file ]] && return 0
    sleep 0.1
  done
  fail "timed out waiting for marker $file"
}

wait_for_process_exit() {
  local pid=$1 attempt process_stat process_rest process_state
  for ((attempt = 0; attempt < 160; ++attempt)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    if IFS= read -r process_stat < "/proc/$pid/stat" 2>/dev/null; then
      process_rest=${process_stat##*) }
      process_state=${process_rest%% *}
      [[ $process_state == Z || $process_state == X ]] && return 0
    fi
    sleep 0.1
  done
  return 1
}

wait_for_ignored_signals() {
  local pid=$1 attempt ignored
  for ((attempt = 0; attempt < 100; ++attempt)); do
    ignored=$(awk '/^SigIgn:/ {print $2}' "/proc/$pid/status" 2>/dev/null || true)
    if [[ $ignored =~ ^[0-9A-Fa-f]+$ ]] &&
       (( (16#$ignored & 16#4003) == 16#4003 )); then
      return 0
    fi
    sleep 0.1
  done
  fail "timed out waiting for launcher $pid to ignore HUP/INT/TERM"
}

assert_finish_once() {
  local context=$1 count=0
  [[ -f $MARKERS/pm-finish ]] && count=$(wc -l < "$MARKERS/pm-finish")
  [[ $count == 1 ]] || fail "pm_finish count for $context is $count, expected 1"
}

# The launcher prefers the /opt PortMaster roots; on a host that really has
# one installed the fixture below would never be sourced and every assertion
# would test the wrong control.txt. Mask them inside this private mount
# namespace so the fixture is ALWAYS the selected root (this also fixes the
# vacuous-fixture hole found in review).
for masked in /opt/system/Tools /opt/tools /storage/roms/ports /roms/ports; do
  if [[ -d $masked ]]; then
    mount -t tmpfs -o size=64k,mode=0755 tmpfs "$masked" 2>/dev/null ||
      fail "cannot mask preferred PortMaster root $masked"
  fi
done

# ---------------------------------------------------------------- fixture CFW
# ROM tree reached through a SYMLINK so the physical-GAMEDIR guarantee is
# actually exercised (the /roms -> /storage/roms case caught on real NextOS).
REAL_ROOT=$TEST_ROOT/real
ln -s "$REAL_ROOT" "$TEST_ROOT/link"
mkdir -p "$REAL_ROOT/roms/ports"
# TMPDIR itself may sit behind a symlink; assertions compare physical paths.
REAL_PHYS=$(cd "$REAL_ROOT" && pwd -P)
PM_DIR=$TEST_ROOT/xdg/PortMaster
mkdir -p "$PM_DIR"
MARKERS=$TEST_ROOT/markers
mkdir -p "$MARKERS"
RUNTIME_DIR=$TEST_ROOT/runtime
mkdir -m 0700 "$RUNTIME_DIR"
LOCK_FILE="$RUNTIME_DIR/.nxbootstrap-${UID:-$(id -u)}/nxport-behav-port.flock"

cat > "$PM_DIR/control.txt" <<CONTROL
# behavioral fixture control.txt
directory="${TEST_ROOT#/}/link/roms"
ESUDO=""
CUR_TTY=/dev/null
CFW_NAME=behavfix
sdl_controllerconfig="\${NXBEHAV_MAPPING-}"
get_controls() {
  if [ -n "\${NXBEHAV_EARLY_MARKER-}" ]; then
    : > "\$NXBEHAV_EARLY_MARKER"
    while :; do sleep 1; done
  fi
}
pm_platform_helper() { printf '%s\n' "\$1" >> "$MARKERS/platform-helper"; printf '%s\n' "\${LD_LIBRARY_PATH-}" >> "$MARKERS/helper-ldpath"; }
pm_finish() { printf 'finish\n' >> "$MARKERS/pm-finish"; }
CONTROL

# ---------------------------------------------------------------- fixture port
cat > "$TEST_ROOT/nxport.json" <<'JSON'
{
  "schema_version": 2,
  "id": "behav-port",
  "title": "Behav Port",
  "launcher_name": "Behav Port.sh",
  "architecture": "aarch64",
  "executable": "behav-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.2.6"},
  "required_files": ["behav-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
python3 -B "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/nxport.json" \
  --output "$TEST_ROOT/generated" >/dev/null ||
  fail 'generator refused the behavioral fixture manifest'

PORTS_DIR=$REAL_ROOT/roms/ports
cp "$TEST_ROOT/generated/Behav Port.sh" "$PORTS_DIR/"
mkdir -p "$PORTS_DIR/behav-port"
cp "$TEST_ROOT/generated/behav-port/nxport.json" "$PORTS_DIR/behav-port/"

cat > "$TEST_ROOT/reset-signals.py" <<'PY'
import os
import signal
import sys

for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(signum, signal.SIG_DFL)
os.execvpe(sys.argv[1], sys.argv[1:], os.environ)
PY

cat > "$TEST_ROOT/term-stuck.py" <<PY
import pathlib
import signal

ready = pathlib.Path("$MARKERS/term-stuck-ready")
seen = pathlib.Path("$MARKERS/term-seen")

def on_term(_signum, _frame):
    seen.write_text("TERM\n", encoding="utf-8")

signal.signal(signal.SIGTERM, on_term)
ready.write_text("ready\n", encoding="utf-8")
while True:
    signal.pause()
PY

cat > "$PORTS_DIR/behav-port/behav-loader" <<STUB
#!/bin/bash
{
  printf 'cwd=%s\n' "\$(pwd -P)"
  printf 'game_dir=%s\n' "\${NXCOMPAT_GAME_DIR-}"
  printf 'port_id=%s\n' "\${NXCOMPAT_PORT_ID-}"
  printf 'mapping=%s\n' "\${SDL_GAMECONTROLLERCONFIG-__unset__}"
} > "$MARKERS/child-env"
printf 'child-stderr\n' >&2
case "\${NXBEHAV_STUB_MODE-}" in
  sleep) exec sleep 5 ;;
  term-immune)
    trap '' TERM
    : > "$MARKERS/term-immune-ready"
    sleep 2
    ;;
  term-127)
    trap '' TERM
    : > "$MARKERS/term-127-ready"
    sleep 2
    exit 127
    ;;
  term-default)
    : > "$MARKERS/term-default-ready"
    exec sleep 5
    ;;
  term-stuck) exec python3 "$TEST_ROOT/term-stuck.py" ;;
  daemon-fd)
    # A detached background helper that would inherit any leaked lock fd.
    sleep 30 >/dev/null 2>&1 </dev/null &
    printf "%s" "\$!" > "$MARKERS/daemon-pid"
    ;;
esac
exit 42
STUB
chmod 0755 "$PORTS_DIR/behav-port/behav-loader"

LAUNCHER=$PORTS_DIR/Behav\ Port.sh
run_launcher() {
  env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
    XDG_DATA_HOME="$TEST_ROOT/xdg" \
    NXBEHAV_MAPPING="${NXBEHAV_MAPPING-}" \
    NXBEHAV_STUB_MODE="${NXBEHAV_STUB_MODE-}" \
    NXBEHAV_EARLY_MARKER="${NXBEHAV_EARLY_MARKER-}" \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    bash "$LAUNCHER" </dev/null
}

# ------------------------------------------------- 1. normal run, real values
NXBEHAV_MAPPING='behavguid,Behav Pad,a:b0,start:b7,back:b6'
export NXBEHAV_MAPPING
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "true exit status was not propagated (got $status)"
LOG=$PORTS_DIR/behav-port/log.txt
[[ -f $LOG ]] || fail 'durable log.txt was not written'
grep -q 'cfw=behavfix' "$LOG" ||
  fail 'launcher did not source the fixture control.txt (vacuous run)'
grep -q 'end (status 42)' "$LOG" || fail 'log does not record the real status'
grep -q '^child-stderr$' "$LOG" || fail 'child stderr did not reach log.txt'
grep -qx "cwd=$REAL_PHYS/roms/ports/behav-port" "$MARKERS/child-env" ||
  fail 'child cwd is not the physical game directory'
grep -qx "game_dir=$REAL_PHYS/roms/ports/behav-port" "$MARKERS/child-env" ||
  fail 'NXCOMPAT_GAME_DIR is not the physical game directory'
grep -qx 'port_id=behav-port' "$MARKERS/child-env" ||
  fail 'child did not receive NXCOMPAT_PORT_ID'
grep -qx "mapping=$NXBEHAV_MAPPING" "$MARKERS/child-env" ||
  fail 'CFW mapping did not reach the child'
[[ $(wc -l < "$MARKERS/pm-finish") == 1 ]] ||
  fail 'pm_finish did not run exactly once on the normal path'
grep -q "behav-loader" "$MARKERS/platform-helper" ||
  fail 'pm_platform_helper did not receive the real executable'
! grep -q "behav-port" "$MARKERS/helper-ldpath" ||
  fail 'game/private library paths contaminated pm_platform_helper'

# --------------------------------- 2. empty CFW mapping must not clobber env
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
NXBEHAV_MAPPING=''
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "empty-mapping run broke the launcher ($status)"
grep -qx 'mapping=__unset__' "$MARKERS/child-env" ||
  fail 'empty CFW mapping clobbered SDL_GAMECONTROLLERCONFIG'
assert_finish_once empty-mapping

# Background cases must signal the LAUNCHER itself, not a function subshell:
# env execs bash, so $! from this spawn is the launcher process.
spawn_launcher() {
  (
    trap - INT HUP TERM
    exec env -i PATH="$PATH" HOME="$TEST_ROOT" TMPDIR="${TMPDIR:-/tmp}" \
      XDG_DATA_HOME="$TEST_ROOT/xdg" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
      NXBEHAV_MAPPING="${NXBEHAV_MAPPING-}" \
      NXBEHAV_STUB_MODE="$1" \
      NXBEHAV_EARLY_MARKER="${NXBEHAV_EARLY_MARKER-}" \
      python3 "$TEST_ROOT/reset-signals.py" bash "$LAUNCHER" \
      </dev/null >/dev/null 2>&1
  ) &
  spawned=$!
}

# ------------------------------------- 3. early HUP/INT/TERM status contract
for signal_case in HUP:129 INT:130 TERM:143; do
  signal_name=${signal_case%%:*}
  expected_status=${signal_case#*:}
  NXBEHAV_EARLY_MARKER="$MARKERS/early-$signal_name"
  rm -f "$NXBEHAV_EARLY_MARKER" "$MARKERS/child-env"
  : > "$MARKERS/pm-finish"
  spawn_launcher ''
  launcher_pid=$spawned
  wait_for_file "$NXBEHAV_EARLY_MARKER"
  kill -"$signal_name" "$launcher_pid"
  status=0; wait "$launcher_pid" || status=$?
  [[ $status == "$expected_status" ]] ||
    fail "early $signal_name returned $status, expected $expected_status"
  [[ ! -e $MARKERS/child-env ]] ||
    fail "loader ran after early $signal_name"
  assert_finish_once "early-$signal_name"
done
unset NXBEHAV_EARLY_MARKER

# ---------------------- 4. lock survives atomic replacement of the executable
cat > "$PORTS_DIR/behav-port/behav-loader.next" <<STUB
#!/bin/bash
: > "$MARKERS/replacement-ran"
{
  printf 'cwd=%s\n' "\$(pwd -P)"
  printf 'game_dir=%s\n' "\${NXCOMPAT_GAME_DIR-}"
  printf 'port_id=%s\n' "\${NXCOMPAT_PORT_ID-}"
  printf 'mapping=%s\n' "\${SDL_GAMECONTROLLERCONFIG-__unset__}"
} > "$MARKERS/child-env"
printf 'child-stderr\n' >&2
case "\${NXBEHAV_STUB_MODE-}" in
  sleep) exec sleep 5 ;;
  term-immune)
    trap '' TERM
    : > "$MARKERS/term-immune-ready"
    sleep 2
    ;;
  term-127)
    trap '' TERM
    : > "$MARKERS/term-127-ready"
    sleep 2
    exit 127
    ;;
  term-default)
    : > "$MARKERS/term-default-ready"
    exec sleep 5
    ;;
  term-stuck) exec python3 "$TEST_ROOT/term-stuck.py" ;;
  daemon-fd)
    # A detached background helper that would inherit any leaked lock fd.
    sleep 30 >/dev/null 2>&1 </dev/null &
    printf "%s" "\$!" > "$MARKERS/daemon-pid"
    ;;
esac
exit 42
STUB
chmod 0755 "$PORTS_DIR/behav-port/behav-loader.next"
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/replacement-ran"
spawn_launcher sleep
first=$spawned
wait_for_file "$MARKERS/child-env"
old_loader_id=$(stat -c '%d:%i' "$PORTS_DIR/behav-port/behav-loader")
mv -f "$PORTS_DIR/behav-port/behav-loader.next" \
  "$PORTS_DIR/behav-port/behav-loader"
new_loader_id=$(stat -c '%d:%i' "$PORTS_DIR/behav-port/behav-loader")
[[ $old_loader_id != "$new_loader_id" ]] ||
  fail 'atomic executable replacement did not change inode'
NXBEHAV_STUB_MODE=''
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "second instance was not refused (got $status)"
grep -q 'another instance holds the lock' "$LOG" ||
  fail 'lock refusal was not logged'
[[ ! -e $MARKERS/replacement-ran ]] ||
  fail 'replacement loader ran while the old inode was active'
assert_finish_once lock-refusal
kill -TERM "$first" 2>/dev/null || true
wait "$first" 2>/dev/null || true
[[ $(wc -l < "$MARKERS/pm-finish") == 2 ]] ||
  fail 'pm_finish was not exactly once for each contending launcher'
lock_released=0
for _ in $(seq 1 60); do
  if flock -n "$LOCK_FILE" -c true 2>/dev/null; then
    lock_released=1
    break
  fi
  sleep 0.2
done
[[ $lock_released == 1 ]] || fail 'stable port lock was not released'
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "replacement loader did not run after release ($status)"
[[ -e $MARKERS/replacement-ran ]] || fail 'replacement loader never ran'
assert_finish_once replacement-after-release

# Lock identity checks must reject attacker-controlled aliases without running
# the loader or modifying the symlink target.
mv "$LOCK_FILE" "$LOCK_FILE.saved"
printf 'victim\n' > "$MARKERS/lock-victim"
ln -s "$MARKERS/lock-victim" "$LOCK_FILE"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "symlink lock was accepted (status $status)"
[[ $(<"$MARKERS/lock-victim") == victim && ! -e $MARKERS/child-env ]] ||
  fail 'symlink lock touched its target or launched the child'
assert_finish_once symlink-lock-rejection
rm "$LOCK_FILE"
mv "$LOCK_FILE.saved" "$LOCK_FILE"

ln "$LOCK_FILE" "$LOCK_FILE.alias"
rm -f "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 1 ]] || fail "hardlinked lock was accepted (status $status)"
[[ ! -e $MARKERS/child-env ]] || fail 'hardlinked lock launched the child'
assert_finish_once hardlink-lock-rejection
rm "$LOCK_FILE.alias"

# ----------------------- 5. cooperative child keeps its truthful exit status
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/term-immune-ready"
spawn_launcher term-immune
launcher_pid=$spawned
wait_for_file "$MARKERS/child-env"
wait_for_file "$MARKERS/term-immune-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
kill -HUP "$launcher_pid" 2>/dev/null || true
status=0; wait "$launcher_pid" || status=$?
[[ $status == 42 ]] ||
  fail "TERM-immune child's real status was not reported (got $status)"
grep -q 'end (status 42)' "$LOG" ||
  fail 'log does not record the true status after a trapped signal'
assert_finish_once cooperative-termination

# --------------------------- 6. exit 127 remains a truthful child exit status
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/term-127-ready"
spawn_launcher term-127
launcher_pid=$spawned
wait_for_file "$MARKERS/term-127-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
kill -HUP "$launcher_pid" 2>/dev/null || true
status=0; wait "$launcher_pid" || status=$?
[[ $status == 127 ]] || fail "real child status 127 became $status"
grep -q 'end (status 127)' "$LOG" || fail 'real status 127 was not recorded'
assert_finish_once status-127

# ----------------------- 7. a child terminated by TERM truthfully returns 143
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/child-env" "$MARKERS/term-default-ready"
spawn_launcher term-default
launcher_pid=$spawned
wait_for_file "$MARKERS/term-default-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
status=0; wait "$launcher_pid" || status=$?
[[ $status == 143 ]] || fail "real child status 143 became $status"
grep -q 'end (status 143)' "$LOG" || fail 'real status 143 was not recorded'
assert_finish_once status-143

# --------------------------- 8. TERM deadline forces a stuck direct child out
rm -f "$MARKERS/term-stuck-ready" "$MARKERS/term-seen" "$MARKERS/child-env"
: > "$MARKERS/pm-finish"
spawn_launcher term-stuck
launcher_pid=$spawned
wait_for_file "$MARKERS/term-stuck-ready"
kill -TERM "$launcher_pid" 2>/dev/null || true
# Prove a later frontend signal cannot interrupt grace/re-wait only after the
# direct child confirms TERM and the launcher publishes ignored dispositions.
wait_for_file "$MARKERS/term-seen"
wait_for_ignored_signals "$launcher_pid"
kill -INT "$launcher_pid" 2>/dev/null || true
if ! wait_for_process_exit "$launcher_pid"; then
  kill -KILL "$launcher_pid" 2>/dev/null || true
  wait "$launcher_pid" 2>/dev/null || true
  fail 'launcher exceeded the bounded termination deadline'
fi
status=0; wait "$launcher_pid" || status=$?
[[ $status == 137 ]] || fail "forced termination returned $status, expected 137"
[[ -e $MARKERS/term-seen ]] || fail 'stuck child never observed TERM'
grep -q 'termination deadline expired; sending KILL' "$LOG" ||
  fail 'forced KILL was not recorded'
grep -q 'end (status 137)' "$LOG" || fail 'forced status 137 was not recorded'
assert_finish_once forced-termination

# ---------------- 9. symlinked control.txt is ignored, game still runs
mv "$PM_DIR/control.txt" "$PM_DIR/control.real"
ln -s "$PM_DIR/control.real" "$PM_DIR/control.txt"
NXBEHAV_STUB_MODE=''
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "symlinked control.txt broke the launcher ($status)"
grep -q 'cfw=none' "$LOG" ||
  fail 'launcher sourced a symlinked control.txt'
rm "$PM_DIR/control.txt"
mv "$PM_DIR/control.real" "$PM_DIR/control.txt"

# ---------------- 10. hostile CFW_NAME cannot traverse into a mod source
cat > "$TEST_ROOT/xdg/pwn.txt" <<PWN
printf 'pwned\n' >> "$MARKERS/pwn"
PWN
sed -i 's|^CFW_NAME=behavfix$|CFW_NAME="x/../../pwn"|' "$PM_DIR/control.txt"
status=0; run_launcher >/dev/null 2>&1 || status=$?
[[ $status == 42 ]] || fail "hostile CFW_NAME broke the launcher ($status)"
[[ ! -e $MARKERS/pwn ]] ||
  fail 'CFW_NAME path traversal sourced an attacker-controlled mod file'
sed -i 's|^CFW_NAME="x/../../pwn"$|CFW_NAME=behavfix|' "$PM_DIR/control.txt"

# -------- 11. lock fd must not leak to a detached child (close-on-exec sense)
# The stub forks a background helper and exits 42; the launcher exits too. If
# the lock fd had leaked into the child, the orphaned helper would still hold
# the port lock and the next launch would be refused.
: > "$MARKERS/pm-finish"
rm -f "$MARKERS/daemon-pid"
NXBEHAV_STUB_MODE=daemon-fd
status=0; run_launcher >/dev/null 2>&1 || status=$?
NXBEHAV_STUB_MODE=''
[[ $status == 42 ]] || fail "daemon-fd run broke the launcher ($status)"
[[ -f $MARKERS/daemon-pid ]] || fail 'daemon helper was not spawned'
daemon_pid=$(cat "$MARKERS/daemon-pid")
# The detached helper is still alive here, but the launcher has exited.
kill -0 "$daemon_pid" 2>/dev/null || fail 'daemon helper died too early to test the leak'
if ! flock -n "$LOCK_FILE" -c true 2>/dev/null; then
  kill -KILL "$daemon_pid" 2>/dev/null || true
  fail 'lock fd leaked into the detached child; port lock held after launcher exit'
fi
kill -KILL "$daemon_pid" 2>/dev/null || true
wait "$daemon_pid" 2>/dev/null || true

printf 'nxbootstrap launcher behavior gate passed: runs=17 lock=4 early-signals=3 bounded-termination=4 finish-once=14 physical-gamedir=1 mapping-contract=1 portmaster-hardening=2 lock-fd-no-leak=1\n'
