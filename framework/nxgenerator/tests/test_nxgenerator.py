#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Filesystem-only regression gate for the deterministic M19 generator."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
TOOL = ROOT / "nxgenerator.py"
BOOTSTRAP_ROOT = REPOSITORY / "framework" / "nxbootstrap"
EXPECTED_BOOTSTRAP_VERSION = (
    (BOOTSTRAP_ROOT / "VERSION").read_text(encoding="utf-8").strip()
)
BOOTSTRAP_GENERATOR = BOOTSTRAP_ROOT / "tools" / "generate-port.py"
BOOTSTRAP_LAUNCHER_TEMPLATE = BOOTSTRAP_ROOT / "templates" / "launcher.sh.in"
NXEXTRACT = (
    REPOSITORY / "suportando_outros_devices" / "extrator-universal"
)
EXAMPLES = (
    (ROOT / "examples" / "nxproject-aarch64.example.json",
     "nxexample-aarch64", "NXExample AArch64.sh", "aarch64"),
    (ROOT / "examples" / "nxproject-armv7.example.json",
     "nxexample-armv7", "NXExample ARMv7.sh", "armv7"),
)
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])"
)
LINK_RE = re.compile(r"\[[^]]+\]\(([^)]+)\)")


class GateError(Exception):
    pass


def load_tool_module():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_under_test", TOOL
    )
    require(specification is not None and specification.loader is not None,
            "cannot load nxgenerator for focused deployment checks")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise GateError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_generator(manifest, output, expected=0):
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-B", str(TOOL), str(manifest),
         "--output", str(output)],
        cwd=str(REPOSITORY),
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    require(result.returncode == expected,
            "generator status %d != %d: %s" %
            (result.returncode, expected, result.stderr.strip()))
    return result


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), "generated tree contains a symlink")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = (
                stat.S_IMODE(path.stat().st_mode), path.read_bytes()
            )
    return result


def validate_links(readme):
    text = readme.read_text(encoding="utf-8")
    for target in LINK_RE.findall(text):
        if target.startswith(("#", "http://", "https://")):
            continue
        require((readme.parent / target).is_file(),
                "generated README link is missing: %s" % target)


