#!/usr/bin/env python3
"""Static, process-free regression gate for the PortMaster contract."""

import json
import re
import sys
from pathlib import Path, PurePosixPath


PORTMASTER_ROOT = Path(__file__).resolve().parents[1]
FRAMEWORK_ROOT = PORTMASTER_ROOT.parent
BOOTSTRAP_ROOT = FRAMEWORK_ROOT / "nxbootstrap"
SOURCES_PATH = PORTMASTER_ROOT / "upstream-sources-v1.json"
CONTRACT_PATH = PORTMASTER_ROOT / "contract-v1.json"
FIXTURES_PATH = PORTMASTER_ROOT / "fixtures" / "contract-cases-v1.json"


def fail(message):
    raise AssertionError(message)


def require(condition, message):
    if not condition:
        fail(message)


def load_json(path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def shell_function(source, name):
    match = re.search(r"^%s\(\) \{\n" % re.escape(name), source, re.MULTILINE)
    require(match is not None, "missing shell function: %s" % name)
    next_match = re.search(r"^[a-z][a-z0-9_]*\(\) \{\n", source[match.end():],
                           re.MULTILINE)
    end = match.end() + next_match.start() if next_match else len(source)
    return source[match.start():end]


def assert_order(text, markers, label):
    position = -1
    for marker in markers:
        found = text.find(marker, position + 1)
        require(found >= 0, "%s is missing marker: %s" % (label, marker))
        require(found > position, "%s has an invalid order near: %s" %
                (label, marker))
        position = found


def walk_key_values(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from walk_key_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_key_values(child)


def check_source_manifest(sources):
    require(sources.get("schema_version") == 1,
            "source manifest schema must be version 1")
    source_entries = sources.get("sources")
    history = sources.get("funcs_history")
    require(isinstance(source_entries, list) and source_entries,
            "source manifest has no sources")
    require(isinstance(history, list) and history,
            "source manifest has no funcs history")

    all_entries = source_entries + history
    ids = [entry.get("id") for entry in all_entries]
    require(all(isinstance(item, str) and item for item in ids),
            "every source and funcs snapshot needs an id")
    require(len(ids) == len(set(ids)), "source ids are not unique")

    sha_pattern = re.compile(r"^[0-9a-f]{64}$")
    commit_pattern = re.compile(r"^[0-9a-f]{40}$")
    for key, value in walk_key_values(sources):
        if key.endswith("sha256"):
            require(isinstance(value, str) and sha_pattern.fullmatch(value),
                    "invalid SHA-256 in source manifest: %r" % value)
        if key == "commit":
            require(isinstance(value, str) and commit_pattern.fullmatch(value),
                    "invalid full commit id in source manifest: %r" % value)

    versions = [entry.get("version") for entry in history]
    require(versions == [1, 2, 3],
            "official funcs history must pin versions 1, 2 and 3 in order")

    for entry in source_entries:
        if entry.get("kind") == "local-firmware-integration":
            require(entry.get("universal_evidence") is False,
                    "local firmware integration was marked universal")

    negatives = sources.get("negative_evidence", [])
    require(negatives, "negative evidence list is empty")
    for entry in negatives:
        require(entry.get("scope") == "negative-only",
                "negative evidence lost its negative-only scope")
        require(entry.get("reuse_forbidden") is True,
                "negative global hook is no longer reuse-forbidden")

    policy = sources.get("reference_policy", {})
    require(policy.get("game_ports_used_as_m03_fixtures") == [],
            "M03 unexpectedly uses a game port as a fixture")
    require(policy.get("wip_sources_allowed") is False,
            "WIP sources became allowed")
    expected_limited = {
        "Pikmin", "PartyBoard", "GTA ports", "Bully", "Dysmantle",
        "LIMBO", "Chrono Trigger",
    }
    require(set(policy.get("limited_references_never_universal", [])) ==
            expected_limited,
            "limited game references changed or were generalized")
    return set(ids)


def check_contract(contract, sources):
    require(contract.get("schema_version") == 1,
            "PortMaster contract schema must be version 1")
    require(contract.get("source_manifest") == SOURCES_PATH.name,
            "contract does not point at its pinned source manifest")
    require(contract.get("principle") ==
            "detect first, correct second, never force by default",
            "the capability-first principle changed")

    funcs_contract = contract["api"]["funcs_txt"]["versions"]
    require([entry["version"] for entry in funcs_contract] == [1, 2, 3],
            "contract does not cover funcs versions 1, 2 and 3")
    history_by_version = {
        item["version"]: item for item in sources["funcs_history"]
    }
    require(set(history_by_version) == {1, 2, 3},
            "contract funcs versions have no matching source snapshots")

    package = contract["package_installation"]
    require(package.get("current_port_json_version") == 4,
            "current port.json version is not pinned to 4")
    require(package.get("accepted_legacy_versions") == [1, 2, 3],
            "legacy port.json upgrade set changed")
    require("trailing slash" in package.get("items_semantics", ""),
            "directory ownership no longer requires a trailing slash")
    require("top-level .sh" in package.get("split_root_semantics", ""),
            "HarbourMaster split-root install rule is missing")
    overlay = package.get("overlay_update_semantics", "")
    require("self-contained" in overlay and
            "generator version" in overlay,
            "overlay-safe launcher deployment rule is missing")
    security = " ".join(package.get("security", [])).lower()
    require("absolute" in security and "parent traversal" in security,
            "archive traversal protections are missing")

    filesystem = contract.get("filesystem_portability", {})
    filesystem_text = json.dumps(filesystem, ensure_ascii=False).lower()
    for fact in ("fat", "exfat", "executable", "symlink", "flock",
                 "bind_files", "capability"):
        require(fact in filesystem_text,
                "filesystem portability contract is missing %s" % fact)

    platform_observations = contract.get("platform_observations", [])
    require(platform_observations, "platform observations are empty")
    require(all(item.get("universal_evidence") is False
                for item in platform_observations),
            "a platform observation was promoted to universal evidence")

    generalizations = " ".join(contract.get("prohibited_generalizations", []))
    for token in ("Pikmin", "PartyBoard", "GTA", "Mali-450", "Panfrost",
                  "WIP"):
        require(token in generalizations,
                "missing prohibited generalization for %s" % token)

    frontend = " ".join(
        contract["frontend_and_signals"]["forbidden_framework_actions"]
    ).lower()
    for action in ("service", "desktop", "log out", "suspend", "reboot",
                   "power off", "setsid"):
        require(action in frontend,
                "frontend/session safety contract is missing %s" % action)


def check_fixtures(fixtures, valid_source_ids, contract):
    require(fixtures.get("schema_version") == 1,
            "fixture schema must be version 1")
    require(fixtures.get("contract") == CONTRACT_PATH.name,
            "fixtures point at the wrong contract")
    cases = fixtures.get("cases")
    require(isinstance(cases, list) and cases,
            "PortMaster fixtures are empty")
    ids = [case.get("id") for case in cases]
    require(len(ids) == len(set(ids)) and all(ids),
            "fixture ids are missing or duplicated")

    fixture_text = json.dumps(fixtures, ensure_ascii=False).lower()
    for forbidden_game in ("pikmin", "partyboard", "gta", "bully",
                           "dysmantle", "limbo", "chrono"):
        require(forbidden_game not in fixture_text,
                "game reference leaked into an M03 fixture: %s" %
                forbidden_game)
    require("wip" not in fixture_text,
            "a WIP source leaked into an M03 fixture")

    allowed_scopes = {
        "official-api-contract",
        "official-platform-integration",
        "local-firmware-integration",
        "negative-only",
    }
    for case in cases:
        require(case.get("universal_evidence") is False,
                "fixture %s was marked universal" % case.get("id"))
        require(case.get("evidence_scope") in allowed_scopes,
                "fixture %s has an unknown evidence scope" % case.get("id"))
        source_ids = case.get("source_ids", [])
        require(source_ids and set(source_ids) <= valid_source_ids,
                "fixture %s has an unpinned source" % case.get("id"))
        for path_text in case.get("source_paths", []):
            path = PurePosixPath(path_text)
            require(not path.is_absolute() and ".." not in path.parts,
                    "fixture source path escapes its source: %s" % path_text)
            require(not path.parts or path.parts[0].lower() != "ports",
                    "game port path used as M03 evidence: %s" % path_text)
        if case.get("evidence_scope") == "negative-only":
            require(case.get("expected", {}).get("reuse_forbidden") is True,
                    "negative fixture is not explicitly reuse-forbidden")

    funcs_by_version = {
        item["version"]: item for item in contract["api"]["funcs_txt"]["versions"]
    }
    funcs_cases = [case for case in cases
                   if case["id"].startswith("official-funcs-v")]
    require(len(funcs_cases) == 3, "fixtures do not cover all three funcs APIs")
    for case in funcs_cases:
        expected = case["expected"]
        version = expected["pm_funcs_version"]
        api = funcs_by_version[version]
        require(expected["required_functions"] == api["baseline_functions"],
                "funcs v%s fixture disagrees with the contract" % version)
        require(expected["optional_or_missing_functions"] ==
                api["not_guaranteed"],
                "funcs v%s optional capability list disagrees" % version)

    required_cases = {
        "official-generic-arkos-family",
        "official-rocknix-helper",
        "official-muos-split-root",
        "official-knulli-exfat",
        "official-harbourmaster-install",
        "nextos-portmaster-control-bridge",
        "nextos-global-rewrite-negative",
    }
    require(required_cases <= set(ids),
            "one or more required platform/installer fixtures are missing")


def strip_shell_comments(text):
    return "\n".join(line for line in text.splitlines()
                     if not line.lstrip().startswith("#"))


def check_implementation(contract):
    bootstrap_path = BOOTSTRAP_ROOT / "nxbootstrap.sh"
    source = bootstrap_path.read_text(encoding="utf-8")
    launcher = (BOOTSTRAP_ROOT / "templates" / "launcher.sh.in").read_text(
        encoding="utf-8")
    generator = (BOOTSTRAP_ROOT / "tools" / "generate-port.py").read_text(
        encoding="utf-8")

    main = shell_function(source, "nxbootstrap_main")
    assert_order(main, [
        "export PORT_32BIT=Y",
        "nxbootstrap_load_portmaster",
        "nxbootstrap_check_arch",
        "nxbootstrap_build_host_environment",
        "nxbootstrap_platform_prepare",
        "nxbootstrap_run_extractor",
        "nxbootstrap_run_prepare",
        "nxbootstrap_check_required_files",
        "nxbootstrap_build_runtime_environment",
        "nxbootstrap_launch",
    ], "nxbootstrap_main")

    loader = shell_function(source, "nxbootstrap_load_portmaster")
    assert_order(loader, [
        'source "$candidate/control.txt"',
        "controlfolder=$candidate",
        'mod_file=$candidate/mod_${CFW_NAME}.txt',
        'source "$mod_file"',
        "declare -F get_controls",
        "get_controls",
        "nxbootstrap_install_traps",
        "export NXCOMPAT_PORTMASTER_DIR=$candidate",
    ], "nxbootstrap_load_portmaster")
    require("! -L $controlfolder/control.txt" in loader and
            "! -L $candidate_root/control.txt" in loader,
            "one of the control.txt discovery routes follows symlinks")
    require(loader.count(
            'nxbootstrap_canonical_directory "$controlfolder"') >= 2,
            "ambient/published controlfolder is not physically canonicalized")
    require("! -L $mod_file" in loader,
            "platform mod discovery follows symlinks")
    require("*[!A-Za-z0-9._-]*" in loader,
            "CFW_NAME is no longer sanitized")
    require("PortMaster loaded (root=$candidate " in loader,
            "selected PortMaster root is not recorded in the runtime log")

    for root in contract["portmaster_discovery"]["known_roots"]:
        require(root in source,
                "nxbootstrap discovery is missing known root %s" % root)

    helper = shell_function(source, "nxbootstrap_platform_prepare")
    require("NXBOOTSTRAP_PLATFORM_PREPARED == 0" in helper,
            "platform helper lacks its exactly-once guard")
    require('pm_platform_helper "$NXBOOTSTRAP_BIN"' in helper,
            "platform helper does not receive the real executable")
    require("returned status $status; continuing" in helper,
            "platform helper failure is not logged as best effort")
    for marker in (
            "PM_PIPE is not a live non-symlink FIFO",
            "close API unavailable while PM_PIPE is active",
            "close API returned status $status",
            "PM_PIPE remained after close request"):
        require(marker in helper,
                "dialog handoff is not fail-closed for: %s" % marker)
    require("PortMaster dialog closed after platform helper" in helper,
            "successful dialog handoff is not recorded")

    finish = shell_function(source, "nxbootstrap_finish_once")
    require("NXBOOTSTRAP_FINISHED == 0" in finish and
            "NXBOOTSTRAP_FINISHED=1" in finish,
            "pm_finish lacks an exactly-once guard")
    require(source.count("if pm_finish; then") == 1,
            "pm_finish has more than one direct invocation")
    require("WARNING: pm_finish returned status" in finish,
            "pm_finish failure is not logged")

    library_builder = shell_function(source, "nxbootstrap_build_library_path")
    assert_order(library_builder, [
        "nxbootstrap_validate_private_library_dir",
        'nxbootstrap_add_library_dir "$controlfolder/libs"',
        "for directory in /usr/local",
        'IFS=: read -r -a nxbootstrap_old_dirs <<< "$old_path"',
        "nxbootstrap_add_library_dir /usr/lib",
    ], "nxbootstrap_build_library_path")
    private_validator = shell_function(
        source, "nxbootstrap_validate_private_library_dir")
    for provider in ("libEGL", "libGLES", "libGL", "libOpenGL", "libgbm",
                     "libdrm", "libmali", "libSDL", "libSDL2"):
        require(provider in private_validator,
                "private graphics-provider gate omits %s" % provider)

    require('PORT_32BIT=\\"Y\\"' in generator,
            "generator lost the literal ARMHF declaration")
    require("@PORT_32BIT_LITERAL@" in launcher,
            "visible launcher lost the ARMHF declaration slot")
    require(launcher.index("@PORT_32BIT_LITERAL@") <
            launcher.index('/opt/system/Tools/PortMaster'),
            "ARMHF declaration is not before launcher discovery")
    require(("@RUN_ARGS@ &" in launcher or "@RUN_ARGS@ 9>&- &" in launcher) and
            'wait "$game_pid"' in launcher,
            "visible launcher does not supervise the game as a direct child")
    require("@RUN_ARGS@ 9>&- &" in launcher,
            "visible launcher leaks the instance-lock fd into the game child")
    require(not re.search(
                r"nxbootstrap(?:-[0-9]+(?:[.][0-9]+)*)?[.]sh", launcher),
            "visible launcher still references the retired bootstrap library")
    for marker in ('NXBOOTSTRAP_LOCK_FILE="$NXBOOTSTRAP_LOCK_DIR/nxport-',
                   'exec 9>>"$NXBOOTSTRAP_LOCK_FILE"',
                   'NXBOOTSTRAP_LOCK_PATH_ID',
                   'NXBOOTSTRAP_LOCK_FD_ID',
                   'NXBOOTSTRAP_FINISHED=1',
                   "nxbootstrap_abort_before_game 129",
                   "nxbootstrap_abort_before_game 130",
                   "nxbootstrap_abort_before_game 143",
                   "NXBOOTSTRAP_CHILD_STARTTIME=${20}",
                   'builtin kill -TERM "$game_pid"',
                   "NXBOOTSTRAP_SHUTDOWN_TICKS=10",
                   'builtin kill -KILL "$game_pid"',
                   "trap '' INT TERM HUP",
                   "printf '\\033c'"):
        require(marker in launcher,
                "visible launcher lacks golden-port guarantee %s" % marker)
    require(launcher.index("flock -n 9") <
            launcher.index("@NXEXTRACT_BLOCK@"),
            "instance lock must precede the extraction phase")
    require("run.sh" not in launcher and 'port_dir / "run.sh"' not in generator,
            "generator retained the forbidden public run.sh layer")

    production_text = "\n".join((source, launcher, generator))
    production_commands = strip_shell_comments(production_text)
    forced_backend = re.compile(
        r"(?:export\s+)?(?:SDL_VIDEODRIVER|SDL_AUDIODRIVER|"
        r"SDL_VIDEO_GL_DRIVER|SDL_VIDEO_EGL_DRIVER)\s*=")
    require(not forced_backend.search(production_commands),
            "production bootstrap forces an SDL/video provider")
    forbidden_command = re.compile(
        r"(?<![A-Za-z0-9_-])(?:systemctl|loginctl|qdbus|dbus-send|"
        r"shutdown|reboot|poweroff|setsid|pgrep|pkill|killall)"
        r"(?![A-Za-z0-9_-])")
    match = forbidden_command.search(production_commands)
    require(match is None,
            "production bootstrap contains forbidden host command %s" %
            (match.group(0) if match else "unknown"))
    require("builtin kill -\"$signal\" \"$pid\"" in source,
            "exact-child signal helper is missing")
    require("nxbootstrap_matching_processes" not in source and
            "nxbootstrap_sweep" not in source,
            "host-wide process sweeping returned")


def main():
    sources = load_json(SOURCES_PATH)
    contract = load_json(CONTRACT_PATH)
    fixtures = load_json(FIXTURES_PATH)
    valid_source_ids = check_source_manifest(sources)
    check_contract(contract, sources)
    check_fixtures(fixtures, valid_source_ids, contract)
    check_implementation(contract)
    print("PortMaster contract gate passed: %d sources, %d fixtures, funcs v1-v3" %
          (len(valid_source_ids), len(fixtures["cases"])))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, KeyError, TypeError, ValueError) as error:
        print("PortMaster contract gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
