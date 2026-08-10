#!/usr/bin/env bash
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TOOL="$ROOT/nxrelease.py"
BOOTSTRAP_ROOT=$(CDPATH= cd -- "$ROOT/../nxbootstrap" && pwd -P)
NXEXTRACT_ROOT=$(CDPATH= cd -- "$ROOT/../../suportando_outros_devices/extrator-universal" && pwd -P)
BOOTSTRAP_VERSION=$(<"$BOOTSTRAP_ROOT/VERSION")
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/nxrelease-test.XXXXXX")
trap 'rm -rf -- "$TEST_TMP"' EXIT INT TERM

fail() {
  printf 'nxrelease tests: FAIL: %s\n' "$*" >&2
  exit 1
}

expect_fail() {
  label=$1
  pattern=$2
  shift 2
  if "$@" >"$TEST_TMP/$label.output" 2>&1; then
    fail "$label unexpectedly passed"
  fi
  if ! grep -Eqi "$pattern" "$TEST_TMP/$label.output"; then
    sed -n '1,120p' "$TEST_TMP/$label.output" >&2
    fail "$label did not report /$pattern/"
  fi
}

for command_name in python3 readelf bash sh sha256sum cmp aarch64-linux-gnu-gcc clang ld.lld; do
  command -v "$command_name" >/dev/null 2>&1 ||
    fail "required command missing: $command_name"
done

mkdir -p "$TEST_TMP/source/fixture/nxextract" "$TEST_TMP/source/payload"

cat >"$TEST_TMP/nxport-input.json" <<'JSON'
{
  "schema_version": 1,
  "id": "fixture",
  "title": "NXRelease Fixture",
  "launcher_name": "Game.sh",
  "architecture": "aarch64",
  "executable": "bin/aarch64/loader",
  "argument_mode": "game-dir-and-passthrough",
  "home_mode": "preserve",
  "nxextract": "yes",
  "required_files": ["bin/aarch64/loader"],
  "extra_library_paths": [],
  "prepare_script": ""
}
JSON
python3 "$BOOTSTRAP_ROOT/tools/generate-port.py" \
  "$TEST_TMP/nxport-input.json" --output "$TEST_TMP/source" >/dev/null

cp "$NXEXTRACT_ROOT/nxextract.py" \
  "$TEST_TMP/source/fixture/nxextract/nxextract.py"
cp "$NXEXTRACT_ROOT/run-extractor.sh" \
  "$TEST_TMP/source/fixture/nxextract/run-extractor.sh"
cp "$NXEXTRACT_ROOT/nxextract-runtime-env.sh" \
  "$TEST_TMP/source/fixture/nxextract/nxextract-runtime-env.sh"
printf '%s\n' '1.2.6' >"$TEST_TMP/source/fixture/nxextract-version.txt"
cat >"$TEST_TMP/source/fixture/extractor.json" <<'JSON'
{
  "schema": 1,
  "id": "fixture",
  "version": "test",
  "title": "NXRelease Fixture",
  "abi_order": ["arm64-v8a"],
  "input": {},
  "extract": [],
  "hooks": [],
  "validate": [],
  "commit": [],
  "marker": ".nxextract-fixture.json"
}
JSON

printf '%s\n' 'fixture payload' >"$TEST_TMP/source/payload/README.txt"

printf '%s\n' \
  '{' \
  '  "version": 4,' \
  '  "name": "fixture.zip",' \
  '  "items": ["Game.sh", "fixture/"],' \
  '  "items_opt": [],' \
  '  "attr": {' \
  '    "title": "NXRelease Fixture",' \
  '    "arch": ["aarch64"],' \
  '    "min_glibc": "2.30",' \
  '    "image": {"cover": "cover.png"}' \
  '  }' \
  '}' \
  >"$TEST_TMP/source/port.json"

printf '%s\n' \
  '<?xml version="1.0" encoding="utf-8"?>' \
  '<gameList>' \
  '  <game>' \
  '    <path>./Game.sh</path>' \
  '    <name>NXRelease Fixture</name>' \
  '    <image>./fixture/cover.png</image>' \
  '  </game>' \
  '</gameList>' \
  >"$TEST_TMP/source/gameinfo.xml"

python3 - "$TEST_TMP/source/cover.png" <<'PY'
import sys
with open(sys.argv[1], "wb") as handle:
    handle.write(b"\x89PNG\r\n\x1a\n" + b"offline-fixture")
PY

# Real dependency-free, loadable AArch64 ELF. A synthetic 64-byte header is
# deliberately not a valid fixture: the gate requires PT_LOAD.
cat >"$TEST_TMP/start.S" <<'ASM'
.global _start
_start:
    mov x0, #0
    mov x8, #93
    svc #0
ASM
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-e,_start \
  "$TEST_TMP/start.S" -o "$TEST_TMP/source/loader"
chmod 0755 "$TEST_TMP/source/loader"

# Additional real ELFs exercise dependency closure and strict dynamic tags.
cat >"$TEST_TMP/dep.c" <<'C'
int dep(void) { return 7; }
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,libdep.so "$TEST_TMP/dep.c" \
  -o "$TEST_TMP/source/libdep.so"
cat >"$TEST_TMP/android-log.c" <<'C'
int android_log_stub(void) { return 0; }
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,liblog.so "$TEST_TMP/android-log.c" \
  -o "$TEST_TMP/source/android-liblog.so"
cat >"$TEST_TMP/android-game.c" <<'C'
extern int android_log_stub(void);
int game_entry(void) { return android_log_stub(); }
C
aarch64-linux-gnu-gcc -fPIC -nostdlib -shared \
  -Wl,-soname,libgame.so "$TEST_TMP/android-game.c" \
  -L"$TEST_TMP/source" -Wl,--no-as-needed -l:android-liblog.so \
  -o "$TEST_TMP/source/android-game.so"
cat >"$TEST_TMP/consumer.S" <<'ASM'
.global _start
.extern dep
_start:
    bl dep
    mov x0, #0
    mov x8, #93
    svc #0
ASM
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
  "$TEST_TMP/consumer.S" -L"$TEST_TMP/source" -Wl,--no-as-needed -ldep \
  -o "$TEST_TMP/source/consumer"

aarch64-linux-gnu-gcc -c "$TEST_TMP/start.S" -o "$TEST_TMP/source/reloc.o"
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/wrong-loader.so \
  "$TEST_TMP/start.S" -o "$TEST_TMP/source/wrong-interp"
aarch64-linux-gnu-gcc -nostdlib -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
  -Wl,-rpath,'$ORIGIN/lib' "$TEST_TMP/start.S" \
  -o "$TEST_TMP/source/with-runpath"

cat >"$TEST_TMP/arm-start.S" <<'ASM'
.syntax unified
.global _start
_start:
    mov r0, #0
    mov r7, #1
    svc #0
ASM
clang --target=armv7-linux-gnueabi -fuse-ld=lld -nostdlib -static \
  -Wl,-e,_start "$TEST_TMP/arm-start.S" -o "$TEST_TMP/source/arm-softfp"

python3 - "$TEST_TMP/source/no-load" "$TEST_TMP/source/class-mismatch" <<'PY'
import struct
import sys

