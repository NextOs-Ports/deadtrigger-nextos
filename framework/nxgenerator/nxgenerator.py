#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate a deterministic, fail-closed PortMaster project scaffold."""

import argparse
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile


ROOT = Path(__file__).resolve().parent
REPOSITORY = ROOT.parents[1]
BOOTSTRAP_ROOT = REPOSITORY / "framework" / "nxbootstrap"
NXEXTRACT_ROOT = REPOSITORY / "suportando_outros_devices" / "extrator-universal"
BOOTSTRAP_GENERATOR_PATH = BOOTSTRAP_ROOT / "tools" / "generate-port.py"
BOOTSTRAP_LAUNCHER_TEMPLATE = BOOTSTRAP_ROOT / "templates" / "launcher.sh.in"
NXEXTRACT_FILES = (
    "nxextract.py",
    "run-extractor.sh",
    "nxextract-runtime-env.sh",
)
PROJECT_KEYS = {
    "schema_version",
    "nxport",
    "nxextract_recipe",
    "adapter",
    "portmaster",
    "license",
    "documentation",
}
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])"
)
PERSONAL_PATH_RE = re.compile(
    r"(?:^|[ /])(?:/home/[^/ ]+|/Users/[^/ ]+|[A-Za-z]:[\\/])",
    re.IGNORECASE,
)
SPDX_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.+-]{0,63}$")
VERSION_RE = re.compile(r"^[0-9]+(?:[.][0-9]+)+$")
AT_FDCWD = -100
RENAME_NOREPLACE = 1


class ProjectError(Exception):
    """A project manifest or publication boundary is invalid."""


def load_bootstrap_generator():
    spec = importlib.util.spec_from_file_location(
        "nxbootstrap_generate_port", BOOTSTRAP_GENERATOR_PATH
    )
    if spec is None or spec.loader is None:
        raise ProjectError("cannot load the pinned nxbootstrap generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BOOTSTRAP_GENERATOR = load_bootstrap_generator()


def sha256_bytes(payload):
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value):
    return (json.dumps(
        value, ensure_ascii=False, indent=2, sort_keys=True
    ) + "\n").encode("utf-8")


def require_object(value, context, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ProjectError(
            "%s must contain exactly: %s" %
            (context, ", ".join(sorted(keys)))
        )
    return value


def require_string(value, context, pattern=None):
    if not isinstance(value, str) or not value or len(value) > 512:
        raise ProjectError("%s must be a bounded non-empty string" % context)
    if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
           for character in value):
        raise ProjectError("%s contains a control character" % context)
    if pattern is not None and not pattern.fullmatch(value):
        raise ProjectError("%s has an invalid value" % context)
    return value


def reject_public_literal(value, context):
    if IPV4_RE.search(value) or PERSONAL_PATH_RE.search(value):
        raise ProjectError("%s contains a private host literal" % context)


def safe_repository_file(value, context):
    value = require_string(value, context)
    logical = PurePosixPath(value)
    if (logical.is_absolute() or not logical.parts or
            any(part in ("", ".", "..") for part in logical.parts)):
        raise ProjectError("%s must be a repository-relative path" % context)
    cursor = REPOSITORY
    for part in logical.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ProjectError("%s traverses a symlink" % context)
    if not cursor.is_file():
        raise ProjectError("%s is not a regular file" % context)
    return cursor


def read_version(path, component):
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise ProjectError("cannot read %s version: %s" % (component, error))
    if not VERSION_RE.fullmatch(value):
        raise ProjectError("%s has an invalid version" % component)
    return value


def require_regular_file(path, context):
    if path.is_symlink() or not path.is_file():
        raise ProjectError("%s must be a regular non-symlink file" % context)
    return path


