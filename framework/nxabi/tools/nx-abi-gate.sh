#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M17-020: the single toolchain/glibc gate.
#
# It runs, in order:
#   1. the pinned toolchain verification          (M17-001, 012, 013)
#   2. the nxabi unit suite                       (M17-005..011, 014..016)
#   3. double ARMHF and AArch64 owned builds       (M17-019)
#   4. the ABI audit of both fixtures              (M17-003, 017)
#   5. a report-only audit of the reference ports  (M17-002, 018)
#
# Nothing here executes an ARM binary, touches a device, or writes inside the
# repository.  Every build happens in an owned mktemp tree.
set -euo pipefail
export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd -P)
NXABI="$REPO_ROOT/framework/nxabi/nxabi.py"
TOOLS="$REPO_ROOT/framework/nxabi/tools"
ARMHF_GCC=${NXABI_ARMHF_GCC:-/opt/prebuilt/bin/arm-linux-gnueabihf-gcc}
REPORT_ONLY_TARGETS=(
  "ports/kotor/kotor-universal"
  "ports/kotor/libkotor_input-universal.so"
  "ports/kotor/nxextract-ui"
  "ports/asm2_127/asm2_127-universal"
  "ports/sonic4/sonic4.arm64"
  "ports/bully2/bully.glibc230"
  "ports/horizonchase/horizonchase-universal"
)

# The work tree must not sit under a symlinked TMPDIR: several checks compare
# resolved paths and a symlink makes them disagree with themselves.
WORK_PARENT=${TMPDIR:-/tmp}
WORK_PARENT=$(cd -- "$WORK_PARENT" && pwd -P)
WORK=$(mktemp -d "$WORK_PARENT/nxabi-gate.XXXXXX")

cleanup() {
  local status=$?
  trap - EXIT
  case $WORK in
    "$WORK_PARENT"/nxabi-gate.??????)
      chmod -R u+w -- "$WORK" 2>/dev/null || true
      rm -rf -- "$WORK"
      ;;
    *)
      printf 'nx-abi-gate: refused cleanup outside owned mktemp: %s\n' \
        "$WORK" >&2
      status=1
      ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

step() { printf '\n[nx-abi-gate] %s\n' "$*"; }
fail() { printf 'nx-abi-gate: FAIL %s\n' "$*" >&2; exit 1; }

step "1/6 pinned toolchains"
python3 -B "$NXABI" toolchain --json "$WORK/toolchain.json" ||
  fail "toolchain pin drift"

step "2/6 nxabi unit suite"
python3 -B "$REPO_ROOT/framework/nxabi/tests/test_nxabi.py" >"$WORK/units.log" 2>&1 ||
  { sed -n '1,80p' "$WORK/units.log" >&2; fail "unit suite"; }
tail -n 3 "$WORK/units.log"

[[ -x $ARMHF_GCC ]] || fail "pinned ARMHF compiler is unavailable: $ARMHF_GCC"

cat >"$WORK/build-fixture.sh" <<'BUILD'
#!/usr/bin/env bash
set -euo pipefail
CC=${NXABI_ARMHF_GCC:-/opt/prebuilt/bin/arm-linux-gnueabihf-gcc}
cat >fixture.c <<'SRC'
#include "nx_symver.h"
#include <stdio.h>
int main(int argc, char **argv) {
  (void)argv;
  volatile float x = (float)argc;
  printf("%f\n", powf(x, 3.f) + expf(x) + exp2f(x) + logf(x) + log2f(x));
  return 0;
}
SRC
"$NX_SOURCE_ROOT/framework/nxabi/nxabi.py" provenance \
  --toolchain armv7-gcc --out provenance.c >/dev/null
# --build-id=sha1 is content derived, so it stays reproducible while giving the
# artifact the identifier M17-018 needs.
"$CC" -O2 -march=armv7-a -mfloat-abi=hard -Wl,--build-id=sha1 \
  -fno-builtin-powf -fno-builtin-expf -fno-builtin-exp2f \
  -fno-builtin-logf -fno-builtin-log2f \
  -I "$NX_SOURCE_ROOT/framework/nxabi/include" \
  fixture.c provenance.c -o fixture.armhf -lm
BUILD
chmod +x "$WORK/build-fixture.sh"

step "3/6 ARMHF double build (byte identity)"
TMPDIR="$WORK_PARENT" "$TOOLS/check-determinism.sh" \
  --artifact fixture.armhf -- "$WORK/build-fixture.sh" ||
  fail "the fixture build is not reproducible"

cat >"$WORK/build-fixture-aarch64.sh" <<'BUILD'
#!/usr/bin/env bash
set -euo pipefail
host_work=$(pwd -P)
cat >fixture.c <<'SRC'
#include "nx_symver.h"
#include <stdio.h>
int main(int argc, char **argv) {
  (void)argv;
  volatile float x = (float)argc;
  printf("%f\n", powf(x, 3.f) + expf(x) + exp2f(x) + logf(x) + log2f(x));
  return 0;
}
SRC
"$NX_SOURCE_ROOT/framework/nxabi/nxabi.py" provenance \
  --toolchain aarch64-buster-container --out provenance.c >/dev/null
