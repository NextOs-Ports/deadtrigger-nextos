#!/usr/bin/env python3
"""Process-free closure audit for M14."""

from __future__ import print_function

import hashlib
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
LEDGER = ROOT / "framework/nxaudio/references/m14-audio-contract-v1.json"
MATRIX = ROOT / "framework/tests/test-matrix-v1.json"
SAFE_RUNNER = ROOT / "framework/tests/run-safe-gates.sh"
MASTER = pathlib.Path(
    "/mnt/ARQUIVOS/TRABALHO CLAUDE CODE/03-PORTS-E-RECEITAS/projetos/"
    "FRAMEWORK-UNIVERSAL-PLANO-MESTRE-2026-08-08.md"
)


def fail(message):
    raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                fail("duplicate JSON key in %s: %s" % (path, key))
            result[key] = value
        return result

    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle, object_pairs_hook=no_duplicates)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_reference(reference):
    match = re.fullmatch(r"([^:]+):(\d+)", reference)
    if not match:
        fail("invalid evidence reference: %s" % reference)
    relative = match.group(1)
    line = int(match.group(2))
    if relative.startswith("/") or ".." in pathlib.PurePosixPath(relative).parts:
        fail("unsafe evidence path: %s" % relative)
    path = ROOT / relative
    if not path.is_file() or path.is_symlink():
        fail("missing or symlinked evidence: %s" % relative)
    lines = path.read_text(encoding="utf-8", errors="strict").splitlines()
    if line < 1 or line > len(lines) or not lines[line - 1].strip():
        fail("invalid evidence line: %s" % reference)


def check_matrix():
    matrix = load_json(MATRIX)
    gates = {gate["id"]: gate for gate in matrix["gates"]}
    host = gates.get("nxaudio-m14-host")
    audit = gates.get("nxaudio-m14-audit")
    if not host or host.get("class") != "filesystem" or not host.get("automatic"):
        fail("nxaudio-m14-host is not an automatic filesystem gate")
    if host.get("command") != ["bash", "framework/nxaudio/tests/run-host.sh"]:
        fail("wrong nxaudio-m14-host command")
    if host.get("sources") != ["framework/nxaudio/tests/test_nxaudio.c"]:
        fail("wrong nxaudio-m14-host source set")
    if not audit or audit.get("class") != "pure" or not audit.get("automatic"):
        fail("nxaudio-m14-audit is not an automatic pure gate")
    if audit.get("command") != [
        "python3", "-B", "framework/nxaudio/tests/test_m14_audio_contract.py"
    ]:
        fail("wrong nxaudio-m14-audit command")
    safe = SAFE_RUNNER.read_text(encoding="utf-8")
    for token in ("run_gate nxaudio-m14-host", "run_gate nxaudio-m14-audit"):
        if token not in safe:
            fail("safe runner missing %s" % token)


def main():
    data = load_json(LEDGER)
    if data.get("schema_version") != 1 or data.get("contract") != "nxaudio-m14-v1":
        fail("wrong M14 schema/contract")
    closure = data.get("closure", {})
    expected_closure = {
        "requirements": 20,
        "completed": 20,
        "master_complete": True,
        "host_gate_closed": True,
        "physical_device_evidence_in_m14_run": False,
        "imported_approved_physical_evidence": True,
    }
    if closure != expected_closure:
        fail("M14 closure is not final and honest")

    items = data.get("requirements", [])
    expected_ids = ["M14-%03d" % index for index in range(1, 21)]
    if [item.get("id") for item in items] != expected_ids:
        fail("M14 requirements are not exact/in order")
    if any(item.get("status") != "verified" for item in items):
        fail("not every M14 requirement is verified")
    for item in items:
        evidence = item.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            fail("missing evidence for %s" % item["id"])
        for reference in evidence:
            check_reference(reference)

    expected_stacks = [
        ("sdl2", "nxcompat-sdl2-audio-v2"),
        ("opensl-es", "tasm2-opensl-sdl-v1"),
        ("opensl-es", "castle-opensl-sdl-v1"),
        ("aaudio", None),
        ("openal", "bully2-openal-v1"),
        ("fmod", "horizon-fmod-sdl-v1"),
        ("fmod-ex", "castle-fmodex-v1"),
        ("wwise", "sor4-wwise-openal-glibc230-v1"),
    ]
    stacks = [(entry.get("id"), entry.get("contract")) for entry in data["stacks"]]
    if stacks != expected_stacks:
        fail("audio stack matrix drift")
    if [entry.get("port") for entry in data["approved_physical_evidence"]] != [
        "horizonchase", "asm2_127", "sor4", "castleofillusion"
    ]:
        fail("approved physical cross-check drift")
    for entry in data["approved_physical_evidence"]:
        for reference in entry["refs"]:
            check_reference(reference)

    pins = data.get("evidence_pins")
    if not isinstance(pins, dict) or not pins:
        fail("M14 evidence pins are empty")
    for relative, expected in pins.items():
        path = ROOT / relative
        if not re.fullmatch(r"[0-9a-f]{64}", expected or ""):
            fail("invalid pin for %s" % relative)
        if not path.is_file() or sha256(path) != expected:
            fail("hash mismatch: %s" % relative)

    header = (ROOT / "framework/nxaudio/include/nxaudio.h").read_text(encoding="utf-8")
    source = (ROOT / "framework/nxaudio/src/nxaudio.c").read_text(encoding="utf-8")
    test = (ROOT / "framework/nxaudio/tests/test_nxaudio.c").read_text(encoding="utf-8")
    readme = (ROOT / "framework/nxaudio/README.md").read_text(encoding="utf-8")
    required_tokens = (
        "#define NXAUDIO_API_VERSION 1u",
        "single-producer/single-consumer",
        "NXAUDIO_REASON_SERVER_UNAVAILABLE",
        "NXAUDIO_REASON_MIXER_STARVED",
        "NXAUDIO_STACK_AAUDIO",
        "NXAUDIO_EVIDENCE_IMPORTED_APPROVED_PHYSICAL",
    )
    joined = header + source + test + readme
    for token in required_tokens:
        if token not in joined:
            fail("missing M14 contract token: %s" % token)
    if "pthread_mutex" in source or "SDL_OpenAudioDevice" in source:
        fail("nxaudio core gained a lock or real device open")
    if "generic-aaudio" not in test or "NXAUDIO_UNSUPPORTED" not in test:
        fail("AAudio fail-closed regression missing")

    master = MASTER.read_text(encoding="utf-8")
    for item_id in expected_ids:
        if "- [x] `%s`" % item_id not in master:
            fail("master checkbox is not closed: %s" % item_id)

    check_matrix()

    public_text = "\n".join(
        (ROOT / relative).read_text(encoding="utf-8", errors="strict")
        for relative in pins
        if relative.startswith("framework/nxaudio/") and "/tests/" not in relative
    )
    forbidden = (r"192\.168\.", r"10\.\d+\.\d+\.\d+", r"/home/", "token=")
    for pattern in forbidden:
        if re.search(pattern, public_text, re.IGNORECASE):
            fail("privacy token in nxaudio public files: %s" % pattern)

    print(
        "nxaudio_m14_audit=PASS requirements=20 stacks=8 "
        "physical_device_evidence_in_m14_run=0 imported_approved_physical=1 "
        "process_free=1"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, KeyError, ValueError, OSError, json.JSONDecodeError) as error:
        print("nxaudio_m14_audit=FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
