#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the invariant PortMaster wrapper/runtime from nxport.json."""

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import sys
import tempfile
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
CAPABILITY_REGISTRY_PATH = (
    ROOT.parent / "nxcompat" / "capabilities-v1.json"
)
QUIRK_REGISTRY_PATH = (
    ROOT.parent / "nxcompat" / "quirk-registry-v1.json"
)
CURRENT_SCHEMA_VERSION = 2
LEGACY_SCHEMA_VERSIONS = (1,)
NXEXTRACT_VERSION = "1.2.6"
NXBOOTSTRAP_VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", NXBOOTSTRAP_VERSION):
    raise RuntimeError("invalid nxbootstrap VERSION")
KNOWN_KEYS_V1 = {
    "schema_version",
    "id",
    "title",
    "launcher_name",
    "architecture",
    "executable",
    "argument_mode",
    "home_mode",
    "nxextract",
    "required_files",
    "extra_library_paths",
    "prepare_script",
}
KNOWN_KEYS_V2 = {
    "schema_version",
    "id",
    "title",
    "launcher_name",
    "architecture",
    "executable",
    "argument_mode",
    "home_mode",
    "nxextract",
    "required_files",
    "private_library_paths",
    "prepare_script",
    "required_capabilities",
    "enabled_quirks",
    "runtime_report",
}
PUBLIC_PATH_RE = re.compile(r"(?:^|/)(?:home|Users)/[^/]+", re.IGNORECASE)
WINDOWS_PATH_RE = re.compile(r"^[A-Za-z]:[\\/]")
CAPABILITY_RE = re.compile(
    r"^(?:host|graphics|audio|input)\.[a-z0-9][a-z0-9.-]{0,62}$"
)
QUIRK_RE = re.compile(
    r"^(?:adapter|engine|game)\.[a-z0-9][a-z0-9._-]{0,62}$"
)