# Complete headers that readelf accepts, but one has no program headers and
# the other deliberately declares ELF32 for an AArch64 manifest.
ident64 = bytearray(16)
ident64[:4] = b"\x7fELF"
ident64[4:7] = bytes((2, 1, 1))
header64 = struct.pack(
    "<16sHHIQQQIHHHHHH", bytes(ident64), 2, 183, 1, 0x400000,
    0, 0, 0, 64, 56, 0, 64, 0, 0,
)
with open(sys.argv[1], "wb") as handle:
    handle.write(header64)

ident32 = bytearray(16)
ident32[:4] = b"\x7fELF"
ident32[4:7] = bytes((1, 1, 1))
header32 = struct.pack(
    "<16sHHIIIIIHHHHHH", bytes(ident32), 2, 183, 1, 0x10000,
    52, 0, 0, 52, 32, 1, 40, 0, 0,
)
phdr32 = struct.pack("<IIIIIIII", 1, 0, 0x10000, 0x10000, 84, 84, 5, 0x1000)
with open(sys.argv[2], "wb") as handle:
    handle.write(header32 + phdr32)
PY

# A real license/notice file exercises the license-notice kind on the happy path.
printf '%s\n' 'Test license notice for the offline deterministic fixture.' \
  > "$TEST_TMP/source/LICENSE"
chmod 0644 "$TEST_TMP/source/LICENSE"

# Sectionless ELF: the same loadable AArch64 ELF with its section header table
# stripped, proving the gate audits Linux ELFs that only expose program headers.
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-e,_start \
  "$TEST_TMP/start.S" -o "$TEST_TMP/source/loader-sectionless"
aarch64-linux-gnu-objcopy --strip-section-headers "$TEST_TMP/source/loader-sectionless"
chmod 0755 "$TEST_TMP/source/loader-sectionless"

python3 - "$TEST_TMP/source" "$TEST_TMP/manifest.json" "$BOOTSTRAP_VERSION" <<'PY'
import hashlib
import json
import os
import sys

root, output, bootstrap_version = sys.argv[1:]
bootstrap_tuple = tuple(int(part) for part in bootstrap_version.split("."))
self_contained = bootstrap_tuple >= (0, 6, 0)
bootstrap_name = (
    "nxbootstrap-{}.sh".format(bootstrap_version)
    if bootstrap_tuple >= (0, 5, 0)
    else "nxbootstrap.sh"
)

def digest(relative):
    h = hashlib.sha256()
    with open(os.path.join(root, relative), "rb") as handle:
        h.update(handle.read())
    return h.hexdigest()

manifest = {
    "schema_version": 2,
    "source_root": "source",
    "package": {
        "id": "fixture",
        "version": "1.0.0",
        "profile": "universal-portmaster",
        "launcher": "Game.sh",
        "launcher_chain": (
            ["Game.sh"] if self_contained
            else ["Game.sh", "fixture/" + bootstrap_name]
        ),
        "launcher_contract": {
            "generator": "nxbootstrap",
            "version": bootstrap_version,
            "config_path": "fixture/nxport.json",
            "config_sha256": digest("fixture/nxport.json"),
        },
        "port_dir": "fixture",
        "license": {
            "spdx_id": "LicenseRef-Test",
            "source_url": "https://example.invalid/fixture",
            "file": "fixture/LICENSE",
        },
    },
    "release": {
        "source_date_epoch": 1785542400,
        "max_glibc": "2.30",
        "compression": "deflated",
    },
    "nxextract": {
        "path": "fixture/nxextract/nxextract.py",
        "version": "1.2.6",
        "minimum_version": "1.2.2",
        "sha256": digest("fixture/nxextract/nxextract.py"),
        "runner_path": "fixture/nxextract/run-extractor.sh",
        "runner_sha256": digest("fixture/nxextract/run-extractor.sh"),
        "runtime_env_path": "fixture/nxextract/nxextract-runtime-env.sh",
        "runtime_env_sha256": digest("fixture/nxextract/nxextract-runtime-env.sh"),
        "recipe_path": "fixture/extractor.json",
        "recipe_sha256": digest("fixture/extractor.json"),
    },
    "portmaster_metadata": {
        "port_json": {
            "path": "fixture/port.json", "sha256": digest("port.json"),
        },
        "gameinfo_xml": {
            "path": "fixture/gameinfo.xml", "sha256": digest("gameinfo.xml"),
        },
        "images": [{
            "path": "fixture/cover.png", "role": "cover",
            "sha256": digest("cover.png"),
        }],
    },
    "dependencies": [],
    "files": [
        {
            "source": "Game.sh", "target": "Game.sh",
            "kind": "launcher", "mode": "0755", "sha256": digest("Game.sh"),
        },
        {
            "source": "fixture/nxport.json", "target": "fixture/nxport.json",
            "kind": "nxbootstrap-config", "mode": "0644",
            "sha256": digest("fixture/nxport.json"),
        },
        {
            "source": "loader", "target": "fixture/bin/aarch64/loader",
            "kind": "project-linux", "mode": "0755",
            "architecture": "aarch64",
            "build_profile": "universal-low-glibc",
            "provenance": "offline deterministic test fixture",
            "sha256": digest("loader"), "needed": [], "soname": None,
        },
        {
            "source": "loader-sectionless",
            "target": "fixture/bin/aarch64/loader-sectionless",
            "kind": "project-linux", "mode": "0755",
            "architecture": "aarch64",
            "build_profile": "universal-low-glibc",
            "provenance": "offline deterministic sectionless fixture",
            "sha256": digest("loader-sectionless"), "needed": [], "soname": None,
        },
        {
            "source": "LICENSE", "target": "fixture/LICENSE",
            "kind": "license-notice", "mode": "0644", "sha256": digest("LICENSE"),
        },
        {
            "source": "fixture/nxextract/nxextract.py",
            "target": "fixture/nxextract/nxextract.py",
            "kind": "nxextract", "mode": "0644",
            "sha256": digest("fixture/nxextract/nxextract.py"),
        },
        {
            "source": "fixture/nxextract/run-extractor.sh",
            "target": "fixture/nxextract/run-extractor.sh",
            "kind": "nxextract-runner", "mode": "0644",
            "sha256": digest("fixture/nxextract/run-extractor.sh"),
        },
        {
            "source": "fixture/nxextract/nxextract-runtime-env.sh",
            "target": "fixture/nxextract/nxextract-runtime-env.sh",
            "kind": "nxextract-runtime-env", "mode": "0644",
            "sha256": digest("fixture/nxextract/nxextract-runtime-env.sh"),
        },
        {
            "source": "fixture/nxextract-version.txt",
            "target": "fixture/nxextract-version.txt",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/nxextract-version.txt"),
        },
        {
            "source": "fixture/extractor.json",
            "target": "fixture/extractor.json",
            "kind": "nxextract-recipe", "mode": "0644",
            "sha256": digest("fixture/extractor.json"),
        },
        {
            "source": "payload", "target": "fixture/assets",
            "kind": "payload",
        },
        {
            "source": "port.json", "target": "fixture/port.json",
            "kind": "portmaster-metadata", "mode": "0644",
            "sha256": digest("port.json"),
        },
        {
            "source": "gameinfo.xml", "target": "fixture/gameinfo.xml",
            "kind": "portmaster-metadata", "mode": "0644",
            "sha256": digest("gameinfo.xml"),
        },
        {
            "source": "cover.png", "target": "fixture/cover.png",
            "kind": "portmaster-image", "mode": "0644",
            "sha256": digest("cover.png"),
        },
    ],
}
if not self_contained:
    manifest["files"].extend([
        {
            "source": "fixture/nxbootstrap.sh",
            "target": "fixture/nxbootstrap.sh",
            "kind": "script", "mode": "0644",
            "sha256": digest("fixture/nxbootstrap.sh"),
        },
        {
            "source": "fixture/" + bootstrap_name,
            "target": "fixture/" + bootstrap_name,
            "kind": "script", "mode": "0644",
            "sha256": digest("fixture/" + bootstrap_name),
        },
        {
            "source": "fixture/nxdeployment.json",
            "target": "fixture/nxdeployment.json",
            "kind": "payload", "mode": "0644",
            "sha256": digest("fixture/nxdeployment.json"),
        },
    ])