def validate_generation(output, port_id, launcher_name, architecture):
    port = output / port_id
    launcher_path = output / launcher_name
    nxport = json.loads(
        (port / "nxport.json").read_text(encoding="utf-8")
    )
    require(launcher_path.is_file() and not launcher_path.is_symlink(),
            "wrapper is absent or unsafe")
    for retired in ("run.sh", "nxbootstrap.sh",
                    "nxbootstrap-%s.sh" % EXPECTED_BOOTSTRAP_VERSION,
                    "nxdeployment.json"):
        require(not (port / retired).exists() and
                not (output / retired).exists(),
                "generator retained the retired artifact %s" % retired)
    require(stat.S_IMODE(launcher_path.stat().st_mode) == 0o755,
            "wrapper mode changed")

    scripts = [launcher_path]
    if nxport["nxextract"]["mode"] != "no":
        scripts.extend((
            port / "nxextract" / "run-extractor.sh",
            port / "nxextract" / "nxextract-runtime-env.sh",
        ))
    for script in scripts:
        checked = subprocess.run(
            ["bash", "-n", str(script)], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        require(checked.returncode == 0,
                "generated shell syntax failed: %s" % script.name)

    wrapper = launcher_path.read_text(encoding="utf-8")
    require("nxbootstrap %s" % EXPECTED_BOOTSTRAP_VERSION in wrapper,
            "launcher does not record its generator version")
    require(not re.search(r"@[A-Z0-9_]+@", wrapper),
            "launcher has unresolved template tokens")
    expected_runtime_exports = (
        "export NXCOMPAT_PORT_ID=%s" % port_id,
        'export NXCOMPAT_GAME_DIR="$GAMEDIR"',
        "export NXCOMPAT_REQUIRED_CAPABILITIES=",
        "export NXCOMPAT_ENABLED_QUIRKS=",
        "export NXCOMPAT_RUNTIME_REPORT=%s" % nxport["runtime_report"],
    )
    for runtime_export in expected_runtime_exports:
        require(runtime_export in wrapper,
                "launcher lacks runtime export: %s" % runtime_export)
    required_assignment = "NXBOOTSTRAP_REQUIRED_FILES=%s" % \
        load_tool_module().BOOTSTRAP_GENERATOR.shell_join(
            nxport["required_files"]
        )
    require(wrapper.count("NXBOOTSTRAP_REQUIRED_FILES=") == 1 and
            wrapper.count(required_assignment) == 1,
            "launcher required-files assignment differs from nxport")
    require(wrapper.index(required_assignment) <
            wrapper.index('BIN="$GAMEDIR/%s"' % nxport["executable"]),
            "launcher required-files gate runs after BIN")
    requested_assignments = re.findall(
        r"(?m)^[ \t]*NXEXTRACT_REQUESTED=([01])$", wrapper
    )
    expected_requested = {
        "no": [], "yes": ["1"], "auto": ["0", "1"],
    }[nxport["nxextract"]["mode"]]
    require(requested_assignments == expected_requested,
            "launcher NXEXTRACT_REQUESTED mode semantics differ")
    for guarantee in ('exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
                      "NXBOOTSTRAP_LOCK_PATH_ID",
                      "NXBOOTSTRAP_LOCK_FD_ID",
                      '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0',
                      "nxbootstrap_abort_before_game 129",
                      "nxbootstrap_abort_before_game 130",
                      "nxbootstrap_abort_before_game 143",
                      "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
                      "flock -n 9",
                      'wait "$game_pid"',
                      "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                      'builtin kill -KILL "$game_pid"',
                      "trap '' INT TERM HUP",
                      "printf '\\033c'",
                      "nxbootstrap_finish"):
        require(guarantee in wrapper,
                "launcher lacks golden-port guarantee: %s" % guarantee)
    if architecture == "armv7":
        require('PORT_32BIT="Y"' in wrapper and
                "export PORT_32BIT" in wrapper,
                "ARMHF wrapper lost its literal")
    else:
        require('PORT_32BIT="Y"' not in wrapper,
                "AArch64 wrapper gained ARMHF metadata")

    if nxport["nxextract"]["mode"] == "no":
        require(not (port / "nxextract").exists() and
                not (port / "extractor.json").exists(),
                "disabled NXExtract project still vendors an integration")
    else:
        for name in ("nxextract.py", "run-extractor.sh",
                     "nxextract-runtime-env.sh"):
            require((port / "nxextract" / name).read_bytes() ==
                    (NXEXTRACT / name).read_bytes(),
                    "vendored NXExtract file differs: %s" % name)
        require((port / "extractor.json").read_bytes() ==
                (NXEXTRACT / "examples" / "recipe-minimal.json").read_bytes(),
                "NXExtract recipe is not byte-pinned")

    adapter = json.loads(
        (port / "adapter" / "adapter-contract.json").read_text(encoding="utf-8")
    )
    require(adapter["status"] == "unimplemented_nonrelease" and
            adapter["release_ready"] is False,
            "adapter skeleton overclaims readiness")
    require(adapter["lifecycle"] == {"sequence": [], "source_evidence": []},
            "adapter skeleton invented lifecycle")
    require(adapter["jni"]["callbacks"] == [] and
            adapter["persistence"]["callbacks"] == [] and
            adapter["terminal"]["action"] is None,
            "adapter skeleton invented callbacks or terminal behavior")

    metadata = json.loads((port / "port.json").read_text(encoding="utf-8"))
    expected_arch = "armhf" if architecture == "armv7" else "aarch64"
    require(metadata == {
        "version": 4,
        "name": port_id + ".zip",
        "items": [launcher_name, port_id + "/"],
        "items_opt": [],
        "attr": {
            "title": "NXExample ARMv7" if architecture == "armv7" else
                     "NXExample AArch64",
            "arch": [expected_arch],
            "min_glibc": "2.17",
        },
    }, "generated PortMaster metadata changed")

    readme = port / "README.md"
    readme_text = readme.read_text(encoding="utf-8")
    for token in (
        "## English", "## Português", "### Architecture",
        "### Arquitetura", "### Solved problems",
        "### Problemas resolvidos", "### Controls", "### Controles",
        "### Game data", "### Dados do jogo", "### Build and run",
        "### Compilar e executar", "### Source map", "### Mapa de fontes",
        "### Licenses", "### Licenças", "Mali-450/GLES2",
        "same ZIP and SHA-256", "mesmo ZIP público exato",
        "development-only", "somente ao desenvolvimento",
    ):
        require(token in readme_text, "generated README lacks: %s" % token)
    require(not IPV4_RE.search(readme_text) and "/home/" not in readme_text and
            "/Users/" not in readme_text,
            "generated README contains a private literal")
    validate_links(readme)

    receipt_path = port / "GENERATION.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    require(receipt["generator"] == {
        "name": "nxgenerator", "version": "0.2.0"
    }, "generator version is not pinned")
    expected_bootstrap_pin = {
        "version": EXPECTED_BOOTSTRAP_VERSION,
        "source_files": {
            "templates/launcher.sh.in": sha256(BOOTSTRAP_LAUNCHER_TEMPLATE),
            "tools/generate-port.py": sha256(BOOTSTRAP_GENERATOR),
        },
    }
    expected_nxextract_pin = receipt["source_pins"]["nxextract"]
    if nxport["nxextract"]["mode"] == "no":
        require(expected_nxextract_pin is None,
                "disabled NXExtract project retained a source pin")
    else:
        require(expected_nxextract_pin["version"] == "1.2.6",
                "vendored NXExtract version pin is not exact")
    require(receipt["source_pins"]["nxbootstrap"] == expected_bootstrap_pin,
            "vendored nxbootstrap source pins are not exact")
    require(receipt["claims"] == {
        "deterministic_scaffold": True,
        "release_ready": False,
        "physical_support_proven": False,
        "adapter_lifecycle_implemented": False,
    }, "generation receipt overclaims completion")
    expected_records = {}
    for path in sorted(output.rglob("*")):
        if path.is_file() and path != receipt_path:
            expected_records[path.relative_to(output).as_posix()] = {
                "path": path.relative_to(output).as_posix(),
                "mode": "%04o" % stat.S_IMODE(path.stat().st_mode),
                "sha256": sha256(path),
            }
    actual_records = {entry["path"]: entry for entry in receipt["artifacts"]}
    require(actual_records == expected_records,
            "generation artifact inventory is incomplete or stale")
    deployment_artifacts = {launcher_name, "%s/nxport.json" % port_id}
    require(deployment_artifacts <= set(actual_records),
            "generation inventory omits the launcher deployment set")
    require(actual_records[launcher_name]["sha256"] == sha256(launcher_path) and
            actual_records["%s/nxport.json" % port_id]["sha256"] ==
            sha256(port / "nxport.json"),
            "generation inventory contains stale deployment hashes")
    require((port / "LICENSE").read_bytes() ==
            (REPOSITORY / "LICENSE").read_bytes(),
            "license was not copied exactly")