def bootstrap_source_state():
    # nxbootstrap 0.6.0: the product is one self-contained launcher plus
    # nxport.json. There is no runtime library and no deployment receipt;
    # the pins cover the generator and its template instead.
    version = read_version(BOOTSTRAP_ROOT / "VERSION", "nxbootstrap")
    if getattr(BOOTSTRAP_GENERATOR, "NXBOOTSTRAP_VERSION", None) != version:
        raise ProjectError(
            "nxbootstrap generator version does not match VERSION"
        )

    generator_path = require_regular_file(
        BOOTSTRAP_GENERATOR_PATH, "nxbootstrap generator"
    )
    launcher_template = require_regular_file(
        BOOTSTRAP_LAUNCHER_TEMPLATE, "nxbootstrap launcher template"
    )
    template_bytes = launcher_template.read_bytes()
    if b"@NXBOOTSTRAP_VERSION@" not in template_bytes:
        raise ProjectError(
            "nxbootstrap launcher template lacks its version slot"
        )
    return {
        "version": version,
        "source_files": {
            "templates/launcher.sh.in": sha256_bytes(template_bytes),
            "tools/generate-port.py": sha256_file(generator_path),
        },
    }


def version_tuple(value):
    return tuple(int(part) for part in value.split("."))


def validate_project(document):
    require_object(document, "project manifest", PROJECT_KEYS)
    if document["schema_version"] != 1:
        raise ProjectError("project schema_version must be 1")

    try:
        nxport = BOOTSTRAP_GENERATOR.validate(document["nxport"])
    except BOOTSTRAP_GENERATOR.ManifestError as error:
        raise ProjectError("invalid nxport: %s" % error)
    if nxport["architecture"] not in ("armv7", "aarch64"):
        raise ProjectError("public project scaffold supports ARMv7 or AArch64")
    reject_public_literal(nxport["title"], "nxport.title")
    if any(character in nxport["title"] for character in "[]<>`@"):
        raise ProjectError("nxport.title contains unsafe documentation markup")

    adapter = require_object(
        document["adapter"], "adapter", {"skeleton"}
    )
    if adapter["skeleton"] != "contract-only":
        raise ProjectError("adapter.skeleton must be contract-only")

    portmaster = require_object(
        document["portmaster"], "portmaster",
        {"metadata_version", "min_glibc"},
    )
    if (isinstance(portmaster["metadata_version"], bool) or
            not isinstance(portmaster["metadata_version"], int) or
            portmaster["metadata_version"] != 4):
        raise ProjectError("portmaster.metadata_version must be 4")
    min_glibc = require_string(
        portmaster["min_glibc"], "portmaster.min_glibc", VERSION_RE
    )
    if version_tuple(min_glibc) > (2, 30):
        raise ProjectError("portmaster.min_glibc exceeds the public ceiling")

    license_config = require_object(
        document["license"], "license", {"spdx_id", "source"}
    )
    spdx_id = require_string(
        license_config["spdx_id"], "license.spdx_id", SPDX_RE
    )
    license_source = safe_repository_file(
        license_config["source"], "license.source"
    )

    documentation = require_object(
        document["documentation"], "documentation",
        {"status", "proven_support"},
    )
    if documentation["status"] != "scaffold":
        raise ProjectError("generated documentation must begin as a scaffold")
    if documentation["proven_support"] != []:
        raise ProjectError(
            "the generator cannot invent physical support declarations"
        )

    recipe_value = document["nxextract_recipe"]
    if nxport["nxextract"]["mode"] == "no":
        if recipe_value is not None:
            raise ProjectError(
                "nxextract_recipe must be null when NXExtract is disabled"
            )
        recipe_source = None
    else:
        recipe_source = safe_repository_file(
            recipe_value, "nxextract_recipe"
        )
        try:
            recipe = json.loads(recipe_source.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, ValueError) as error:
            raise ProjectError("invalid NXExtract recipe: %s" % error)
        if not isinstance(recipe, dict):
            raise ProjectError("NXExtract recipe root must be an object")

    return {
        "nxport": nxport,
        "project": document,
        "recipe_source": recipe_source,
        "license_source": license_source,
        "license_spdx": spdx_id,
        "min_glibc": min_glibc,
        "metadata_version": portmaster["metadata_version"],
    }