def load_capability_registry():
    try:
        data = json.loads(CAPABILITY_REGISTRY_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("cannot load capability registry: %s" % error)
    if not isinstance(data, dict):
        raise RuntimeError("invalid capability registry root")
    entries = data.get("capabilities")
    if (data.get("schema_version") != 1 or
            data.get("default_required") is not False or
            not isinstance(entries, list)):
        raise RuntimeError("invalid capability registry header")
    identifiers = []
    for entry in entries:
        identifier = entry.get("id") if isinstance(entry, dict) else None
        if not isinstance(identifier, str) or not CAPABILITY_RE.fullmatch(identifier):
            raise RuntimeError("invalid capability registry identifier")
        identifiers.append(identifier)
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("duplicate capability registry identifier")
    return tuple(identifiers)


def load_quirk_registry():
    try:
        data = json.loads(QUIRK_REGISTRY_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("cannot load quirk registry: %s" % error)
    if not isinstance(data, dict):
        raise RuntimeError("invalid quirk registry root")
    entries = data.get("quirks")
    if (data.get("schema_version") != 1 or
            data.get("default_enabled") is not False or
            not isinstance(entries, list)):
        raise RuntimeError("invalid quirk registry header")
    identifiers = []
    for entry in entries:
        identifier = entry.get("id") if isinstance(entry, dict) else None
        if not isinstance(identifier, str) or not QUIRK_RE.fullmatch(identifier):
            raise RuntimeError("invalid quirk registry identifier")
        if not isinstance(entry.get("condition"), str) or \
                not isinstance(entry.get("effect"), str) or \
                not isinstance(entry.get("evidence"), list):
            raise RuntimeError(
                "quirk registry entry lacks condition/effect/evidence")
        identifiers.append(identifier)
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("duplicate quirk registry identifier")
    return tuple(identifiers)


CAPABILITY_REGISTRY_ERROR = None
try:
    CAPABILITY_IDENTIFIERS = load_capability_registry()
    QUIRK_IDENTIFIERS = load_quirk_registry()
except RuntimeError as error:
    CAPABILITY_IDENTIFIERS = ()
    QUIRK_IDENTIFIERS = ()
    CAPABILITY_REGISTRY_ERROR = str(error)
CAPABILITY_IDS = frozenset(CAPABILITY_IDENTIFIERS)
QUIRK_IDS = frozenset(QUIRK_IDENTIFIERS)
CAPABILITY_ORDER = {
    identifier: index
    for index, identifier in enumerate(CAPABILITY_IDENTIFIERS)
}


class ManifestError(Exception):
    pass


def require_string(data, key, allow_empty=False):
    value = data.get(key)
    if not isinstance(value, str) or (not allow_empty and not value):
        raise ManifestError("%s must be a%s string" %
                            (key, "n optionally empty" if allow_empty else " non-empty"))
    if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
           for character in value):
        raise ManifestError("%s contains a control character" % key)
    return value


def require_object(data, key):
    value = data.get(key)
    if not isinstance(value, dict):
        raise ManifestError("%s must be an object" % key)
    return value


def reject_personal_path(value, key):
    if PUBLIC_PATH_RE.search(value) or WINDOWS_PATH_RE.match(value):
        raise ManifestError("%s contains a personal or host path" % key)


def require_relative(value, key, allow_empty=False):
    if allow_empty and value == "":
        return value
    path = PurePosixPath(value)
    if len(value) > 512:
        raise ManifestError("%s is longer than 512 characters" % key)
    if path.is_absolute() or not path.parts or any(part in ("", ".", "..")
                                                   for part in path.parts):
        raise ManifestError("%s must be a normalized relative path" % key)
    reject_personal_path(value, key)
    return value


def string_list(data, key, paths=False):
    value = data.get(key, [])
    if not isinstance(value, list) or any(not isinstance(item, str) or not item
                                          for item in value):
        raise ManifestError("%s must be a list of non-empty strings" % key)
    if len(value) != len(set(value)):
        raise ManifestError("%s contains duplicates" % key)
    for item in value:
        if any(ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
               for character in item):
            raise ManifestError("%s contains a control character" % key)
        if paths:
            require_relative(item, key)
    return value


def named_list(data, key, pattern, allowed=None):
    values = string_list(data, key)
    for value in values:
        if not pattern.fullmatch(value):
            raise ManifestError("%s contains an invalid name: %s" % (key, value))
        if ".device." in ".%s." % value or value.startswith("device."):
            raise ManifestError("%s cannot select a device by name" % key)
        if allowed is not None and value not in allowed:
            raise ManifestError("%s contains an unknown name: %s" %
                                (key, value))
    return values


def normalize_legacy_v1(data):
    unknown = sorted(set(data) - KNOWN_KEYS_V1)
    if unknown:
        raise ManifestError("unknown field(s): %s" % ", ".join(unknown))
    normalized = dict(data)
    normalized["schema_version"] = CURRENT_SCHEMA_VERSION
    normalized["nxextract"] = {
        "mode": data.get("nxextract", "auto"),
        "version": NXEXTRACT_VERSION,
    }
    normalized["private_library_paths"] = data.get(
        "extra_library_paths", []
    )
    normalized.pop("extra_library_paths", None)
    normalized["required_capabilities"] = []
    normalized["enabled_quirks"] = []
    normalized["runtime_report"] = "log-and-logo"
    return normalized


def validate(data):
    if CAPABILITY_REGISTRY_ERROR is not None:
        raise ManifestError(CAPABILITY_REGISTRY_ERROR)
    if not isinstance(data, dict):
        raise ManifestError("manifest root must be an object")
    input_schema_version = data.get("schema_version")
    if input_schema_version in LEGACY_SCHEMA_VERSIONS:
        data = normalize_legacy_v1(data)
    elif input_schema_version != CURRENT_SCHEMA_VERSION:
        raise ManifestError(
            "schema_version must be %s (legacy input supported: %s)" %
            (CURRENT_SCHEMA_VERSION, ", ".join(
                str(item) for item in LEGACY_SCHEMA_VERSIONS))
        )
    unknown = sorted(set(data) - KNOWN_KEYS_V2)
    if unknown:
        raise ManifestError("unknown field(s): %s" % ", ".join(unknown))
    port_id = require_string(data, "id")
    if not re.match(r"^[a-z0-9][a-z0-9._-]{0,62}$", port_id):
        raise ManifestError("id must be a lowercase filesystem-safe identifier")
    title = require_string(data, "title")
    if len(title) > 128:
        raise ManifestError("title is longer than 128 characters")
    reject_personal_path(title, "title")
    launcher = require_string(data, "launcher_name")
    if len(launcher) > 160:
        raise ManifestError("launcher_name is longer than 160 characters")
    if Path(launcher).name != launcher or not launcher.endswith(".sh"):
        raise ManifestError("launcher_name must be a basename ending in .sh")
    architecture = require_string(data, "architecture")
    if architecture not in ("aarch64", "armv7", "x86_64", "i386"):
        raise ManifestError("unsupported architecture: %s" % architecture)
    executable = require_relative(require_string(data, "executable"), "executable")
    argument_mode = data.get("argument_mode", "game-dir-and-passthrough")
    if argument_mode not in ("none", "passthrough", "game-dir",
                             "game-dir-and-passthrough"):
        raise ManifestError("invalid argument_mode")
    nxextract_config = require_object(data, "nxextract")
    unknown_nxextract = sorted(set(nxextract_config) - {"mode", "version"})
    if unknown_nxextract:
        raise ManifestError("nxextract has unknown field(s): %s" %
                            ", ".join(unknown_nxextract))
    if set(nxextract_config) != {"mode", "version"}:
        raise ManifestError("nxextract requires mode and version")
    nxextract = require_string(nxextract_config, "mode")
    if nxextract not in ("auto", "yes", "no"):
        raise ManifestError("nxextract.mode must be auto, yes or no")
    nxextract_version = require_string(nxextract_config, "version")
    if nxextract_version != NXEXTRACT_VERSION:
        raise ManifestError("nxextract.version must be exactly %s" %
                            NXEXTRACT_VERSION)
    home_mode = data.get("home_mode", "preserve")
    if home_mode not in ("preserve", "port"):
        raise ManifestError("home_mode must be preserve or port")
    required = string_list(data, "required_files", paths=True)
    if executable not in required:
        required.insert(0, executable)
    libraries = string_list(data, "private_library_paths", paths=True)
    prepare = (require_string(data, "prepare_script", allow_empty=True)
               if "prepare_script" in data else "")
    require_relative(prepare, "prepare_script", allow_empty=True)
    capabilities = named_list(data, "required_capabilities", CAPABILITY_RE,
                              CAPABILITY_IDS)
    capabilities.sort(key=CAPABILITY_ORDER.__getitem__)
    quirks = named_list(data, "enabled_quirks", QUIRK_RE, QUIRK_IDS)
    runtime_report = data.get("runtime_report", "log-and-logo")
    if runtime_report not in ("log", "log-and-logo"):
        raise ManifestError("runtime_report must be log or log-and-logo")
    return {
        "schema_version": CURRENT_SCHEMA_VERSION,
        "input_schema_version": input_schema_version,
        "id": port_id,
        "title": title,
        "launcher_name": launcher,
        "architecture": architecture,
        "executable": executable,
        "argument_mode": argument_mode,
        "nxextract": {
            "mode": nxextract,
            "version": nxextract_version,
        },
        "home_mode": home_mode,
        "required_files": required,
        "private_library_paths": libraries,
        "prepare_script": prepare,
        "required_capabilities": capabilities,
        "enabled_quirks": quirks,
        "runtime_report": runtime_report,
    }


def render_nxextract_block(config):
    if config["nxextract"]["mode"] == "no":
        block = "# NXExtract: disabled for this port (nxextract.mode=no)."
    else:
        requested = "1" if config["nxextract"]["mode"] == "yes" else "0"
        auto_probe = ""
        if config["nxextract"]["mode"] == "auto":
            auto_probe = """
for nxprobe in "$GAMEDIR/extractor.json" \\
  "$GAMEDIR/nxextract/run-extractor.sh" "$GAMEDIR/nxextract/nxextract.py" \\
  "$GAMEDIR/nxextract/nxextract-runtime-env.sh" \\
  "$GAMEDIR/run-extractor.sh" "$GAMEDIR/nxextract.py" \\
  "$GAMEDIR/nxextract-runtime-env.sh"; do
  [ -e "$nxprobe" ] || [ -L "$nxprobe" ] || continue
  NXEXTRACT_REQUESTED=1
  break
done
unset nxprobe"""
        block = """\

# NXExtract owner-data phase (BYO data): runs before the game, other process.
NXDIR=""
[ -f "$GAMEDIR/nxextract/run-extractor.sh" ] && NXDIR="$GAMEDIR/nxextract"
[ -z "$NXDIR" ] && [ -f "$GAMEDIR/run-extractor.sh" ] && NXDIR="$GAMEDIR"
NXEXTRACT_REQUESTED=%s%s
if [ "$NXEXTRACT_REQUESTED" = 1 ]; then
  NXEXTRACT_INCOMPLETE=0
  [ -n "$NXDIR" ] && [ ! -L "$NXDIR" ] || NXEXTRACT_INCOMPLETE=1
  for nxfile in "$GAMEDIR/extractor.json" "$NXDIR/run-extractor.sh" \\
    "$NXDIR/nxextract.py" "$NXDIR/nxextract-runtime-env.sh"; do
    [ -f "$nxfile" ] && [ -s "$nxfile" ] && [ ! -L "$nxfile" ] || NXEXTRACT_INCOMPLETE=1
  done
  if [ "$NXEXTRACT_INCOMPLETE" = 1 ]; then
    echo "ERROR: incomplete NXExtract integration"
    nxbootstrap_finish
    exit 1
  fi
  $ESUDO chmod +x "$NXDIR/nxextract-ui" 2>/dev/null || true
  NXEXTRACT_GAME_DIR="$GAMEDIR" \\
    bash "$NXDIR/nxextract-runtime-env.sh" \\
    bash "$NXDIR/run-extractor.sh" || {
      echo "ERROR: game data installation did not complete"
      nxbootstrap_finish
      exit 1
    }
fi
unset NXDIR NXEXTRACT_REQUESTED NXEXTRACT_INCOMPLETE nxfile""" % (
            requested, auto_probe)
    if config["prepare_script"]:
        block += """

bash "$GAMEDIR/%s" || {
  echo "ERROR: prepare script failed"
  nxbootstrap_finish
  exit 1
}""" % config["prepare_script"]
    return block


def render_required_files_block(config):
    return """\

# Manifest-owned payload gate: extraction/prepare must finish before launch.
NXBOOTSTRAP_REQUIRED_FILES=%s
while IFS= read -r required_file; do
  [ -n "$required_file" ] || continue
  required_path=$(readlink -f "$GAMEDIR/$required_file" 2>/dev/null) || required_path=""
  case "$required_path" in
    "$GAMEDIR"/*) ;;
    *) required_path="" ;;
  esac
  if [ -z "$required_path" ] || [ ! -f "$required_path" ] || \\
     [ ! -s "$required_path" ] || [ -L "$GAMEDIR/$required_file" ]; then
    echo "ERROR: required file is missing or unsafe: $required_file"
    nxbootstrap_finish
    exit 1
  fi
done <<< "$NXBOOTSTRAP_REQUIRED_FILES"
unset NXBOOTSTRAP_REQUIRED_FILES required_file required_path""" % shell_join(
        config["required_files"])


def render_home_block(config):
    if config["home_mode"] == "port":
        return ('# Saves and configuration stay inside the port directory.\n'
                'export HOME="$GAMEDIR"')
    return "# HOME preserved (home_mode=preserve)."


def render_library_block(config):
    private = "".join(':$GAMEDIR/%s' % path
                      for path in config["private_library_paths"])
    if config["architecture"] == "armv7":
        return """\
# Firmware first; the device's 32-bit world (lib32 on muOS,
# arm-linux-gnueabihf on ArkOS/ROCKNIX) before our own; /usr/lib last (a
# 64-bit CFW's ld.so skips the wrong ELF class and keeps searching).
LIBS=""
for d in /usr/local/lib/arm-linux-gnueabihf /usr/local/lib32 \\
         /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf /usr/lib32 \\
         "$controlfolder/libs" "$controlfolder/libs.armhf" /usr/lib; do
  [ -n "$d" ] && [ -d "$d" ] && LIBS="${LIBS:+$LIBS:}$d"
done
export LD_LIBRARY_PATH="${LIBS}:$GAMEDIR%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# AArch64 CFWs route the ALSA default through PipeWire; the 32-bit process
# needs the 32-bit ALSA/PipeWire/SPA modules or SDL audio dies (TASM2).
for d in /usr/lib32 /usr/lib/arm-linux-gnueabihf /usr/local/lib/arm-linux-gnueabihf; do
  [ -d "$d/pipewire-0.3" ] && export PIPEWIRE_MODULE_DIR="$d/pipewire-0.3"
  [ -d "$d/spa-0.2" ] && export SPA_PLUGIN_DIR="$d/spa-0.2"
  [ -f "$d/alsa-lib/libasound_module_pcm_pipewire.so" ] && export ALSA_PLUGIN_DIR="$d/alsa-lib"
done""" % private
    return """\
# Firmware libraries first, then the game's own directory.
LIBS=""
for d in "$controlfolder/libs" /usr/lib/aarch64-linux-gnu /usr/lib64 /usr/lib; do
  [ -n "$d" ] && [ -d "$d" ] && LIBS="${LIBS:+$LIBS:}$d"
done
export LD_LIBRARY_PATH="${LIBS}:$GAMEDIR%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\
""" % private


def render(template_name, replacements):
    text = (ROOT / "templates" / template_name).read_text(encoding="utf-8")
    for name, value in replacements.items():
        text = text.replace("@%s@" % name, value)
    leftovers = sorted(set(re.findall(r"@[A-Z0-9_]+@", text)))
    if leftovers:
        raise ManifestError("unresolved template token(s): %s" % ", ".join(leftovers))
    return text


def atomic_write(path, content, mode, force):
    if (path.exists() or path.is_symlink()) and not force:
        raise ManifestError("refusing to overwrite %s (use --force)" % path)
    if path.parent.is_symlink() or not path.parent.is_dir():
        raise ManifestError("unsafe output parent: %s" % path.parent)
    descriptor, temporary = tempfile.mkstemp(prefix=".%s." % path.name,
                                              dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, str(path))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def shell_join(values):
    return shlex.quote("\n".join(values))


def render_launcher(config):
    port32 = ("PORT_32BIT=\"Y\"\nexport PORT_32BIT"
              if config["architecture"] == "armv7" else "")
    # Titles are embedded inside double-quoted shell strings and comments.
    safe_title = re.sub(r'[`"$\\]', "", config["title"])
    replacements = {
        "PORT_ID": config["id"],
        "PORT_ID_SHELL": shlex.quote(config["id"]),
        "REQUIRED_CAPABILITIES_SHELL": shell_join(
            config["required_capabilities"]),
        "ENABLED_QUIRKS_SHELL": shell_join(config["enabled_quirks"]),
        "RUNTIME_REPORT_SHELL": shlex.quote(config["runtime_report"]),
        "PORT_TITLE": safe_title,
        "PORT_TITLE_SHELL": shlex.quote(config["title"]),
        "LAUNCHER_NAME": config["launcher_name"],
        "PORT_EXECUTABLE": config["executable"],
        "NXBOOTSTRAP_VERSION": NXBOOTSTRAP_VERSION,
        "PORT_32BIT_LITERAL": port32,
        "NXEXTRACT_BLOCK": render_nxextract_block(config),
        "REQUIRED_FILES_BLOCK": render_required_files_block(config),
        "HOME_BLOCK": render_home_block(config),
        "LIBRARY_BLOCK": render_library_block(config),
        "RUN_ARGS": {
            "none": "",
            "passthrough": ' "$@"',
            "game-dir": ' "$GAMEDIR"',
            "game-dir-and-passthrough": ' "$GAMEDIR" "$@"',
        }[config["argument_mode"]],
    }
    return render("launcher.sh.in", replacements)


def generate(manifest_path, output, force):
    with manifest_path.open("r", encoding="utf-8") as stream:
        data = json.load(stream)
    config = validate(data)
    interpreters = {
        "aarch64": "/lib/ld-linux-aarch64.so.1",
        "armv7": "/lib/ld-linux-armhf.so.3",
        "x86_64": "/lib64/ld-linux-x86-64.so.2",
        "i386": "/lib/ld-linux.so.2",
    }
    canonical = {
        "schema_version": CURRENT_SCHEMA_VERSION,
        "id": config["id"],
        "title": config["title"],
        "launcher_name": config["launcher_name"],
        "architecture": config["architecture"],
        "executable": config["executable"],
        "argument_mode": config["argument_mode"],
        "home_mode": config["home_mode"],
        "nxextract": config["nxextract"],
        "required_files": config["required_files"],
        "private_library_paths": config["private_library_paths"],
        "prepare_script": config["prepare_script"],
        "required_capabilities": config["required_capabilities"],
        "enabled_quirks": config["enabled_quirks"],
        "runtime_report": config["runtime_report"],
    }
    canonical_text = json.dumps(
        canonical, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    output.mkdir(parents=True, exist_ok=True)
    if output.is_symlink() or not output.is_dir():
        raise ManifestError("output must be a real directory: %s" % output)
    port_dir = output / config["id"]
    if port_dir.exists() or port_dir.is_symlink():
        if port_dir.is_symlink() or not port_dir.is_dir():
            raise ManifestError("port output must be a real directory: %s" %
                                port_dir)
    else:
        port_dir.mkdir(mode=0o755)
    launcher_target = output / config["launcher_name"]
    manifest_target = port_dir / "nxport.json"
    targets = [launcher_target, manifest_target]
    if not force:
        existing = [str(path) for path in targets if path.exists() or path.is_symlink()]
        if existing:
            raise ManifestError("refusing to overwrite: %s (use --force)" %
                                ", ".join(existing))
    atomic_write(launcher_target, render_launcher(config), 0o755, force)
    atomic_write(manifest_target, canonical_text, 0o644, force)
    return config


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.output.is_symlink():
            raise ManifestError("output cannot be a symlink: %s" % args.output)
        resolved_output = args.output.resolve()
        config = generate(args.manifest.resolve(), resolved_output, args.force)
    except (OSError, ValueError, json.JSONDecodeError, ManifestError) as error:
        print("nxbootstrap generator: %s" % error, file=sys.stderr)
        return 1
    upgrade = ""
    if config["input_schema_version"] != config["schema_version"]:
        upgrade = " (upgraded schema v%s -> v%s)" % (
            config["input_schema_version"], config["schema_version"])
    print("generated %s (%s) in %s%s" %
          (config["id"], config["architecture"], resolved_output,
           upgrade))
    return 0


if __name__ == "__main__":
    sys.exit(main())