def validate_deployment_tamper_rejection(clean, work, port_id, launcher_name):
    tool_module = load_tool_module()
    source_state = tool_module.bootstrap_source_state()
    nxport = json.loads(
        (clean / port_id / "nxport.json").read_text(encoding="utf-8")
    )

    def expect_rejected(label, mutate):
        target = work / ("tampered-" + label)
        shutil.copytree(clean, target)
        mutate(target)
        try:
            tool_module.validate_bootstrap_deployment(
                target, nxport, source_state
            )
        except tool_module.ProjectError:
            return
        raise GateError(
            "nxgenerator accepted a tampered deployment: %s" % label
        )

    def resurrect_library(root):
        (root / port_id / "nxbootstrap.sh").write_text(
            "#!/bin/bash\n", encoding="utf-8"
        )

    def resurrect_receipt(root):
        (root / port_id / "nxdeployment.json").write_text(
            "{}\n", encoding="utf-8"
        )

    def strip_instance_lock(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        require("flock -n 9" in contents,
                "fixture launcher lacks the instance lock")
        path.write_text(
            contents.replace("flock -n 9", "true", 1), encoding="utf-8"
        )

    def remove_manifest(root):
        (root / port_id / "nxport.json").unlink()

    def strip_runtime_contract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        needle = "export NXCOMPAT_RUNTIME_REPORT="
        require(needle in contents,
                "fixture launcher lacks the runtime contract")
        path.write_text(
            contents.replace(needle, "export NXCOMPAT_REPORT=", 1),
            encoding="utf-8",
        )

    def duplicate_runtime_contract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        marker = 'if [ -n "$BIN_PRELOAD" ]; then'
        require(contents.count(marker) == 1,
                "fixture launcher lacks its launch boundary")
        path.write_text(
            contents.replace(
                marker,
                "NXCOMPAT_RUNTIME_REPORT=divergent\n" + marker,
                1,
            ),
            encoding="utf-8",
        )

    def diverge_required_files(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        assignment = "NXBOOTSTRAP_REQUIRED_FILES=%s" % \
            tool_module.BOOTSTRAP_GENERATOR.shell_join(
                nxport["required_files"]
            )
        require(contents.count(assignment) == 1,
                "fixture launcher lacks the required-files assignment")
        path.write_text(
            contents.replace(
                assignment, "NXBOOTSTRAP_REQUIRED_FILES=wrong-file", 1
            ),
            encoding="utf-8",
        )

    def strip_required_guard(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        guard = '[ ! -s "$required_path" ]'
        require(contents.count(guard) == 1,
                "fixture launcher lacks the non-empty required-file guard")
        path.write_text(
            contents.replace(guard, "false", 1), encoding="utf-8"
        )

    def move_required_gate_after_bin(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        block = tool_module.BOOTSTRAP_GENERATOR.render_required_files_block(
            nxport
        )
        bin_line = 'BIN="$GAMEDIR/%s"\n' % nxport["executable"]
        require(contents.count(block) == 1 and contents.count(bin_line) == 1,
                "fixture launcher lacks the ordered required-files gate")
        contents = contents.replace(block, "", 1)
        path.write_text(
            contents.replace(bin_line, bin_line + block + "\n", 1),
            encoding="utf-8",
        )

    def disable_required_nxextract(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        assignment = "NXEXTRACT_REQUESTED=1"
        require(nxport["nxextract"]["mode"] == "yes" and
                contents.count(assignment) == 1,
                "fixture launcher is not a mandatory NXExtract port")
        path.write_text(
            contents.replace(assignment, "NXEXTRACT_REQUESTED=0", 1),
            encoding="utf-8",
        )

    def strip_executable_symlink_guard(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        guard = '[ -L "$GAMEDIR/%s" ]' % nxport["executable"]
        require(contents.count(guard) == 1,
                "fixture launcher lacks the executable symlink guard")
        path.write_text(
            contents.replace(
                guard, '[ -L "$NXBOOTSTRAP_EXECUTABLE" ]', 1
            ),
            encoding="utf-8",
        )

    def hide_payload_gates_in_dead_code(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        nxextract_block = \
            tool_module.BOOTSTRAP_GENERATOR.render_nxextract_block(nxport)
        required_block = \
            tool_module.BOOTSTRAP_GENERATOR.render_required_files_block(nxport)
        require(contents.count(nxextract_block) == 1 and
                contents.count(required_block) == 1,
                "fixture launcher lacks its payload gates")
        contents = contents.replace(
            nxextract_block, "if false; then\n" + nxextract_block, 1
        )
        path.write_text(
            contents.replace(required_block, required_block + "\nfi", 1),
            encoding="utf-8",
        )

    def restore_inode_bound_lock(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        stable = 'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"'
        require(contents.count(stable) == 1,
                "fixture launcher lacks its stable lock open")
        path.write_text(
            contents.replace(
                stable, 'exec 9<"$NXBOOTSTRAP_EXECUTABLE"', 1
            ),
            encoding="utf-8",
        )

    def strip_forced_termination(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        forced = 'builtin kill -KILL "$game_pid"'
        require(contents.count(forced) == 1,
                "fixture launcher lacks bounded forced termination")
        path.write_text(
            contents.replace(forced, 'builtin kill -TERM "$game_pid"', 1),
            encoding="utf-8",
        )

    def strip_finish_guard(root):
        path = root / launcher_name
        contents = path.read_text(encoding="utf-8")
        guard = '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0'
        require(contents.count(guard) == 1,
                "fixture launcher lacks the pm_finish exactly-once guard")
        path.write_text(
            contents.replace(guard, "true", 1), encoding="utf-8"
        )

    expect_rejected("resurrected-library", resurrect_library)
    expect_rejected("resurrected-receipt", resurrect_receipt)
    expect_rejected("stripped-instance-lock", strip_instance_lock)
    expect_rejected("stripped-runtime-contract", strip_runtime_contract)
    expect_rejected("duplicate-runtime-contract", duplicate_runtime_contract)
    expect_rejected("divergent-required-files", diverge_required_files)
    expect_rejected("stripped-required-guard", strip_required_guard)
    expect_rejected("late-required-gate", move_required_gate_after_bin)
    expect_rejected("disabled-required-nxextract", disable_required_nxextract)
    expect_rejected("stripped-executable-preflight",
                    strip_executable_symlink_guard)
    expect_rejected("dead-payload-gates", hide_payload_gates_in_dead_code)
    expect_rejected("inode-bound-lock", restore_inode_bound_lock)
    expect_rejected("unbounded-termination", strip_forced_termination)
    expect_rejected("unguarded-finish", strip_finish_guard)
    expect_rejected("missing-manifest", remove_manifest)


def main():
    with tempfile.TemporaryDirectory(prefix="nxgenerator-m19-") as temporary:
        work = Path(temporary)
        for manifest, port_id, launcher, architecture in EXAMPLES:
            first = work / (port_id + "-first")
            second = work / (port_id + "-second")
            run_generator(manifest, first)
            run_generator(manifest, second)
            validate_generation(first, port_id, launcher, architecture)
            require(snapshot(first) == snapshot(second),
                    "two clean generations differ for %s" % architecture)
            if architecture == "aarch64":
                validate_deployment_tamper_rejection(
                    first, work, port_id, launcher
                )
            run_generator(manifest, first, expected=1)

        # The production deployment gate is mode-sensitive even though the
        # two public examples deliberately use mandatory extraction.
        for nxextract_mode in ("auto", "no"):
            mode_project = json.loads(
                EXAMPLES[0][0].read_text(encoding="utf-8")
            )
            mode_project["nxport"]["nxextract"]["mode"] = nxextract_mode
            if nxextract_mode == "no":
                mode_project["nxextract_recipe"] = None
            mode_manifest = work / ("mode-%s.json" % nxextract_mode)
            mode_manifest.write_text(
                json.dumps(mode_project, ensure_ascii=False), encoding="utf-8"
            )
            mode_output = work / ("mode-%s-output" % nxextract_mode)
            run_generator(mode_manifest, mode_output)
            validate_generation(
                mode_output, "nxexample-aarch64",
                "NXExample AArch64.sh", "aarch64"
            )

        victim = work / "victim"
        victim.mkdir()
        symlink_output = work / "output-link"
        symlink_output.symlink_to(victim, target_is_directory=True)
        run_generator(EXAMPLES[0][0], symlink_output, expected=1)
        require(list(victim.iterdir()) == [],
                "generator wrote through an output symlink")

        hostile = json.loads(EXAMPLES[0][0].read_text(encoding="utf-8"))
        hostile["nxport"]["title"] = "Private 192.0.2.10"
        hostile_path = work / "hostile.json"
        hostile_path.write_text(
            json.dumps(hostile, ensure_ascii=False), encoding="utf-8"
        )
        run_generator(hostile_path, work / "hostile-output", expected=1)

        missing_recipe = json.loads(
            EXAMPLES[0][0].read_text(encoding="utf-8")
        )
        missing_recipe["nxextract_recipe"] = None
        missing_path = work / "missing-recipe.json"
        missing_path.write_text(
            json.dumps(missing_recipe), encoding="utf-8"
        )
        run_generator(missing_path, work / "missing-output", expected=1)

    print("M19 nxgenerator tests passed: abis=2 deterministic=1 "
          "nxbootstrap=%s deployment=1 tamper_rejections=15 "
          "nxextract_modes=3 metadata=1 adapter_unimplemented=1 "
          "support_claims=0"
          % EXPECTED_BOOTSTRAP_VERSION)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError) as error:
        print("M19 nxgenerator tests failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
