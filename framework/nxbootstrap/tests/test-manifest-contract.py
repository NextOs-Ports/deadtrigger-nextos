#!/usr/bin/env python3
"""Process-free regression gate for nxport schema v2 and module locks."""

import ast
import copy
import importlib.util
import json
import re
import sys
from pathlib import Path


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP_ROOT = REPO_ROOT / "framework" / "nxbootstrap"
GENERATOR_PATH = BOOTSTRAP_ROOT / "tools" / "generate-port.py"
NXBOOTSTRAP_PATH = BOOTSTRAP_ROOT / "nxbootstrap.sh"
CONTRACT_PATH = REPO_ROOT / "framework" / "contracts" / "declarative-v1.json"
SCHEMA_V2_PATH = BOOTSTRAP_ROOT / "schema" / "nxport-v2.schema.json"
SCHEMA_V1_PATH = BOOTSTRAP_ROOT / "schema" / "nxport-v1-legacy.schema.json"
M06_AUDIT_PATH = BOOTSTRAP_ROOT / "m06-audit-v1.json"
CAPABILITY_REGISTRY_PATH = (
    REPO_ROOT / "framework" / "nxcompat" / "capabilities-v1.json"
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def load_generator():
    spec = importlib.util.spec_from_file_location("nxport_generator_contract",
                                                  GENERATOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expect_manifest_error(generator, manifest, label):
    try:
        generator.validate(manifest)
    except generator.ManifestError:
        return
    raise AssertionError("invalid manifest was accepted: %s" % label)


def parse_define(path, name, seen=None):
    text = path.read_text(encoding="utf-8")
    match = re.search(
        r"^#define\s+%s\s+([A-Za-z_][A-Za-z0-9_]*|[0-9]+)u?\s*$" %
        re.escape(name), text, re.MULTILINE)
    require(match is not None, "missing API define %s in %s" % (name, path))
    value = match.group(1)
    if value.isdigit():
        return int(value)
    seen = set() if seen is None else set(seen)
    require(name not in seen, "cyclic API define %s in %s" % (name, path))
    seen.add(name)
    return parse_define(path, value, seen)


def check_component_contract(contract):
    require(contract.get("schema_version") == 1,
            "declarative contract schema changed")
    require(contract.get("contract_version") == "1.0.9",
            "declarative contract version changed unexpectedly")
    require([layer["id"] for layer in contract.get("layers", [])] ==
            ["pre-main", "runtime-common", "adapter"],
            "three-layer boundary is incomplete or reordered")

    semver = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
    components = contract.get("components", [])
    ids = [component.get("id") for component in components]
    require(len(ids) == len(set(ids)), "component ids are duplicated")
    require(set(ids) == {
        "nxbootstrap", "nxcompat", "nxgl", "nxinput", "nxloader",
        "nxandroid", "nxrelease", "nxextract",
    }, "component lock is incomplete")
    for component in components:
        current = component.get("current_version")
        require(isinstance(current, str) and semver.fullmatch(current),
                "component version is not semantic: %s" % component.get("id"))
        version_file = REPO_ROOT / component["version_file"]
        require(version_file.is_file(),
                "component version file is missing: %s" % version_file)
        require(version_file.read_text(encoding="utf-8").strip() == current,
                "component lock differs from VERSION: %s" % component["id"])

    api_checks = {
        "nxcompat": parse_define(
            REPO_ROOT / "framework/nxcompat/include/nxcompat.h",
            "NXCOMPAT_API_VERSION"),
        "nxgl": parse_define(
            REPO_ROOT / "framework/nxgl/include/nxgl.h",
            "NXGL_API_CURRENT_VERSION"),
        "nxinput": parse_define(
            REPO_ROOT / "framework/nxinput/include/nxinput.h",
            "NXINPUT_API_VERSION"),
        "nxloader": parse_define(
            REPO_ROOT / "framework/nxloader/include/nxloader.h",
            "NXLOADER_API_VERSION_MAJOR"),
        "nxandroid": parse_define(
            REPO_ROOT / "framework/nxandroid/include/nxandroid.h",
            "NXANDROID_API_VERSION"),
    }
    by_id = {component["id"]: component for component in components}
    for component_id, api_version in api_checks.items():
        require(by_id[component_id]["api_version"] == api_version,
                "API lock differs from public header: %s" % component_id)

    nxrelease_source = (
        REPO_ROOT / "framework/nxrelease/nxrelease.py"
    ).read_text(encoding="utf-8")
    release_schema = re.search(r"^SCHEMA_VERSION\s*=\s*([0-9]+)$",
                               nxrelease_source, re.MULTILINE)
    require(release_schema and
            by_id["nxrelease"]["api_version"] == int(release_schema.group(1)),
            "nxrelease schema lock differs from implementation")

    nxport = contract.get("nxport", {})
    require(nxport.get("capability_registry") == {
        "path": "framework/nxcompat/capabilities-v1.json",
        "schema_version": 1,
        "registry_version": "1.0.0",
        "count": 31,
        "default_required": False,
    }, "nxport capability registry lock is incomplete")
    require(nxport.get("quirk_registry") == {
        "path": "framework/nxcompat/quirk-registry-v1.json",
        "schema_version": 1,
        "registry_version": "1.0.0",
        "count": 20,
        "default_enabled": False,
    }, "nxport quirk registry lock is incomplete")
    quirk_registry = json.loads(
        (REPO_ROOT / "framework/nxcompat/quirk-registry-v1.json").read_text(
            encoding="utf-8"))
    require(len(quirk_registry["quirks"]) == 20 and
            quirk_registry["default_enabled"] is False,
            "quirk registry drifted from its contract lock")
    for entry in quirk_registry["quirks"]:
        require(isinstance(entry.get("condition"), str) and
                "device" not in entry["id"].split(".")[1][:6] and
                isinstance(entry.get("effect"), str) and
                isinstance(entry.get("evidence"), list) and entry["evidence"],
                "quirk registry entry is not observation-conditioned: %s" %
                entry.get("id"))
    require(nxport.get("current_schema") == 2 and
            nxport.get("legacy_input_schemas") == [1] and
            nxport.get("generator_output_schema") == 2 and
            nxport.get("public_release_schema") == 2,
            "nxport v1->v2 compatibility boundary changed")
    require(nxport.get("process_names_allowed") is False,
            "process names returned to the manifest")
    require(nxport.get("quirk_default") == [],
            "a quirk became enabled by default")
    require(nxport.get("home_policy", {}).get("default") == "preserve",
            "HOME no longer defaults to preserve")
    require(nxport.get("nxextract_version") == "1.2.6",
            "declarative contract lost the exact NXExtract pin")
    runtime_codes = contract.get("exit_codes", {}).get("runtime", {})
    require(set(("0", "1", "2", "129", "130", "143", "child")) <=
            set(runtime_codes), "runtime exit-code contract is incomplete")


def check_schema(generator, schema_v2, schema_v1):
    require(schema_v2.get("$id") == "urn:nextos:nxport:schema:2",
            "current schema id changed")
    require(schema_v2.get("additionalProperties") is False,
            "v2 schema accepts unknown top-level fields")
    require(set(schema_v2["properties"]) == generator.KNOWN_KEYS_V2,
            "v2 JSON Schema and generator fields disagree")
    require(schema_v2["properties"]["schema_version"].get("const") == 2,
            "v2 JSON Schema does not pin schema_version")
    require(schema_v2["properties"]["nxextract"]["properties"]["version"].get(
        "const") == "1.2.6", "v2 JSON Schema does not pin NXExtract 1.2.6")
    for definition in ("relative_path",):
        re.compile(schema_v2["$defs"][definition]["pattern"])
    for property_name in ("id", "title", "launcher_name",
                          "required_capabilities", "enabled_quirks"):
        definition = schema_v2["properties"][property_name]
        pattern = definition.get("pattern") or definition.get("items", {}).get(
            "pattern")
        require(pattern is not None, "schema pattern missing for %s" %
                property_name)
        re.compile(pattern)
    require(schema_v1.get("additionalProperties") is False,
            "legacy schema accepts unknown fields")
    require(set(schema_v1["properties"]) == generator.KNOWN_KEYS_V1,
            "legacy JSON Schema and generator fields disagree")
    require(schema_v1["properties"]["schema_version"].get("const") == 1,
            "legacy JSON Schema does not pin schema_version")

    registry = load_json(CAPABILITY_REGISTRY_PATH)
    entries = registry.get("capabilities", [])
    identifiers = [entry.get("id") for entry in entries
                   if isinstance(entry, dict)]
    require(set(registry) == {
        "schema_version", "registry_version", "default_required", "states",
        "phases", "sources", "roles", "capabilities",
    }, "capability registry top-level schema changed")
    require(registry.get("schema_version") == 1 and
            registry.get("registry_version") == "1.0.0" and
            registry.get("default_required") is False,
            "capability registry header is invalid")
    require(registry.get("states") == [
        "absent", "observed", "opened", "active", "lost",
    ] and registry.get("phases") == [
        "preflight", "graphics", "audio", "input", "ready",
    ] and registry.get("sources") == [
        "probe", "nxgl", "sdl2-audio", "nxinput", "engine-adapter",
    ] and registry.get("roles") == [
        "observation", "baseline-graphics", "port-declared",
        "optional-enhancement", "optional-runtime",
    ], "capability registry ordered vocabularies changed")
    require(len(identifiers) == 31 == len(set(identifiers)),
            "capability registry must contain 31 unique identifiers")
    require(all(set(entry) == {"id", "phase", "source",
                               "minimum_evidence", "role"}
                for entry in entries),
            "capability registry entry schema changed")
    require(all(entry["phase"] in registry.get("phases", []) and
                entry["source"] in registry.get("sources", []) and
                entry["minimum_evidence"] in registry.get("states", []) and
                entry["minimum_evidence"] not in ("absent", "lost") and
                entry["role"] in registry.get("roles", [])
                for entry in entries),
            "capability registry contains an invalid contract value")
    require(set(identifiers) == set(generator.CAPABILITY_IDS),
            "generator capability allowlist differs from registry")
    require({prefix: sum(identifier.startswith(prefix + ".")
                         for identifier in identifiers)
             for prefix in ("host", "graphics", "audio", "input")} == {
                 "host": 13, "graphics": 10, "audio": 4, "input": 4,
             }, "capability registry namespace counts changed")
    schema_ids = schema_v2["properties"]["required_capabilities"]["items"].get(
        "enum", [])
    require(schema_ids == identifiers,
            "nxport schema capability enum differs from registry order")
    bootstrap_text = NXBOOTSTRAP_PATH.read_text(encoding="utf-8")
    shell_match = re.search(
        r"nxbootstrap_capability_known\(\) \{(.*?)\n\}", bootstrap_text,
        re.DOTALL)
    require(shell_match is not None,
            "nxbootstrap finite capability allowlist is missing")
    shell_ids = re.findall(
        r"(?:host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}",
        shell_match.group(1))
    require(shell_ids == identifiers,
            "nxbootstrap shell capability allowlist differs from registry")


def check_examples(generator):
    aarch64 = load_json(BOOTSTRAP_ROOT / "examples" / "nxport.example.json")
    armv7 = load_json(
        BOOTSTRAP_ROOT / "examples" / "nxport-armv7.example.json")
    normalized_aarch64 = generator.validate(aarch64)
    normalized_armv7 = generator.validate(armv7)
    require(normalized_aarch64["schema_version"] == 2 and
            normalized_aarch64["architecture"] == "aarch64",
            "AArch64 example is not canonical v2")
    require(normalized_armv7["schema_version"] == 2 and
            normalized_armv7["architecture"] == "armv7",
            "ARMv7 example is not canonical v2")
    for example in (normalized_aarch64, normalized_armv7):
        require(example["home_mode"] == "preserve",
                "safe examples change HOME by default")
        require(example["nxextract"] == {
            "mode": "auto", "version": "1.2.6"},
            "safe example does not pin NXExtract")
        require(example["enabled_quirks"] == [],
                "safe example enables a quirk")
        require(example["runtime_report"] == "log-and-logo",
                "safe example does not request logo/log reporting")
        text = json.dumps(example, ensure_ascii=False).lower()
        for forbidden in ("/home/", "\\users\\", "process_names",
                          "sdl_videodriver", "sdl_audiodriver", "device."):
            require(forbidden not in text,
                    "safe example contains forbidden selector/path: %s" %
                    forbidden)

    auto_block = generator.render_nxextract_block(normalized_aarch64)
    require("NXEXTRACT_REQUESTED=0" in auto_block and
            "NXEXTRACT_REQUESTED=1" in auto_block and
            '"$GAMEDIR/nxextract/nxextract.py"' in auto_block and
            '"$GAMEDIR/nxextract-runtime-env.sh"' in auto_block,
            "mode=auto does not fail closed on partial NXExtract state")
    yes_config = copy.deepcopy(normalized_aarch64)
    yes_config["nxextract"]["mode"] = "yes"
    yes_block = generator.render_nxextract_block(yes_config)
    require("NXEXTRACT_REQUESTED=1" in yes_block and
            "NXEXTRACT_REQUESTED=0" not in yes_block and
            "incomplete NXExtract integration" in yes_block,
            "mode=yes does not require the complete NXExtract set")
    no_config = copy.deepcopy(normalized_aarch64)
    no_config["nxextract"]["mode"] = "no"
    no_block = generator.render_nxextract_block(no_config)
    require("NXExtract: disabled" in no_block and
            "NXEXTRACT_REQUESTED=" not in no_block,
            "mode=no still carries an active NXExtract phase")

    literal_config = copy.deepcopy(normalized_aarch64)
    literal_config["required_files"] = [
        literal_config["executable"], "data/owner $literal * 'quote'.bin"
    ]
    required_block = generator.render_required_files_block(literal_config)
    required_assignment = "NXBOOTSTRAP_REQUIRED_FILES=" + \
        generator.shell_join(literal_config["required_files"])
    require(required_assignment in required_block and
            'readlink -f "$GAMEDIR/$required_file"' in required_block and
            '[ ! -s "$required_path" ]' in required_block and
            '[ -L "$GAMEDIR/$required_file" ]' in required_block,
            "required_files gate lost literal quoting or safety checks")

    reordered = copy.deepcopy(aarch64)
    reordered["required_capabilities"] = list(reversed(
        reordered["required_capabilities"]))
    normalized_reordered = generator.validate(reordered)
    require(normalized_reordered["required_capabilities"] ==
            normalized_aarch64["required_capabilities"],
            "capabilities are not canonicalized in registry order")

    legacy = {
        "schema_version": 1,
        "id": "legacy",
        "title": "Legacy",
        "launcher_name": "Legacy.sh",
        "architecture": "armv7",
        "executable": "legacy-loader",
    }
    upgraded = generator.validate(legacy)
    require(upgraded["input_schema_version"] == 1 and
            upgraded["schema_version"] == 2,
            "legacy v1 input was not deterministically upgraded")
    require(upgraded["private_library_paths"] == [] and
            upgraded["required_capabilities"] == [] and
            upgraded["enabled_quirks"] == [] and
            upgraded["runtime_report"] == "log-and-logo",
            "legacy v1 defaults were not fully materialized")

    invalid_cases = []
    base = copy.deepcopy(aarch64)
    case = copy.deepcopy(base)
    case["process_names"] = ["game"]
    invalid_cases.append(("process names", case))
    case = copy.deepcopy(base)
    case["schema_version"] = 3
    invalid_cases.append(("future schema", case))
    case = copy.deepcopy(base)
    case["executable"] = "/home/felipe/private-loader"
    invalid_cases.append(("absolute personal executable", case))
    case = copy.deepcopy(base)
    case["title"] = "build from /home/felipe/private"
    invalid_cases.append(("personal title", case))
    case = copy.deepcopy(base)
    case["required_files"] = ["../escape"]
    invalid_cases.append(("required file traversal", case))
    case = copy.deepcopy(base)
    case["private_library_paths"] = ["/usr/lib"]
    invalid_cases.append(("host library path", case))
    case = copy.deepcopy(base)
    case["nxextract"]["version"] = "1.2.4"
    invalid_cases.append(("stale NXExtract", case))
    case = copy.deepcopy(base)
    case["required_capabilities"] = ["host.portmaster", "host.portmaster"]
    invalid_cases.append(("duplicate capability", case))
    case = copy.deepcopy(base)
    case["required_capabilities"] = ["host.device.r36s"]
    invalid_cases.append(("device capability", case))
    case = copy.deepcopy(base)
    case["required_capabilities"] = ["host.unregistered-capability"]
    invalid_cases.append(("unregistered capability", case))
    case = copy.deepcopy(base)
    case["enabled_quirks"] = ["game.device.r36s"]
    invalid_cases.append(("device quirk", case))
    case = copy.deepcopy(base)
    case["enabled_quirks"] = ["npot_fix"]
    invalid_cases.append(("unnamespaced quirk", case))
    case = copy.deepcopy(base)
    case["runtime_report"] = "none"
    invalid_cases.append(("disabled reporting", case))
    for label, manifest in invalid_cases:
        expect_manifest_error(generator, manifest, label)


def check_generated_contract_sources():
    launcher_template = (
        BOOTSTRAP_ROOT / "templates" / "launcher.sh.in"
    ).read_text(encoding="utf-8")
    release = (
        REPO_ROOT / "framework/nxrelease/nxrelease.py"
    ).read_text(encoding="utf-8")
    require("# PORTMASTER: @PORT_ID@, @LAUNCHER_NAME@" in launcher_template,
            "launcher template lost the PORTMASTER header")
    require("run.sh" not in launcher_template,
            "run.sh is forbidden in generated ports")
    for token in (
            'source "$controlfolder/control.txt"',
            "mod_${NXBOOTSTRAP_MOD_NAME}.txt",
            "get_controls",
            'GAMEDIR="/$directory/ports/@PORT_ID@"',
            "export NXCOMPAT_PORT_ID=@PORT_ID_SHELL@",
            'export NXCOMPAT_GAME_DIR="$GAMEDIR"',
            "export NXCOMPAT_REQUIRED_CAPABILITIES="
            "@REQUIRED_CAPABILITIES_SHELL@",
            "export NXCOMPAT_ENABLED_QUIRKS=@ENABLED_QUIRKS_SHELL@",
            "export NXCOMPAT_RUNTIME_REPORT=@RUNTIME_REPORT_SHELL@",
            'exec > "$GAMEDIR/log.txt" 2>&1',
            '[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG',
            "pm_platform_helper",
            "pm_finish",
            'NXBOOTSTRAP_LOCK_FILE="$NXBOOTSTRAP_LOCK_DIR/nxport-',
            'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
            "NXBOOTSTRAP_LOCK_PATH_ID",
            "NXBOOTSTRAP_LOCK_FD_ID",
            '[ "$NXBOOTSTRAP_FINISHED" = 0 ] || return 0',
            "nxbootstrap_abort_before_game 129",
            "nxbootstrap_abort_before_game 130",
            "nxbootstrap_abort_before_game 143",
            "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
            'builtin kill -TERM "$game_pid"',
            "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
            'builtin kill -KILL "$game_pid"',
            "@NXEXTRACT_BLOCK@",
            "@REQUIRED_FILES_BLOCK@",
            "@HOME_BLOCK@",
            "@LIBRARY_BLOCK@",
            '! -L "$GAMEDIR/port-env.sh"'):
        require(token in launcher_template,
                "launcher template lost canonical element: %s" % token)
    require("eval " not in launcher_template and
            "pkill" not in launcher_template and
            "killall" not in launcher_template,
            "generated templates must not eval or kill by name")
    require("NXPORT_SCHEMA_VERSION = 2" in release and
            '"private_library_paths"' in release and
            '"required_capabilities"' in release and
            '"enabled_quirks"' in release,
            "nxrelease does not validate the current nxport contract")
    ast.parse(GENERATOR_PATH.read_text(encoding="utf-8"),
              filename=str(GENERATOR_PATH))
    ast.parse((REPO_ROOT / "framework/nxrelease/nxrelease.py").read_text(
        encoding="utf-8"), filename="nxrelease.py")


def check_m06_audit():
    audit = load_json(M06_AUDIT_PATH)
    require(audit.get("schema_version") == 1 and
            audit.get("milestone") == "M06",
            "M06 adversarial audit header changed")
    require(audit.get("scope") ==
            "local-static-filesystem-and-sealed-process-namespace" and
            audit.get("device_evidence") is False,
            "M06 audit incorrectly claims device evidence")
    requirements = audit.get("requirements")
    require(isinstance(requirements, list), "M06 requirements are missing")
    expected_ids = ["M06-%03d" % number for number in range(1, 31)]
    require([item.get("id") for item in requirements] == expected_ids,
            "M06 audit ids are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "implementation", "tests"},
                "M06 audit entry has an unknown field: %s" % item.get("id"))
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s has no %s evidence" % (item["id"], group))
            for reference in references:
                require(set(reference) == {"path", "token"},
                        "%s has malformed %s evidence" % (item["id"], group))
                relative = Path(reference.get("path", ""))
                require(not relative.is_absolute() and ".." not in relative.parts,
                        "%s evidence path escapes the repository" % item["id"])
                evidence_path = REPO_ROOT / relative
                require(evidence_path.is_file() and not evidence_path.is_symlink(),
                        "%s evidence file is missing/unsafe: %s" %
                        (item["id"], relative))
                token = reference.get("token")
                require(isinstance(token, str) and token and
                        token in evidence_path.read_text(encoding="utf-8"),
                        "%s evidence token is absent from %s: %r" %
                        (item["id"], relative, token))


def main():
    generator = load_generator()
    contract = load_json(CONTRACT_PATH)
    schema_v2 = load_json(SCHEMA_V2_PATH)
    schema_v1 = load_json(SCHEMA_V1_PATH)
    require(generator.CURRENT_SCHEMA_VERSION == 2 and
            generator.LEGACY_SCHEMA_VERSIONS == (1,),
            "generator schema compatibility boundary changed")
    check_component_contract(contract)
    check_schema(generator, schema_v2, schema_v1)
    check_examples(generator)
    check_generated_contract_sources()
    check_m06_audit()
    print("nxport manifest contract passed: schema v2, legacy v1 upgrade, 8 component locks, M06 30/30")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, KeyError, TypeError, ValueError) as error:
        print("nxport manifest contract failed: %s" % error, file=sys.stderr)
        sys.exit(1)