with open(output, "w", encoding="utf-8") as handle:
    json.dump(manifest, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY

python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json" |
  grep -q 'NXRELEASE VALIDATE: PASS' || fail 'positive validate did not pass'

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/pre-deployment-bootstrap.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["package"]["launcher_contract"]["version"] = "0.4.0"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail pre-deployment-bootstrap 'before 0[.]5[.]1.*complete deployment receipt' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/pre-deployment-bootstrap.json"
# 0.6.0: the single launcher carries the whole PortMaster integration.
for token in 'control.txt' 'get_controls' 'pm_platform_helper' \
             'nxbootstrap_finish' 'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"' \
             'NXBOOTSTRAP_LOCK_PATH_ID' 'NXBOOTSTRAP_LOCK_FD_ID' \
             'NXBOOTSTRAP_CHILD_STARTTIME=${20}' \
             'NXBOOTSTRAP_SHUTDOWN_TICKS=10' \
             'builtin kill -KILL "$game_pid"' "trap '' INT TERM HUP"; do
  grep -Fq "$token" "$TEST_TMP/source/Game.sh" ||
    fail "self-contained launcher fixture is missing: $token"
done

# A benign secondary hop used to survive validation because every script was
# audited individually but the public shell layout was never counted.
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$TEST_TMP/source/benign.sh"
chmod 0755 "$TEST_TMP/source/benign.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source/benign.sh" \
  "$TEST_TMP/secondary-run.json" "$TEST_TMP/second-top-level.json" <<'PY'
import copy
import hashlib
import json
import sys

manifest_path, script_path, run_output, top_output = sys.argv[1:]
with open(manifest_path, encoding="utf-8") as handle:
    base = json.load(handle)
with open(script_path, "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()

secondary_run = copy.deepcopy(base)
secondary_run["files"].append({
    "source": "benign.sh",
    "target": "fixture/RUN.sh",
    "kind": "script",
    "mode": "0755",
    "sha256": digest,
})
with open(run_output, "w", encoding="utf-8") as handle:
    json.dump(secondary_run, handle, sort_keys=True, indent=2)
    handle.write("\n")

second_top_level = copy.deepcopy(base)
second_top_level["files"].append({
    "source": "benign.sh",
    "target": "Legacy.sh",
    "kind": "script",
    "mode": "0755",
    "sha256": digest,
})
with open(top_output, "w", encoding="utf-8") as handle:
    json.dump(second_top_level, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
expect_fail secondary-run 'forbidden secondary.*run[.]sh' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/secondary-run.json"
expect_fail second-top-level 'exactly one top-level [.]sh' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/second-top-level.json"

# Public release consumes only canonical nxport v2 and proves the generated
# The single public launcher carries every declarative field. Legacy input is upgraded by the
# generator, never guessed by the release gate.
for contract_case in legacy unknown capability capability-order quirk \
                     divergence runtime-contract required-files \
                     nxextract-optional required-check dead-payload-gates \
                     inode-bound-lock unbounded-termination unguarded-finish; do
  cp -a -- "$TEST_TMP/source" "$TEST_TMP/source-$contract_case"
  python3 - "$TEST_TMP/manifest.json" \
    "$TEST_TMP/manifest-$contract_case.json" \
    "$TEST_TMP/source-$contract_case" "$contract_case" <<'PY'
import hashlib
import json
import pathlib
import sys

base, output, source_root, mode = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)
data["source_root"] = pathlib.Path(source_root).name

root = pathlib.Path(source_root)
config_path = root / "fixture" / "nxport.json"
launcher_path = root / "Game.sh"
if mode == "legacy":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["schema_version"] = 1
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "unknown":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["process_names"] = ["forbidden"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "capability":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["required_capabilities"] = ["host.unregistered-capability"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "capability-order":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["required_capabilities"] = ["graphics.gles2", "host.portmaster"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "quirk":
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["enabled_quirks"] = ["engine.unregistered-quirk"]
    config_path.write_text(json.dumps(config, sort_keys=True, indent=2) + "\n",
                           encoding="utf-8")
elif mode == "divergence":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'GAMEDIR="/$directory/ports/fixture"'
    if text.count(old) != 1:
        raise SystemExit("GAMEDIR assignment fixture changed")
    launcher_path.write_text(
        text.replace(old, 'GAMEDIR="/$directory/ports/other"'),
        encoding="utf-8")
elif mode == "runtime-contract":
    text = launcher_path.read_text(encoding="utf-8")
    old = "export NXCOMPAT_RUNTIME_REPORT=log-and-logo"
    if text.count(old) != 1:
        raise SystemExit("runtime report export fixture changed")
    launcher_path.write_text(
        text.replace(old, "export NXCOMPAT_RUNTIME_REPORT=log"),
        encoding="utf-8")
elif mode == "required-files":
    text = launcher_path.read_text(encoding="utf-8")
    old = "NXBOOTSTRAP_REQUIRED_FILES=bin/aarch64/loader"
    if text.count(old) != 1:
        raise SystemExit("required_files assignment fixture changed")
    launcher_path.write_text(
        text.replace(old, "NXBOOTSTRAP_REQUIRED_FILES=missing/payload", 1),
        encoding="utf-8")
elif mode == "nxextract-optional":
    text = launcher_path.read_text(encoding="utf-8")
    old = "NXEXTRACT_REQUESTED=1"
    if text.count(old) != 1:
        raise SystemExit("mode=yes NXExtract fixture changed")
    launcher_path.write_text(
        text.replace(old, "NXEXTRACT_REQUESTED=0", 1),
        encoding="utf-8")
elif mode == "required-check":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'required_path=$(readlink -f "$GAMEDIR/$required_file"'
    if text.count(old) != 1:
        raise SystemExit("required file readlink fixture changed")
    launcher_path.write_text(
        text.replace(
            old,
            'required_path=$(printf %s "$GAMEDIR/$required_file"',
            1,
        ),
        encoding="utf-8")
elif mode == "dead-payload-gates":
    text = launcher_path.read_text(encoding="utf-8")
    first = "# NXExtract owner-data phase"
    last = "unset NXBOOTSTRAP_REQUIRED_FILES required_file required_path"
    if text.count(first) != 1 or text.count(last) != 1:
        raise SystemExit("payload gate boundaries changed")
    text = text.replace(first, "if false; then\n" + first, 1)
    launcher_path.write_text(
        text.replace(last, last + "\nfi", 1), encoding="utf-8"
    )
elif mode == "inode-bound-lock":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"'
    if text.count(old) != 1:
        raise SystemExit("stable lock fixture changed")
    launcher_path.write_text(
        text.replace(old, 'exec 9<"$NXBOOTSTRAP_EXECUTABLE"', 1),
        encoding="utf-8")
elif mode == "unbounded-termination":
    text = launcher_path.read_text(encoding="utf-8")
    old = 'builtin kill -KILL "$game_pid"'
    if text.count(old) != 1:
        raise SystemExit("forced termination fixture changed")
    launcher_path.write_text(
        text.replace(old, 'builtin kill -TERM "$game_pid"', 1),
        encoding="utf-8")
elif mode == "unguarded-finish":
    text = launcher_path.read_text(encoding="utf-8")
    old = '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0'
    if text.count(old) != 1:
        raise SystemExit("finish guard fixture changed")
    launcher_path.write_text(
        text.replace(old, "true", 1), encoding="utf-8")
else:
    raise SystemExit("unknown contract case")

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

if mode in ("legacy", "unknown", "capability", "capability-order", "quirk"):
    config_sha = digest(config_path)
    data["package"]["launcher_contract"]["config_sha256"] = config_sha
    entry = next(item for item in data["files"]
                 if item["target"] == "fixture/nxport.json")
    entry["sha256"] = config_sha
else:
    launcher_sha = digest(launcher_path)
    entry = next(item for item in data["files"]
                 if item["target"] == "Game.sh")
    entry["sha256"] = launcher_sha

with open(output, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY
done
expect_fail nxport-legacy 'schema_version must be 2|regenerate legacy' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-legacy.json"
expect_fail nxport-unknown 'unknown field.*process_names' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-unknown.json"
expect_fail nxport-capability 'required_capabilities has an unknown name' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-capability.json"
expect_fail nxport-capability-order 'required_capabilities is not in canonical' \
  python3 "$TOOL" validate --manifest \
    "$TEST_TMP/manifest-capability-order.json"
expect_fail nxport-quirk 'enabled_quirks has an unknown name' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-quirk.json"
expect_fail nxport-divergence 'does not derive GAMEDIR' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest-divergence.json"
expect_fail nxport-runtime-contract \
  'NXCOMPAT_RUNTIME_REPORT differs from nxport[.]json' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-runtime-contract.json"
expect_fail nxport-required-files 'required_files gate differs' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-required-files.json"
expect_fail nxport-nxextract-optional 'NXExtract policy differs' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-nxextract-optional.json"
expect_fail nxport-required-check 'required_files gate differs' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-required-check.json"
expect_fail nxport-dead-payload-gates 'canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-dead-payload-gates.json"
expect_fail nxport-inode-bound-lock 'is missing|canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-inode-bound-lock.json"
expect_fail nxport-unbounded-termination 'is missing|canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-unbounded-termination.json"
expect_fail nxport-unguarded-finish 'is missing|canonical nxbootstrap render' \
  python3 "$TOOL" validate \
    --manifest "$TEST_TMP/manifest-unguarded-finish.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/schema-v1.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["schema_version"] = 1
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail schema-v1 'v1 did not close dependencies|schema_version must be 2' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/schema-v1.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/traversal.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["files"][-1]["target"] = "../escape"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail manifest-traversal 'not a safe relative path' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/traversal.json"

ln -s ../README.txt "$TEST_TMP/source/payload/hostile-link"
expect_fail source-symlink 'contains symlink|traverses a symlink|non-regular file' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"
rm -f -- "$TEST_TMP/source/payload/hostile-link"

python3 "$TOOL" build \
  --manifest "$TEST_TMP/manifest.json" \
  --stage "$TEST_TMP/stage-one" \
  --output "$TEST_TMP/fixture-one.zip" \
  | grep -q 'NXRELEASE BUILD: PASS' || fail 'positive build did not pass'

[ -f "$TEST_TMP/stage-one/fixture/.nxrelease/MANIFEST.sha256" ] || fail 'stage has no scoped MANIFEST.sha256'
[ -f "$TEST_TMP/stage-one/fixture/.nxrelease/NXRELEASE-METADATA.json" ] || fail 'stage has no scoped release metadata'
[ ! -e "$TEST_TMP/stage-one/MANIFEST.sha256" ] || fail 'shared ZIP root contains colliding MANIFEST.sha256'
[ -f "$TEST_TMP/fixture-one.zip.sha256" ] || fail 'archive has no external SHA-256'

python3 "$TOOL" verify \
  --archive "$TEST_TMP/fixture-one.zip" \
  --sha256-file "$TEST_TMP/fixture-one.zip.sha256" \
  | grep -q 'NXRELEASE VERIFY: PASS' || fail 'archive verify did not pass'

python3 "$TOOL" verify-stage --stage "$TEST_TMP/stage-one" \
  | grep -q 'NXRELEASE VERIFY-STAGE: PASS' || fail 'stage verify did not pass'

# A second build from identical inputs must be byte-for-byte identical.
python3 "$TOOL" build \
  --manifest "$TEST_TMP/manifest.json" \
  --stage "$TEST_TMP/stage-two" \
  --output "$TEST_TMP/fixture-two.zip" >/dev/null
cmp "$TEST_TMP/fixture-one.zip" "$TEST_TMP/fixture-two.zip" ||
  fail 'deterministic archives differ'

python3 "$TOOL" bundle \
  --manifest "$TEST_TMP/manifest.json" \
  --stage "$TEST_TMP/stage-bundle" \
  --destination "$TEST_TMP/publication-bundle" \
  --archive-name fixture.zip \
  | grep -q 'NXRELEASE BUNDLE: PASS' || fail 'atomic bundle did not pass'
[ -f "$TEST_TMP/publication-bundle/fixture.zip" ] || fail 'bundle lacks ZIP'
[ -f "$TEST_TMP/publication-bundle/fixture.zip.sha256" ] || fail 'bundle lacks SHA-256'
cmp "$TEST_TMP/fixture-one.zip" "$TEST_TMP/publication-bundle/fixture.zip" ||
  fail 'bundle ZIP is not deterministic'

python3 - "$TEST_TMP/fixture-one.zip" "$BOOTSTRAP_VERSION" <<'PY'
import json
import stat
import sys
import time
import zipfile

bootstrap_version = sys.argv[2]
bootstrap_tuple = tuple(int(part) for part in bootstrap_version.split("."))
bootstrap_name = (
    "nxbootstrap-{}.sh".format(bootstrap_version)
    if bootstrap_tuple >= (0, 5, 0)
    else "nxbootstrap.sh"
)

with zipfile.ZipFile(sys.argv[1]) as archive:
    infos = archive.infolist()
    names = [item.filename for item in infos]
    assert names == sorted(names)
    assert names.index("fixture/nxextract-version.txt") < names.index(
        "fixture/nxextract/nxextract.py"
    )
    metadata = json.loads(archive.read("fixture/.nxrelease/NXRELEASE-METADATA.json").decode("utf-8"))
    expected_chain = (
        ["Game.sh"] if bootstrap_tuple >= (0, 6, 0)
        else ["Game.sh", "fixture/" + bootstrap_name]
    )
    assert metadata["package"]["launcher_chain"] == expected_chain
    assert metadata["portmaster_metadata"]["port_json"]["path"] == "fixture/port.json"
    assert metadata["elf_audit"]["files"][0]["needed"] == []
    assert metadata["elf_audit"]["files"][0]["soname"] is None
    epoch = metadata["archive"]["source_date_epoch"]
    stamp = list(time.gmtime(epoch)[:6])
    stamp[5] -= stamp[5] % 2
    assert all(item.date_time == tuple(stamp) for item in infos)
    launcher = archive.getinfo("Game.sh")
    assert ((launcher.external_attr >> 16) & 0o7777) == 0o755
    manifest = archive.read("fixture/.nxrelease/MANIFEST.sha256").decode("utf-8")
    assert "fixture/.nxrelease/NXRELEASE-METADATA.json" in manifest
    assert "fixture/.nxrelease/MANIFEST.sha256" not in manifest
PY

# The immutable public ceiling cannot be raised.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/ceiling.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["release"]["max_glibc"] = "2.31"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail ceiling 'public ceiling' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/ceiling.json"

# A current-host/current-glibc profile is rejected even for a static ELF.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/current.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["build_profile"] = "nextos-current"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail current 'current-host|build_profile' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/current.json"

# NXExtract is a content pin, not just a filename/version claim.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxhash.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["nxextract"]["sha256"] = "0" * 64
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxhash 'NXExtract|pin' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxhash.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxfloor.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["nxextract"]["minimum_version"] = "1.2.1"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxfloor 'below tool floor' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxfloor.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxmissing.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["files"] = [
    item for item in data["files"]
    if item["target"] != "fixture/nxextract/nxextract-runtime-env.sh"
]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxmissing 'runtime_env_path|runtime helper' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxmissing.json"

printf '%s\n' '{"schema":2,"id":"fixture","extract":[],"validate":[],"commit":[]}' \
  >"$TEST_TMP/source/bad-extractor.json"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/nxrecipe.json" \
  "$TEST_TMP/source/bad-extractor.json" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["nxextract"]["recipe_sha256"] = digest
entry = next(item for item in data["files"] if item["kind"] == "nxextract-recipe")
entry["source"] = "bad-extractor.json"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail nxrecipe 'recipe schema must be 1' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/nxrecipe.json"

# No ELF may hide in a generic payload tree.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/unclassified.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["kind"] = "payload"
entry.pop("architecture")
entry.pop("build_profile")
entry.pop("provenance")
entry.pop("needed")
entry.pop("soname")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail unclassified 'ELF.*unclassified' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/unclassified.json"

# DT_NEEDED is an exact per-ELF contract, not an informational report.
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/needed.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["needed"] = ["libc.so.6"]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail needed 'DT_NEEDED differs' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/needed.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/soname.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["soname"] = "libfixture.so"
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail soname 'DT_SONAME differs' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/soname.json"

# Every readelf phase is mandatory and loadability/ABI/interpreter/search paths
# are hard gates, not best-effort reports.
cat >"$TEST_TMP/check-elf.py" <<'PY'
import importlib.util
import pathlib
import sys

tool, elf_path, arch = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_test_module", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
try:
    module.elf_information(
        pathlib.Path(elf_path), pathlib.Path(elf_path).name,
        "project-linux", arch, "2.30", "universal-low-glibc", [], None,
    )
except module.ReleaseError as error:
    print("NXRELEASE EXPECTED FAIL: {}".format(error), file=sys.stderr)
    raise SystemExit(1)
raise SystemExit(0)
PY
expect_fail elf-rel 'ET_EXEC/ET_DYN|non-loadable type' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/reloc.o" aarch64
expect_fail elf-no-load 'no PT_LOAD' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/no-load" aarch64
expect_fail elf-class 'class.*disagrees|ELF32' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/class-mismatch" aarch64
expect_fail elf-softfp 'soft-float|hard-float' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/arm-softfp" armv7
expect_fail elf-interp 'PT_INTERP must be exactly' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/wrong-interp" aarch64
expect_fail elf-runpath 'RPATH/RUNPATH' \
  python3 "$TEST_TMP/check-elf.py" "$TOOL" "$TEST_TMP/source/with-runpath" aarch64

mkdir -p "$TEST_TMP/fakebin"
cat >"$TEST_TMP/fakebin/readelf" <<SH
#!/usr/bin/env bash
if [ "\${1:-}" = "\${NX_TEST_READELF_FAIL_ARG:--lW}" ]; then
  printf 'forced readelf phase failure: %s\n' "\${1:-}" >&2
  exit 9
fi
exec "$(command -v readelf)" "\$@"
SH
chmod 0755 "$TEST_TMP/fakebin/readelf"
expect_fail readelf-phase 'readelf -lW rejected|forced program-header' \
  env PATH="$TEST_TMP/fakebin:$PATH" \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"
expect_fail readelf-dynamic 'readelf -dW rejected|forced readelf phase' \
  env PATH="$TEST_TMP/fakebin:$PATH" NX_TEST_READELF_FAIL_ARG=-dW \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"
expect_fail readelf-versions 'readelf --version-info --wide rejected|forced readelf phase' \
  env PATH="$TEST_TMP/fakebin:$PATH" NX_TEST_READELF_FAIL_ARG=--version-info \
  python3 "$TOOL" validate --manifest "$TEST_TMP/manifest.json"

# DT_NEEDED closes over an explicit (namespace, ABI, SONAME) provider map.
cp "$TEST_TMP/source/libdep.so" "$TEST_TMP/source/libdep2.so"
cat >"$TEST_TMP/dependency-manifest.py" <<'PY'
import hashlib
import json
import os
import sys

base, output, source_root, mode = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)

def digest(relative):
    with open(os.path.join(source_root, relative), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()

def elf_entry(source, target, needed, soname):
    return {
        "source": source,
        "target": target,
        "kind": "third-party-linux",
        "mode": "0755",
        "architecture": "aarch64",
        "build_profile": "universal-low-glibc",
        "provenance": "offline dependency closure fixture",
        "sha256": digest(source),
        "needed": needed,
        "soname": soname,
    }

if mode != "bad-name":
    data["files"].append(elf_entry(
        "consumer", "fixture/tests/consumer", ["libdep.so"], None,
    ))

if mode == "external":
    data["dependencies"] = [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "libdep.so", "provider": "portmaster",
    }]
elif mode in ("package", "duplicate", "two-package"):
    data["files"].append(elf_entry(
        "libdep.so", "fixture/lib/aarch64/libdep.so", [], "libdep.so",
    ))
    data["dependencies"] = [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "libdep.so",
        "provider": "package" if mode != "duplicate" else "portmaster",
        **({"path": "fixture/lib/aarch64/libdep.so"}
           if mode != "duplicate" else {}),
    }]
    if mode == "two-package":
        data["files"].append(elf_entry(
            "libdep2.so", "fixture/lib/aarch64/libdep-copy.so",
            [], "libdep.so",
        ))
elif mode == "bad-glibc":
    data["dependencies"] = [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "libdep.so", "provider": "glibc-base",
    }]
elif mode == "bad-name":
    data["dependencies"] = [{
        "namespace": "linux", "architecture": "aarch64",
        "soname": "../libevil.so", "provider": "firmware",
    }]

with open(output, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY

for dependency_mode in unresolved external package duplicate two-package bad-glibc bad-name; do
  python3 "$TEST_TMP/dependency-manifest.py" \
    "$TEST_TMP/manifest.json" "$TEST_TMP/dependency-$dependency_mode.json" \
    "$TEST_TMP/source" "$dependency_mode"
done
expect_fail dep-unresolved 'unresolved DT_NEEDED.*libdep[.]so' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-unresolved.json"
python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-external.json" \
  | grep -q 'NXRELEASE VALIDATE: PASS' || fail 'explicit external provider failed'
python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-package.json" \
  | grep -q 'NXRELEASE VALIDATE: PASS' || fail 'explicit package provider failed'
python3 "$TOOL" stage --manifest "$TEST_TMP/dependency-external.json" \
  --stage "$TEST_TMP/dependency-stage" \
  | grep -q 'NXRELEASE STAGE: PASS' || fail 'dependency metadata round-trip failed'
python3 "$TOOL" stage --manifest "$TEST_TMP/dependency-package.json" \
  --stage "$TEST_TMP/dependency-package-stage" \
  | grep -q 'NXRELEASE STAGE: PASS' || fail 'package-provider metadata round-trip failed'
expect_fail dep-duplicate 'duplicate providers' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-duplicate.json"
expect_fail dep-package-duplicate 'duplicate packaged ELF provider' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-two-package.json"
expect_fail dep-glibc-label 'cannot label.*glibc-base' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-bad-glibc.json"
expect_fail dep-portable-name 'portable ELF library basename' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dependency-bad-name.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/not-glibc-base.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["dependencies"] = [
    {"namespace": "linux", "architecture": "aarch64",
     "soname": "libgcc_s.so.1", "provider": "glibc-base"},
    {"namespace": "linux", "architecture": "aarch64",
     "soname": "libstdc++.so.6", "provider": "glibc-base"},
]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail dep-cxx-not-base 'libgcc_s[.]so[.]1.*glibc-base|cannot label' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/not-glibc-base.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source" \
  "$TEST_TMP/android-package.json" "$TEST_TMP/android-mislabelled.json" <<'PY'
import hashlib, json, os, sys
base, root, output, mislabelled = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)
with open(os.path.join(root, "android-game.so"), "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["files"].append({
    "source": "android-game.so",
    "target": "fixture/android/libgame.so",
    "kind": "android-upstream",
    "mode": "0644",
    "architecture": "aarch64",
    "provenance": "offline original Android namespace fixture",
    "sha256": digest,
    "needed": ["liblog.so"],
    "soname": "libgame.so",
})
data["dependencies"] = [{
    "namespace": "android", "architecture": "aarch64",
    "soname": "liblog.so", "provider": "nxloader-import-registry",
}]
with open(output, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
data["files"][-1]["kind"] = "third-party-linux"
data["files"][-1]["build_profile"] = "universal-low-glibc"
data["dependencies"] = [{
    "namespace": "linux", "architecture": "aarch64",
    "soname": "liblog.so", "provider": "firmware",
}]
with open(mislabelled, "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail android-byo 'kind is unsupported.*android-upstream' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/android-package.json"
expect_fail android-mislabelled 'Android/Bionic dependencies.*cannot be packaged as Linux' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/android-mislabelled.json"

# Declared PortMaster metadata is parsed and tied to the actual wrapper path.
printf '%s\n' \
  '<?xml version="1.0" encoding="utf-8"?>' \
  '<gameList><game><path>./Wrong.sh</path><name>Broken</name></game></gameList>' \
  >"$TEST_TMP/source/bad-gameinfo.xml"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/bad-gameinfo.json" "$TEST_TMP/source/bad-gameinfo.xml" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["portmaster_metadata"]["gameinfo_xml"]["sha256"] = digest
entry = next(item for item in data["files"] if item["target"].endswith("/gameinfo.xml"))
entry["source"] = "bad-gameinfo.xml"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail gameinfo 'gameinfo.xml path does not match' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/bad-gameinfo.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source" \
  "$TEST_TMP/items-double.json" "$TEST_TMP/items-file-slash.json" <<'PY'
import hashlib, json, os, sys
base, root, double_manifest, file_manifest = sys.argv[1:]
with open(base, encoding="utf-8") as handle:
    original = json.load(handle)

def variant(items, source_name, output):
    data = json.loads(json.dumps(original))
    with open(os.path.join(root, "port.json"), encoding="utf-8") as handle:
        port_json = json.load(handle)
    port_json["items"] = items
    source_path = os.path.join(root, source_name)
    with open(source_path, "w", encoding="utf-8") as handle:
        json.dump(port_json, handle, sort_keys=True, indent=2)
        handle.write("\n")
    with open(source_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    data["portmaster_metadata"]["port_json"]["sha256"] = digest
    entry = next(item for item in data["files"] if item["target"].endswith("/port.json"))
    entry["source"] = source_name
    entry["sha256"] = digest
    with open(output, "w", encoding="utf-8") as handle:
        json.dump(data, handle, sort_keys=True, indent=2)

variant(["Game.sh", "fixture//"], "port-double.json", double_manifest)
variant(["Game.sh/", "fixture/"], "port-file-slash.json", file_manifest)
PY
expect_fail port-item-double 'more than one trailing' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/items-double.json"
expect_fail port-item-file 'uses ./. but is not a directory|not a directory' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/items-file-slash.json"

# Use a real host-linked executable with e_machine relabeled AArch64 so the
# test exercises version-info without needing a cross compiler.
if command -v cc >/dev/null 2>&1; then
  printf '%s\n' 'int main(void) { return 0; }' >"$TEST_TMP/host.c"
  cc "$TEST_TMP/host.c" -o "$TEST_TMP/source/high-glibc"
  python3 - "$TEST_TMP/source/high-glibc" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as handle:
    handle.seek(18)
    handle.write(struct.pack("<H", 183))
PY
  python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/high.json" <<'PY'
import hashlib, json, re, subprocess, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
entry = next(item for item in data["files"] if item["target"].endswith("/loader"))
entry["source"] = "high-glibc"
with open(sys.argv[1].rsplit("/", 1)[0] + "/source/high-glibc", "rb") as handle:
    entry["sha256"] = hashlib.sha256(handle.read()).hexdigest()
dynamic = subprocess.check_output(
    ["readelf", "-dW", sys.argv[1].rsplit("/", 1)[0] + "/source/high-glibc"],
    text=True, stderr=subprocess.STDOUT,
)
entry["needed"] = sorted(set(re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic)))
sonames = sorted(set(re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)))
entry["soname"] = sonames[0] if sonames else None
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
  if readelf --version-info --wide "$TEST_TMP/source/high-glibc" 2>/dev/null | grep -q 'GLIBC_'; then
    expect_fail glibc 'requires GLIBC_' \
      python3 "$TOOL" validate --manifest "$TEST_TMP/high.json" --max-glibc 0.1
  fi
fi

# PortMaster integration is a release contract, not launcher folklore.
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'source "${controlfolder}/control.txt"' \
  'get_controls' \
  '"${directory}/pm_platform_helper" "${GAMEDIR}/game"' \
  >"$TEST_TMP/source/Bad.sh"
chmod 0755 "$TEST_TMP/source/Bad.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/bad-launcher.json" "$TEST_TMP/source/Bad.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["package"]["launcher"] = "Bad.sh"
data["package"]["launcher_chain"][0] = "Bad.sh"
data["files"][0]["source"] = "Bad.sh"
data["files"][0]["target"] = "Bad.sh"
with open(sys.argv[3], "rb") as handle:
    data["files"][0]["sha256"] = hashlib.sha256(handle.read()).hexdigest()
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail portmaster 'canonical|nxbootstrap config|launcher_name' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/bad-launcher.json"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'case x in' \
  '  x) true & ;;' \
  'esac' \
  >"$TEST_TMP/source/background.sh"
chmod 0755 "$TEST_TMP/source/background.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/background.json" \
  "$TEST_TMP/source/background.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
data["files"].append({
    "source": "background.sh", "target": "fixture/background.sh",
    "kind": "script", "mode": "0755", "sha256": digest,
})
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail background-case 'backgrounds a child' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/background.json"

python3 - "$TEST_TMP/source/Game.sh" "$TEST_TMP/source/wrong-exec.sh" <<'PY'
import sys
text = open(sys.argv[1], encoding="utf-8").read()
assert "flock -n 9" in text
text = text.replace("flock -n 9", "true", 1)
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    handle.write(text)
PY
chmod 0755 "$TEST_TMP/source/wrong-exec.sh"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/wrong-exec.json" \
  "$TEST_TMP/source/wrong-exec.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
entry = next(item for item in data["files"] if item["kind"] == "launcher")
entry["source"] = "wrong-exec.sh"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail launcher-exec 'self-contained launcher is missing' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/wrong-exec.json"

python3 - "$TEST_TMP/source/Game.sh" \
  "$TEST_TMP/source/dead-portmaster.sh" <<'PY'
import sys
text = open(sys.argv[1], encoding="utf-8").read()
text = text.replace("control.txt", "control.dead")
text = text.replace("get_controls", "ignored_controls")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    handle.write(text)
PY
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/dead-portmaster.json" \
  "$TEST_TMP/source/dead-portmaster.sh" <<'PY'
import hashlib, json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
with open(sys.argv[3], "rb") as handle:
    digest = hashlib.sha256(handle.read()).hexdigest()
entry = next(item for item in data["files"] if item["kind"] == "launcher")
entry["source"] = "dead-portmaster.sh"
entry["sha256"] = digest
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail launcher-dead-token 'self-contained launcher is missing|missing control' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/dead-portmaster.json"

# Adversarial payload scans: the gate rejects proprietary/temp suffixes
# (.jar), saves/tmp directory parts, embedded credential literals, funding
# advocacy and FUNDING.* members; and it refuses a license pinned to a
# non-license file.
printf 'evil jar payload\n' > "$TEST_TMP/source/evil.jar"
printf 'save data\n' > "$TEST_TMP/source/save-secret"
printf 'password=supersecret123\n' > "$TEST_TMP/source/secret.txt"
printf 'please donate via patreon\n' > "$TEST_TMP/source/donate.txt"
printf 'github: patreon\n' > "$TEST_TMP/source/FUNDING.yml"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source" \
  "$TEST_TMP/forbidden-jar.json" "$TEST_TMP/forbidden-saves.json" \
  "$TEST_TMP/forbidden-secret.json" "$TEST_TMP/forbidden-advocacy.json" \
  "$TEST_TMP/forbidden-funding.json" "$TEST_TMP/license-bad.json" <<'PY'
import copy, hashlib, json, os, sys
base, source_root = sys.argv[1], sys.argv[2]
jar_out, saves_out, secret_out, advocacy_out, funding_out, license_out = sys.argv[3:9]
with open(base, encoding="utf-8") as handle:
    data = json.load(handle)

def digest(rel):
    with open(os.path.join(source_root, rel), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()

def with_extra(source_rel, target):
    d = copy.deepcopy(data)
    d["files"].append({
        "source": source_rel, "target": target,
        "kind": "license-notice", "mode": "0644", "sha256": digest(source_rel),
    })
    return d

specs = {
    jar_out: with_extra("evil.jar", "fixture/evil.jar"),
    saves_out: with_extra("save-secret", "fixture/saves/save-secret"),
    secret_out: with_extra("secret.txt", "fixture/secret.txt"),
    advocacy_out: with_extra("donate.txt", "fixture/donate.txt"),
    funding_out: with_extra("FUNDING.yml", "fixture/FUNDING.yml"),
}
for path, payload in specs.items():
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, sort_keys=True, indent=2)
        handle.write("\n")

bad = copy.deepcopy(data)
bad["package"]["license"]["file"] = "fixture/nxextract/run-extractor.sh"
with open(license_out, "w", encoding="utf-8") as handle:
    json.dump(bad, handle, sort_keys=True, indent=2)
    handle.write("\n")
PY

expect_fail forbidden-jar 'proprietary/temp/log suffix|forbidden release data' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-jar.json"
expect_fail forbidden-saves 'private/temp/cache data' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-saves.json"
expect_fail forbidden-secret 'credential/secret' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-secret.json"
expect_fail forbidden-advocacy 'funding/donations' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-advocacy.json"
expect_fail forbidden-funding 'forbidden release artifact|funding/donations' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-funding.json"
expect_fail license-bad 'license.file must match a license-notice' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/license-bad.json"

python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/license-missing.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
data["package"].pop("license")
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail license-missing 'package.license is required' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/license-missing.json"

printf '%s\n' 'hostname=private-build-node' > "$TEST_TMP/source/hostname.txt"
python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source/hostname.txt" \
  "$TEST_TMP/forbidden-hostname.json" <<'PY'
import hashlib, json, pathlib, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
source = pathlib.Path(sys.argv[2])
data["files"].append({
    "source": source.name,
    "target": "fixture/hostname.txt",
    "kind": "payload",
    "mode": "0644",
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
})
with open(sys.argv[3], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
expect_fail forbidden-hostname 'hostname literal|private host information' \
  python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-hostname.json"

for android_suffix in apk obb dex; do
  python3 - "$TEST_TMP/manifest.json" "$TEST_TMP/source/payload/README.txt" \
    "$TEST_TMP/forbidden-$android_suffix.json" "$android_suffix" <<'PY'
import hashlib, json, pathlib, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
source = pathlib.Path(sys.argv[2])
suffix = sys.argv[4]
data["files"].append({
    "source": "payload/README.txt",
    "target": "fixture/data/game." + suffix,
    "kind": "payload",
    "mode": "0644",
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
})
with open(sys.argv[3], "w", encoding="utf-8") as handle:
    json.dump(data, handle, sort_keys=True, indent=2)
PY
  expect_fail "forbidden-$android_suffix" 'proprietary/temp/log suffix|forbidden release data' \
    python3 "$TOOL" validate --manifest "$TEST_TMP/forbidden-$android_suffix.json"
done

# Re-opening the artifact catches a payload changed without refreshing its
# internal MANIFEST.sha256.
python3 - "$TEST_TMP/fixture-one.zip" "$TEST_TMP/tampered.zip" <<'PY'
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1], "r") as source:
    with zipfile.ZipFile(sys.argv[2], "w") as target:
        for info in source.infolist():
            data = source.read(info.filename)
            if info.filename == "fixture/assets/README.txt":
                data += b"tampered\n"
            target.writestr(info, data)
PY
expect_fail tamper 'MANIFEST.sha256 verification failed|hash mismatch' \
  python3 "$TOOL" verify --archive "$TEST_TMP/tampered.zip"

# Adversarial ZIP members must fail before extraction or metadata trust: path
# traversal, Unix symlink entries and NFC/case-fold collisions are independent
# attack classes.
python3 - "$TEST_TMP/fixture-one.zip" "$TEST_TMP" <<'PY'
import shutil
import stat
import sys
import zipfile

source, root = sys.argv[1:]

traversal = root + "/adversarial-traversal.zip"
shutil.copyfile(source, traversal)
with zipfile.ZipFile(traversal, "a") as archive:
    archive.writestr("../escape", b"escape")

symlink = root + "/adversarial-symlink.zip"
shutil.copyfile(source, symlink)
with zipfile.ZipFile(symlink, "a") as archive:
    info = zipfile.ZipInfo("fixture/hostile-link")
    info.create_system = 3
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    archive.writestr(info, "../../outside")

collision = root + "/adversarial-unicode.zip"
shutil.copyfile(source, collision)
with zipfile.ZipFile(collision, "a") as archive:
    for name in ("fixture/Caf\u00e9.txt", "fixture/Cafe\u0301.txt"):
        info = zipfile.ZipInfo(name)
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | 0o644) << 16
        archive.writestr(info, b"collision")
PY
expect_fail zip-traversal 'ZIP member.*safe relative path|not a safe relative path' \
  python3 "$TOOL" verify --archive "$TEST_TMP/adversarial-traversal.zip"
expect_fail zip-symlink 'archive contains symlink' \
  python3 "$TOOL" verify --archive "$TEST_TMP/adversarial-symlink.zip"
expect_fail zip-unicode 'case-insensitive collision' \
  python3 "$TOOL" verify --archive "$TEST_TMP/adversarial-unicode.zip"

# A source changed after validation but before copy is caught by the staged
# hash. The barrier makes the race deterministic instead of timing-dependent.
python3 - "$TOOL" "$TEST_TMP/manifest.json" "$TEST_TMP/toctou-stage" <<'PY'
import importlib.util
import pathlib
import sys
import threading

tool, manifest, destination = sys.argv[1:]
spec = importlib.util.spec_from_file_location("nxrelease_toctou", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
config = module.load_manifest(manifest)
payload = pathlib.Path(manifest).parent / "source" / "payload" / "README.txt"
original_payload = payload.read_bytes()
real_copy = module.shutil.copyfile
copy_reached = threading.Event()
continue_copy = threading.Event()
errors = []

def barrier_copy(source, target):
    if pathlib.Path(source) == payload:
        copy_reached.set()
        if not continue_copy.wait(10):
            raise RuntimeError("TOCTOU test barrier timed out")
    return real_copy(source, target)

def worker():
    try:
        module.stage_release(config, destination)
    except BaseException as error:
        errors.append(error)

module.shutil.copyfile = barrier_copy
thread = threading.Thread(target=worker)
thread.start()
if not copy_reached.wait(10):
    raise SystemExit("copy barrier was never reached")
payload.write_bytes(original_payload + b"raced\n")
continue_copy.set()
thread.join(10)
payload.write_bytes(original_payload)
if thread.is_alive():
    raise SystemExit("stage worker did not finish")
if len(errors) != 1 or not isinstance(errors[0], module.ReleaseError):
    raise SystemExit("TOCTOU mutation was not rejected: {!r}".format(errors))
if "changed" not in str(errors[0]) and "pin" not in str(errors[0]):
    raise SystemExit("unexpected TOCTOU error: {}".format(errors[0]))
if pathlib.Path(destination).exists():
    raise SystemExit("TOCTOU failure published a stage")
PY

# Pair publication installs checksum first, uses hard-link O_EXCL semantics and
# rolls back only its own inode if either final name loses a race.
python3 - "$TOOL" "$TEST_TMP" <<'PY'
import importlib.util
import pathlib
import sys

tool, root_value = sys.argv[1:]
root = pathlib.Path(root_value)
spec = importlib.util.spec_from_file_location("nxrelease_publish", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def run_race(label, collide_on):
    archive_temp = root / (label + "-archive.tmp")
    checksum_temp = root / (label + "-checksum.tmp")
    output = root / (label + ".zip")
    checksum_output = root / (label + ".zip.sha256")
    archive_temp.write_bytes(b"candidate archive")
    checksum_temp.write_bytes(b"candidate checksum")
    real_link = module.os.link
    competitor = b"concurrent owner"

    def racing_link(source, destination):
        destination_path = pathlib.Path(destination)
        if destination_path == collide_on(output, checksum_output):
            destination_path.write_bytes(competitor)
        return real_link(source, destination)

    module.os.link = racing_link
    try:
        try:
            module.publish_archive_pair(
                archive_temp, checksum_temp, output, checksum_output,
            )
        except module.ReleaseError:
            pass
        else:
            raise SystemExit(label + " race unexpectedly published")
    finally:
        module.os.link = real_link

    collision_path = collide_on(output, checksum_output)
    if collision_path.read_bytes() != competitor:
        raise SystemExit(label + " overwrote/deleted the concurrent destination")
    other = checksum_output if collision_path == output else output
    if other.exists():
        raise SystemExit(label + " left a partial release pair")

run_race("race-archive", lambda archive, checksum: archive)
run_race("race-checksum", lambda archive, checksum: checksum)

bundle_destination = root / "race-bundle"
real_rename = module.rename_noreplace

def racing_rename(source, destination):
    pathlib.Path(destination).mkdir()
    (pathlib.Path(destination) / "owner.txt").write_bytes(b"concurrent bundle")
    return real_rename(source, destination)

module.rename_noreplace = racing_rename
try:
    try:
        module.create_release_bundle(
            root / "stage-one", bundle_destination, "fixture.zip",
        )
    except module.ReleaseError:
        pass
    else:
        raise SystemExit("bundle destination race unexpectedly published")
finally:
    module.rename_noreplace = real_rename
if (bundle_destination / "owner.txt").read_bytes() != b"concurrent bundle":
    raise SystemExit("bundle race overwrote the concurrent directory")
if list(root.glob(".nxrelease-bundle-*")):
    raise SystemExit("bundle race leaked a hidden publication directory")
PY

before_hash=$(sha256sum "$TEST_TMP/fixture-one.zip")
expect_fail no-overwrite 'already exists' \
  python3 "$TOOL" build --manifest "$TEST_TMP/manifest.json" \
    --stage "$TEST_TMP/unused-stage" --output "$TEST_TMP/fixture-one.zip"
after_hash=$(sha256sum "$TEST_TMP/fixture-one.zip")
[ "$before_hash" = "$after_hash" ] || fail 'existing publication was overwritten'
[ ! -e "$TEST_TMP/unused-stage" ] || fail 'preflight collision still created a stage'

printf '%s\n' 'nxrelease tests: PASS'