docker run --rm --network none \
  --user "$(id -u):$(id -g)" \
  -v "$host_work:/work" -v "$NX_SOURCE_ROOT:/source:ro" -w /work \
  playfetch-builder:buster \
  aarch64-linux-gnu-gcc -O2 -march=armv8-a -Wl,--build-id=sha1 \
    -fno-builtin-powf -fno-builtin-expf -fno-builtin-exp2f \
    -fno-builtin-logf -fno-builtin-log2f \
    -I /source/framework/nxabi/include \
    fixture.c provenance.c -o fixture.aarch64 -lm
BUILD
chmod +x "$WORK/build-fixture-aarch64.sh"

step "4/6 AArch64 double build (byte identity, pinned offline image)"
TMPDIR="$WORK_PARENT" "$TOOLS/check-determinism.sh" \
  --artifact fixture.aarch64 -- "$WORK/build-fixture-aarch64.sh" ||
  fail "the AArch64 fixture build is not reproducible"

step "5/6 audit of the freshly built fixtures"
( cd "$WORK" && NX_SOURCE_ROOT="$REPO_ROOT" SOURCE_DATE_EPOCH=1600000000 \
    ./build-fixture.sh )
( cd "$WORK" && NX_SOURCE_ROOT="$REPO_ROOT" SOURCE_DATE_EPOCH=1600000000 \
    ./build-fixture-aarch64.sh )
python3 -B "$NXABI" audit "$WORK/fixture.armhf" "$WORK/fixture.aarch64" \
  --json "$WORK/fixture-audit.json" ||
  fail "an owned fixture does not satisfy the public policy"
python3 -B - "$WORK/fixture-audit.json" <<'PY' || exit 1
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    report = json.load(stream)
record = {finding["check"] for finding in report["findings"]}
# The fixture is built with nx_symver.h and the provenance stamp, so it must
# not even warn about the preferred floor or about missing provenance.
unexpected = record & {"glibc-preferred", "toolchain-note", "build-id"}
if unexpected:
    print("nx-abi-gate: fixture regressed on {}".format(sorted(unexpected)),
          file=sys.stderr)
    raise SystemExit(1)
if report.get("counts", {}).get("elves") != 2:
    raise SystemExit("nx-abi-gate: owned fixture inventory is incomplete")
print("nx-abi-gate: ARMHF and AArch64 fixtures are clean at the preferred floor")
PY

step "6/6 reference ports (report only)"
targets=()
for target in "${REPORT_ONLY_TARGETS[@]}"; do
  [[ -f $REPO_ROOT/$target ]] && targets+=("$target")
done
[[ ${#targets[@]} -eq ${#REPORT_ONLY_TARGETS[@]} ]] ||
  fail "approved reference inventory is incomplete"
set +e
( cd "$REPO_ROOT" && python3 -B "$NXABI" audit "${targets[@]}" \
    --json "$WORK/m17-reference-audit-v1.json" )
reference_status=$?
set -e
[[ $reference_status -eq 1 ]] ||
  fail "reference audit did not return the one documented historical error"
python3 -B - "$WORK/m17-reference-audit-v1.json" <<'PY' || exit 1
import json
import sys
from pathlib import PurePosixPath

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    report = json.load(stream)
expected = {
    "ports/kotor/kotor-universal",
    "ports/kotor/libkotor_input-universal.so",
    "ports/kotor/nxextract-ui",
    "ports/asm2_127/asm2_127-universal",
    "ports/sonic4/sonic4.arm64",
    "ports/bully2/bully.glibc230",
    "ports/horizonchase/horizonchase-universal",
}
if report.get("counts", {}).get("elves") != len(expected):
    raise SystemExit("nx-abi-gate: reference ELF count changed")
paths = {finding["path"] for finding in report.get("findings", [])}
if not paths <= expected:
    raise SystemExit("nx-abi-gate: reference report escaped approved scope")
if any(PurePosixPath(path).is_absolute() or ".." in PurePosixPath(path).parts
       for path in paths):
    raise SystemExit("nx-abi-gate: reference report contains an unsafe path")
errors = [
    (finding["path"], finding["check"])
    for finding in report.get("findings", [])
    if finding.get("level") == "error"
]
if errors != [("ports/horizonchase/horizonchase-universal", "search-path")]:
    raise SystemExit("nx-abi-gate: documented reference error set changed: {}"
                     .format(errors))
print("nx-abi-gate: reference scope and historical error are exact")
PY
cmp -s "$WORK/m17-reference-audit-v1.json" \
  "$REPO_ROOT/framework/nxabi/m17-reference-audit-v1.json" ||
  fail "checked-in reference audit is stale or contains host-specific paths"
printf 'nx-abi-gate: reference audit matches the sanitized checked-in receipt\n'

printf '\nnx-abi-gate: PASS toolchain=1 units=1 '
printf 'determinism_armhf=1 determinism_aarch64=1 fixture-audit=1\n'