def atomic_write_bytes(path, payload, mode):
    descriptor, temporary = tempfile.mkstemp(
        prefix=".%s." % path.name, dir=str(path.parent)
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def render_template(name, replacements):
    text = (ROOT / "templates" / name).read_text(encoding="utf-8")
    for key, value in replacements.items():
        text = text.replace("@%s@" % key, value)
    leftovers = sorted(set(re.findall(r"@[A-Z0-9_]+@", text)))
    if leftovers:
        raise ProjectError(
            "unresolved documentation token(s): %s" % ", ".join(leftovers)
        )
    return text.encode("utf-8")


def artifact_inventory(stage):
    records = []
    for path in sorted(stage.rglob("*")):
        if path.is_symlink():
            raise ProjectError("generated tree contains a symlink")
        if not path.is_file():
            continue
        relative = path.relative_to(stage).as_posix()
        if relative.endswith("/GENERATION.json"):
            continue
        mode = stat.S_IMODE(path.stat().st_mode)
        records.append({
            "path": relative,
            "mode": "%04o" % mode,
            "sha256": sha256_file(path),
        })
    return records


def validate_bootstrap_deployment(stage, nxport, source_state):
    # 0.6.0 output contract: exactly one visible launcher (0755) plus the
    # port's nxport.json (0644). No library, no receipt, no run.sh.
    port_dir = stage / nxport["id"]
    launcher_path = require_regular_file(
        stage / nxport["launcher_name"], "generated launcher"
    )
    manifest_path = require_regular_file(
        port_dir / "nxport.json", "generated nxport manifest"
    )
    expected_modes = {launcher_path: 0o755, manifest_path: 0o644}
    for path, expected_mode in expected_modes.items():
        if stat.S_IMODE(path.stat().st_mode) != expected_mode:
            raise ProjectError(
                "generated deployment has an unsafe mode: %s" % path.name
            )

    for retired in ("nxbootstrap.sh",
                    "nxbootstrap-%s.sh" % source_state["version"],
                    "nxdeployment.json",
                    "run.sh"):
        if (port_dir / retired).exists() or (stage / retired).exists():
            raise ProjectError(
                "generated deployment contains retired artifact: %s" % retired
            )

    try:
        launcher = launcher_path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise ProjectError(
            "generated launcher is not UTF-8: %s" % error
        )
    if "nxbootstrap %s" % source_state["version"] not in launcher:
        raise ProjectError(
            "generated launcher does not record its generator version"
        )
    if re.search(r"@[A-Z0-9_]+@", launcher):
        raise ProjectError("generated launcher has unresolved tokens")
    if launcher != BOOTSTRAP_GENERATOR.render_launcher(nxport):
        raise ProjectError(
            "generated launcher differs from the canonical nxbootstrap render"
        )
    expected_runtime_exports = (
        ("NXCOMPAT_PORT_ID",
         BOOTSTRAP_GENERATOR.shell_join([nxport["id"]])),
        ("NXCOMPAT_GAME_DIR", '"$GAMEDIR"'),
        ("NXCOMPAT_REQUIRED_CAPABILITIES",
         BOOTSTRAP_GENERATOR.shell_join(nxport["required_capabilities"])),
        ("NXCOMPAT_ENABLED_QUIRKS",
         BOOTSTRAP_GENERATOR.shell_join(nxport["enabled_quirks"])),
        ("NXCOMPAT_RUNTIME_REPORT",
         BOOTSTRAP_GENERATOR.shell_join([nxport["runtime_report"]])),
    )
    for name, value in expected_runtime_exports:
        assignment = "export {}={}".format(name, value)
        if (launcher.count(name + "=") != 1 or
                launcher.count(assignment) != 1):
            raise ProjectError(
                "generated launcher runtime export differs: %s" % name
            )

    expected_nxextract = BOOTSTRAP_GENERATOR.render_nxextract_block(nxport)
    expected_required = BOOTSTRAP_GENERATOR.render_required_files_block(nxport)
    if launcher.count(expected_nxextract) != 1:
        raise ProjectError(
            "generated launcher NXExtract contract differs from nxport"
        )
    if launcher.count(expected_required) != 1:
        raise ProjectError(
            "generated launcher required-files gate differs from nxport"
        )

    required_assignment = "NXBOOTSTRAP_REQUIRED_FILES={}".format(
        BOOTSTRAP_GENERATOR.shell_join(nxport["required_files"])
    )
    required_assignments = re.findall(
        r"(?m)^NXBOOTSTRAP_REQUIRED_FILES=", launcher
    )
    if (len(required_assignments) != 1 or
            launcher.count(required_assignment) != 1):
        raise ProjectError(
            "generated launcher required-files assignment differs from nxport"
        )

    requested_assignments = re.findall(
        r"(?m)^[ \t]*NXEXTRACT_REQUESTED=([01])$", launcher
    )
    nxextract_mode = nxport["nxextract"]["mode"]
    expected_requested = {
        "no": [],
        "yes": ["1"],
        "auto": ["0", "1"],
    }[nxextract_mode]
    if requested_assignments != expected_requested:
        raise ProjectError(
            "generated launcher NXEXTRACT_REQUESTED differs for mode %s" %
            nxextract_mode
        )
    runner_call = 'bash "$NXDIR/run-extractor.sh"'
    if ((nxextract_mode == "no" and runner_call in launcher) or
            (nxextract_mode != "no" and launcher.count(runner_call) != 1)):
        raise ProjectError(
            "generated launcher NXExtract invocation differs for mode %s" %
            nxextract_mode
        )

    bin_assignment = 'BIN="$GAMEDIR/{}"'.format(nxport["executable"])
    if launcher.count(bin_assignment) != 1:
        raise ProjectError("generated launcher executable assignment differs")
    nxextract_index = launcher.index(expected_nxextract)
    required_index = launcher.index(expected_required)
    bin_index = launcher.index(bin_assignment)
    if not nxextract_index < required_index < bin_index:
        raise ProjectError(
            "generated launcher setup gates must finish before BIN"
        )

    executable_path = '"$GAMEDIR/{}"'.format(nxport["executable"])
    executable_preflight = (
        "NXBOOTSTRAP_EXECUTABLE=$(readlink -f {} 2>/dev/null) || "
        "NXBOOTSTRAP_EXECUTABLE=\"\"".format(executable_path),
        'case "$NXBOOTSTRAP_EXECUTABLE" in',
        '"$GAMEDIR"/*) ;;',
        '*) NXBOOTSTRAP_EXECUTABLE="" ;;',
        ('if [ -z "$NXBOOTSTRAP_EXECUTABLE" ] || '
         '[ ! -f "$NXBOOTSTRAP_EXECUTABLE" ] || \\'),
        ('[ ! -s "$NXBOOTSTRAP_EXECUTABLE" ] || [ -L {} ]; then'.format(
            executable_path
        )),
        '$ESUDO chmod +x "$NXBOOTSTRAP_EXECUTABLE"',
    )
    preflight = launcher[:nxextract_index]
    cursor = -1
    for marker in executable_preflight:
        marker_index = preflight.find(marker, cursor + 1)
        if marker_index < 0:
            raise ProjectError(
                "generated launcher lacks safe executable preflight: %s" %
                marker
            )
        cursor = marker_index

    stable_lock = (
        'NXBOOTSTRAP_LOCK_DIR="$NXBOOTSTRAP_LOCK_ROOT/.nxbootstrap-',
        ('NXBOOTSTRAP_LOCK_FILE="$NXBOOTSTRAP_LOCK_DIR/nxport-'
         '{}.flock"'.format(nxport["id"])),
        'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
        "stat -L -c '%h' -- /proc/self/fd/9",
        "stat -L -c '%d:%i' -- \"$NXBOOTSTRAP_LOCK_FILE\"",
        "stat -L -c '%d:%i' -- /proc/self/fd/9",
        '[ "$NXBOOTSTRAP_LOCK_PATH_ID" != "$NXBOOTSTRAP_LOCK_FD_ID" ]',
        "flock -n 9",
    )
    for marker in stable_lock:
        marker_index = preflight.find(marker, cursor + 1)
        if marker_index < 0:
            raise ProjectError(
                "generated launcher lacks stable instance lock: %s" % marker
            )
        cursor = marker_index

    required_guard_markers = (
        "while IFS= read -r required_file; do",
        ('required_path=$(readlink -f "$GAMEDIR/$required_file" '
         '2>/dev/null) || required_path=""'),
        'case "$required_path" in',
        '"$GAMEDIR"/*) ;;',
        ('if [ -z "$required_path" ] || [ ! -f "$required_path" ] || \\\n'
         '     [ ! -s "$required_path" ] || '
         '[ -L "$GAMEDIR/$required_file" ]; then'),
        'done <<< "$NXBOOTSTRAP_REQUIRED_FILES"',
        "unset NXBOOTSTRAP_REQUIRED_FILES required_file required_path",
    )
    required_cursor = -1
    for marker in required_guard_markers:
        marker_index = expected_required.find(marker, required_cursor + 1)
        if marker_index < 0:
            raise ProjectError(
                "generated launcher lacks required-files guard: %s" % marker
            )
        required_cursor = marker_index
    for guarantee in ("flock -n 9",
                      'wait "$game_pid"',
                      "trap '' INT TERM HUP",
                      "nxbootstrap_finish",
                      '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0',
                      "nxbootstrap_abort_before_game 129",
                      "nxbootstrap_abort_before_game 130",
                      "nxbootstrap_abort_before_game 143",
                      "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
                      "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                      'builtin kill -KILL "$game_pid"'):
        if guarantee not in launcher:
            raise ProjectError(
                "generated launcher lacks golden-port guarantee: %s" %
                guarantee
            )

    return {
        "launcher_sha256": sha256_file(launcher_path),
        "nxport_sha256": sha256_file(manifest_path),
    }


def rename_noreplace(source, destination):
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError as error:
        raise ProjectError(
            "host lacks renameat2; no-overwrite publication is unavailable"
        ) from error
    renameat2.argtypes = (
        ctypes.c_int, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    result = renameat2(
        AT_FDCWD, os.fsencode(source),
        AT_FDCWD, os.fsencode(destination),
        RENAME_NOREPLACE,
    )
    if result == 0:
        return
    selected_errno = ctypes.get_errno()
    if selected_errno in (errno.EEXIST, errno.ENOTEMPTY):
        raise ProjectError("refusing to overwrite output: %s" % destination)
    raise ProjectError(
        "cannot publish output without replacement: %s" %
        os.strerror(selected_errno)
    )


def generate_project(document, output):
    config = validate_project(document)
    output = Path(os.path.abspath(os.fspath(output)))
    parent = output.parent
    if output.name in ("", ".", ".."):
        raise ProjectError("invalid output name")
    if parent.is_symlink() or not parent.is_dir():
        raise ProjectError("output parent must be a real existing directory")
    if output.exists() or output.is_symlink():
        raise ProjectError("refusing to overwrite output: %s" % output)

    stage = Path(tempfile.mkdtemp(prefix=".nxgenerator.", dir=str(parent)))
    published = False
    try:
        bootstrap_source = bootstrap_source_state()
        nxport_input = stage / ".nxport-input.json"
        atomic_write_bytes(
            nxport_input, canonical_json(document["nxport"]), 0o600
        )
        try:
            BOOTSTRAP_GENERATOR.generate(nxport_input, stage, False)
        except (OSError, ValueError, BOOTSTRAP_GENERATOR.ManifestError) as error:
            raise ProjectError("nxbootstrap generation failed: %s" % error)
        nxport_input.unlink()
        validate_bootstrap_deployment(
            stage, config["nxport"], bootstrap_source
        )

        port_id = config["nxport"]["id"]
        port_dir = stage / port_id
        adapter_dir = port_dir / "adapter"
        adapter_dir.mkdir(mode=0o755)

        if config["recipe_source"] is not None:
            nxextract_dir = port_dir / "nxextract"
            nxextract_dir.mkdir(mode=0o755)
            for name in NXEXTRACT_FILES:
                source = NXEXTRACT_ROOT / name
                if source.is_symlink() or not source.is_file():
                    raise ProjectError("canonical NXExtract file is unsafe: %s" % name)
                atomic_write_bytes(
                    nxextract_dir / name, source.read_bytes(), 0o644
                )
            atomic_write_bytes(
                port_dir / "extractor.json",
                config["recipe_source"].read_bytes(),
                0o644,
            )

        adapter_contract = {
            "schema": "nxadapter-skeleton-v1",
            "schema_version": 1,
            "status": "unimplemented_nonrelease",
            "release_ready": False,
            "lifecycle": {"sequence": [], "source_evidence": []},
            "jni": {"classes": [], "methods": [], "callbacks": []},
            "imports": [],
            "audio": {"format": None, "callbacks": []},
            "input": {"mapping": None, "touch": None},
            "persistence": {"paths": [], "callbacks": []},
            "terminal": {"action": None, "evidence": []},
            "quirks": [],
            "prohibitions": [
                "do not invent lifecycle order",
                "do not promote offsets or callbacks to defaults",
                "do not invent save or terminal behavior",
            ],
        }
        atomic_write_bytes(
            adapter_dir / "adapter-contract.json",
            canonical_json(adapter_contract), 0o644,
        )

        architecture = config["nxport"]["architecture"]
        portmaster_arch = "armhf" if architecture == "armv7" else architecture
        metadata = {
            "version": config["metadata_version"],
            "name": port_id + ".zip",
            "items": [config["nxport"]["launcher_name"], port_id + "/"],
            "items_opt": [],
            "attr": {
                "title": config["nxport"]["title"],
                "arch": [portmaster_arch],
                "min_glibc": config["min_glibc"],
            },
        }
        atomic_write_bytes(
            port_dir / "port.json", canonical_json(metadata), 0o644
        )

        project_canonical = canonical_json(document)
        atomic_write_bytes(
            port_dir / "nxproject.json", project_canonical, 0o644
        )
        atomic_write_bytes(
            port_dir / "LICENSE", config["license_source"].read_bytes(), 0o644
        )

        readme = render_template("README.md.in", {
            "TITLE": config["nxport"]["title"],
            "PORT_ID": port_id,
            "ARCHITECTURE": architecture,
            "PORTMASTER_ARCH": portmaster_arch,
            "LICENSE_SPDX": config["license_spdx"],
        })
        atomic_write_bytes(port_dir / "README.md", readme, 0o644)

        nxgenerator_version = read_version(ROOT / "VERSION", "nxgenerator")
        nxextract_version = read_version(
            NXEXTRACT_ROOT / "VERSION", "NXExtract"
        )
        source_pins = {
            "nxbootstrap": {
                "version": bootstrap_source["version"],
                "source_files": bootstrap_source["source_files"],
            },
            "nxextract": None,
        }
        if config["recipe_source"] is not None:
            source_pins["nxextract"] = {
                "version": nxextract_version,
                "files": {
                    name: sha256_file(NXEXTRACT_ROOT / name)
                    for name in NXEXTRACT_FILES
                },
                "recipe_sha256": sha256_file(config["recipe_source"]),
            }
        generation = {
            "schema": "nxgenerator-receipt-v1",
            "schema_version": 1,
            "generator": {
                "name": "nxgenerator",
                "version": nxgenerator_version,
            },
            "project_manifest_sha256": sha256_bytes(project_canonical),
            "source_pins": source_pins,
            "artifacts": artifact_inventory(stage),
            "claims": {
                "deterministic_scaffold": True,
                "release_ready": False,
                "physical_support_proven": False,
                "adapter_lifecycle_implemented": False,
            },
        }
        atomic_write_bytes(
            port_dir / "GENERATION.json", canonical_json(generation), 0o644
        )

        rename_noreplace(stage, output)
        published = True
        return output, config
    finally:
        if not published and stage.exists():
            shutil.rmtree(stage)


def load_project(path):
    if path.is_symlink() or not path.is_file():
        raise ProjectError("project manifest must be a regular file")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise ProjectError("cannot read project manifest: %s" % error)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate a deterministic PortMaster project scaffold"
    )
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args(argv)
    try:
        output, config = generate_project(
            load_project(options.manifest), options.output
        )
    except ProjectError as error:
        print("nxgenerator: %s" % error, file=sys.stderr)
        return 1
    print("generated project %s (%s) in %s" % (
        config["nxport"]["id"],
        config["nxport"]["architecture"],
        output,
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
