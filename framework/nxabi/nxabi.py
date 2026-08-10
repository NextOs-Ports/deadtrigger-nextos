#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M17 ABI/toolchain gate: machine-readable ELF inventory and audit.

This is a host-side, read-only tool.  It never executes an inspected ELF, never
loads guest code and never touches a device.  GNU readelf and Python 3.7+ are
the only runtime requirements.

Subcommands
-----------
inventory   emit the machine-readable ELF inventory for files or directories
audit       apply the public universal policy and fail on any violation
sdl-table   regenerate sdl2-symbol-floor.tsv from a local SDL2 header tree
toolchain   verify the pinned toolchains described by TOOLCHAIN-PIN.json
"""

from __future__ import print_function

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

TOOL_NAME = "nxabi"
TOOL_VERSION = "0.1.0"
SCHEMA_VERSION = 1
MODULE_DIR = Path(__file__).resolve().parent
DEFAULT_POLICY = MODULE_DIR / "policy-v1.json"
DEFAULT_PIN = MODULE_DIR / "TOOLCHAIN-PIN.json"

GLIBC_RE = re.compile(r"\bGLIBC_(?:[0-9]+(?:\.[0-9]+)+|PRIVATE|ABI_[A-Za-z0-9_]+)\b")
GLIBCXX_RE = re.compile(r"\bGLIBCXX_(?:[0-9]+(?:\.[0-9]+)+)\b")
CXXABI_RE = re.compile(r"\bCXXABI_(?:[0-9]+(?:\.[0-9]+)+)\b")
VERSIONED_UND_RE = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)@@?(GLIBC|GLIBCXX|CXXABI)_([0-9][0-9.]*)$"
)
BIONIC_SONAMES = frozenset((
    "libc.so", "libdl.so", "libm.so", "liblog.so", "libandroid.so",
    "libGLESv2.so", "libEGL.so", "libOpenSLES.so", "libstdc++.so",
))


class AbiError(Exception):
    """A user-facing failure of the tool itself (not a policy finding)."""


# ---------------------------------------------------------------- utilities


def fail(message):
    raise AbiError(message)


def version_tuple(value):
    return tuple(int(part) for part in value.split("."))


def version_gt(left, right):
    return version_tuple(left) > version_tuple(right)


def max_version(values):
    if not values:
        return None
    return sorted(values, key=version_tuple)[-1]


def sha256_file(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_elf(path):
    try:
        with open(str(path), "rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def run_readelf(path, arguments):
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    process = subprocess.run(
        ["readelf"] + list(arguments) + ["--", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        env=environment,
    )
    if process.returncode != 0:
        detail = (process.stderr or process.stdout).strip()
        fail("readelf {} rejected {}: {}".format(" ".join(arguments), path, detail))
    return process.stdout


# ------------------------------------------------------------- ELF inspection


def inspect_elf(path, logical_path=None):
    """Return every ABI fact the gate needs about one ELF file."""
    logical = logical_path or str(path)
    header = run_readelf(path, ("-hW",))

    def header_field(name):
        match = re.search(
            r"^\s*{}:\s*(.+?)\s*$".format(name), header, re.MULTILINE
        )
        return match.group(1) if match else None

    elf_class = header_field("Class")
    data = header_field("Data")
    elf_type = header_field("Type")
    machine = header_field("Machine")
    flags = header_field("Flags") or ""
    os_abi = header_field("OS/ABI")

    program_headers = run_readelf(path, ("-lW",))
    load_count = len(re.findall(r"^\s*LOAD\s", program_headers, re.MULTILINE))
    interpreters = re.findall(
        r"Requesting program interpreter:\s*([^\]]+)\]", program_headers
    )
    gnu_stack = re.search(
        r"^\s*GNU_STACK\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+([RWE ]+)",
        program_headers,
        re.MULTILINE,
    )

    dynamic = run_readelf(path, ("-dW",))
    needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic)
    sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    rpath = re.findall(r"\(RPATH\).*?\[([^\]]*)\]", dynamic)
    runpath = re.findall(r"\(RUNPATH\).*?\[([^\]]*)\]", dynamic)

    versions = run_readelf(path, ("--version-info", "--wide"))
    glibc_tokens = sorted(set(GLIBC_RE.findall(versions)))
    glibc_numeric = sorted(
        {token[len("GLIBC_"):] for token in glibc_tokens
         if re.match(r"^GLIBC_[0-9]", token)},
        key=version_tuple,
    )
    glibcxx_numeric = sorted(
        {token[len("GLIBCXX_"):] for token in set(GLIBCXX_RE.findall(versions))},
        key=version_tuple,
    )
    cxxabi_numeric = sorted(
        {token[len("CXXABI_"):] for token in set(CXXABI_RE.findall(versions))},
        key=version_tuple,
    )
    forbidden_tokens = sorted(
        token for token in glibc_tokens if not re.match(r"^GLIBC_[0-9]", token)
    )

    undefined = undefined_symbols(path)
    glibc_max = max_version(glibc_numeric)
    floor_symbols = sorted(
        name for name, (library, version) in undefined.items()
        if library == "GLIBC" and glibc_max is not None and version == glibc_max
    )

    notes = run_readelf(path, ("-nW",))
    build_id_match = re.search(r"Build ID:\s*([0-9a-f]+)", notes)
    toolchain_note = extract_toolchain_note(path)

    architecture = {"ARM": "armv7", "AArch64": "aarch64"}.get(machine)
    interpreter = interpreters[0].strip() if interpreters else None

    record = {
        "path": logical,
        "sha256": sha256_file(path),
        "size": os.path.getsize(str(path)),
        "build_id": build_id_match.group(1) if build_id_match else None,
        "class": elf_class,
        "data": data,
        "elf_type": elf_type,
        "machine": machine,
        "flags": flags,
        "os_abi": os_abi,
        "architecture": architecture,
        "namespace": classify_namespace(interpreter, needed, glibc_numeric),
        "pt_load_count": load_count,
        "pt_interp": interpreter,
        "pt_interp_count": len(interpreters),
        "pt_gnu_stack": (gnu_stack.group(1).strip() if gnu_stack else None),
        "needed": sorted(needed),
        "needed_raw_count": len(needed),
        "soname": sonames[0] if sonames else None,
        "soname_count": len(sonames),
        "rpath": rpath,
        "runpath": runpath,
        "glibc_versions": glibc_numeric,
        "glibc_max": glibc_max,
        "glibc_floor_symbols": floor_symbols,
        "glibcxx_max": max_version(glibcxx_numeric),
        "cxxabi_max": max_version(cxxabi_numeric),
        "forbidden_version_tokens": forbidden_tokens,
        "undefined_symbols": sorted(undefined),
        "undefined_sdl": sorted(
            name for name in undefined if name.startswith("SDL_")
        ),
        "toolchain_note": toolchain_note,
    }
    return record


def undefined_symbols(path):
    """Map every undefined dynamic symbol to (library, version) when versioned."""
    output = run_readelf(path, ("-sW", "--dyn-syms"))
    result = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        if fields[6] != "UND":
            continue
        raw = fields[7]
        match = VERSIONED_UND_RE.match(raw)
        if match:
            # readelf can print the same undefined symbol twice (once per
            # symbol table, with @VER and @@VER).  Never let an unversioned
            # or lower entry overwrite a versioned one.
            name, library, version = match.groups()
            known = result.get(name)
            if known is None or known[1] is None or version_gt(version, known[1]):
                result[name] = (library, version)
        else:
            name = raw.split("@")[0]
            if name not in result:
                result[name] = (None, None)
    result.pop("", None)
    return result


def classify_namespace(interpreter, needed, glibc_numeric):
    if interpreter and interpreter.startswith("/system/bin/linker"):
        return "android"
    if glibc_numeric:
        return "linux"
    if any(item in BIONIC_SONAMES for item in needed):
        return "android"
    return "linux"


def extract_toolchain_note(path):
    """Read .comment and the project's own .note.nx.toolchain section, if any."""
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    notes = []
    for section in (".comment", ".note.nx.toolchain"):
        process = subprocess.run(
            ["readelf", "-p", section, "--", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
            env=environment,
        )
        if process.returncode != 0:
            continue
        for line in process.stdout.splitlines():
            match = re.match(r"^\s*\[\s*[0-9a-f]+\]\s+(.*\S)\s*$", line)
            if match:
                notes.append(match.group(1))
    unique = []
    for note in notes:
        if note not in unique:
            unique.append(note)
    return unique or None


# ------------------------------------------------------------------ discovery


def discover(targets, follow_symlinks=False):
    """Return every ELF under the given files/directories, sorted by path."""
    found = []
    seen = set()
    for target in targets:
        base = Path(target)
        if not base.exists():
            fail("missing path: {}".format(target))
        if base.is_dir():
            candidates = sorted(base.rglob("*"))
        else:
            candidates = [base]
        for candidate in candidates:
            if candidate.is_symlink() and not follow_symlinks:
                continue
            if not candidate.is_file():
                continue
            resolved = str(candidate.resolve())
            if resolved in seen:
                continue
            if not is_elf(candidate):
                continue
            seen.add(resolved)
            found.append(candidate)
    return sorted(found, key=lambda item: str(item))


# --------------------------------------------------------------------- policy


def load_policy(path):
    try:
        with open(str(path), "r", encoding="utf-8") as stream:
            policy = json.load(stream)
    except (OSError, ValueError) as error:
        fail("cannot load policy {}: {}".format(path, error))
    if policy.get("schema_version") != 1:
        fail("unsupported policy schema in {}".format(path))
    return policy


def load_sdl_table(policy, policy_path):
    table_name = policy.get("sdl", {}).get("table")
    if not table_name:
        return {}
    table_path = Path(policy_path).resolve().parent / table_name
    if not table_path.exists():
        return {}
    table = {}
    with open(str(table_path), "r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            source = parts[2] if len(parts) > 2 else "unknown"
            table[parts[0]] = (parts[1], source)
    return table


def finding(level, record, check, message):
    return {
        "level": level,
        "path": record["path"],
        "check": check,
        "message": message,
    }


def apply_exceptions(findings, record, policy):
    """Downgrade a finding to a warning when the artifact has a signed waiver.

    A waiver is keyed by the artifact sha256, so it dies the moment the binary
    is rebuilt.  It never hides the finding; it only stops it from failing the
    gate for an artifact that already has physical evidence.
    """
    waivers = policy.get("exceptions", {})
    waiver = waivers.get(record["sha256"])
    if not waiver:
        return findings
    waived_checks = set(waiver.get("checks", ()))
    result = []
    for item in findings:
        if item["level"] == "error" and item["check"] in waived_checks:
            item = dict(item)
            item["level"] = "warn"
            item["waived"] = True
            item["waiver_reason"] = waiver.get("reason", "")
            item["message"] += " [waived: {}]".format(
                waiver.get("reason", "no reason recorded"))
        result.append(item)
    return result


def audit_record(record, policy, sdl_table, options):
    """Apply the whole policy to one ELF and return a list of findings."""
    out = []
    ceilings = policy["ceilings"]
    profile = options.get("profile", "universal-low-glibc")
    profiles = policy.get("build_profiles", {})
    is_public = profiles.get(profile, {}).get("public", True)
    namespace = record["namespace"]

    # M17-006: an Android BYO ELF is never a Linux build of the project.
    if namespace == "android":
        if record["glibc_versions"] or record["forbidden_version_tokens"]:
            out.append(finding(
                "error", record, "android-namespace",
                "tagged Android but references GLIBC symbol versions",
            ))
        expected = policy["android_interpreters"].get(record["architecture"])
        if record["pt_interp"] and record["pt_interp"] != expected:
            out.append(finding(
                "error", record, "android-interpreter",
                "Android ELF has PT_INTERP {} (expected {})".format(
                    record["pt_interp"], expected),
            ))
        out.append(finding(
            "info", record, "android-upstream",
            "Android upstream ELF; not counted as a Linux build of the project",
        ))
        return out

    # M17-007: class, endianness, machine and float ABI.
    if record["data"] is None or "little endian" not in record["data"]:
        out.append(finding("error", record, "endianness",
                           "ELF is not little-endian"))
    if record["elf_type"] not in ("EXEC (Executable file)",
                                  "DYN (Shared object file)",
                                  "DYN (Position-Independent Executable file)"):
        out.append(finding("error", record, "elf-type",
                           "unexpected ELF type {}".format(record["elf_type"])))
    architecture = record["architecture"]
    if architecture is None:
        out.append(finding("error", record, "machine",
                           "unsupported machine {}".format(record["machine"])))
        return out
    spec = policy["architectures"][architecture]
    if record["class"] != spec["elf_class"]:
        out.append(finding("error", record, "elf-class",
                           "class {} disagrees with {}".format(
                               record["class"], architecture)))
    for required in spec["required_flags"]:
        if required not in record["flags"]:
            out.append(finding("error", record, "float-abi",
                               "missing required ELF flag '{}' (flags: {})".format(
                                   required, record["flags"] or "none")))

    # M17-008: PT_LOAD / PT_INTERP / PT_GNU_STACK.
    if record["pt_load_count"] < 1:
        out.append(finding("error", record, "pt-load", "no PT_LOAD segment"))
    if record["pt_interp_count"] > 1:
        out.append(finding("error", record, "pt-interp",
                           "multiple PT_INTERP segments"))
    interpreter = record["pt_interp"]
    if interpreter is not None:
        if not interpreter.startswith("/"):
            out.append(finding("error", record, "pt-interp",
                               "non-absolute PT_INTERP {}".format(interpreter)))
        elif interpreter.startswith("/home/") or interpreter.startswith("/Users/"):
            out.append(finding("error", record, "pt-interp",
                               "PT_INTERP embeds a personal path"))
        elif interpreter != spec["interpreter"]:
            out.append(finding("error", record, "pt-interp",
                               "PT_INTERP must be {}; got {}".format(
                                   spec["interpreter"], interpreter)))
    stack = record["pt_gnu_stack"]
    if stack and "E" in stack:
        out.append(finding("error", record, "pt-gnu-stack",
                           "executable stack ({})".format(stack)))
    if stack is None:
        out.append(finding("warn", record, "pt-gnu-stack",
                           "no PT_GNU_STACK segment; the kernel assumes an "
                           "executable stack"))

    # M17-009: DT_NEEDED / SONAME / RPATH / RUNPATH.
    if record["needed_raw_count"] != len(set(record["needed"])):
        out.append(finding("error", record, "dt-needed",
                           "repeated DT_NEEDED entry"))
    if record["soname_count"] > 1:
        out.append(finding("error", record, "dt-soname",
                           "multiple DT_SONAME values"))
    allowlist = policy["soname_allowlist"]
    forbidden = policy["soname_forbidden"]
    for item in record["needed"]:
        if item in forbidden:
            out.append(finding("error", record, "soname-forbidden",
                               "DT_NEEDED {} is forbidden: {}".format(
                                   item, forbidden[item])))
        elif item not in allowlist:
            out.append(finding("warn", record, "soname-unknown",
                               "DT_NEEDED {} is not in the policy allowlist"
                               .format(item)))
    search = policy["search_paths"]
    for kind, values in (("RPATH", record["rpath"]), ("RUNPATH", record["runpath"])):
        for value in values:
            allowed = search["allow_rpath"] if kind == "RPATH" else search["allow_runpath"]
            if allowed:
                continue
            if search.get("allow_origin_only") and value == "$ORIGIN":
                out.append(finding("warn", record, "search-path",
                                   "{} $ORIGIN accepted by policy exception"
                                   .format(kind)))
                continue
            out.append(finding("error", record, "search-path",
                               "embeds {}=[{}]; universal packages require none"
                               .format(kind, value)))
    for value in record["rpath"] + record["runpath"]:
        if value.startswith("/home/") or value.startswith("/Users/"):
            out.append(finding("error", record, "search-path",
                               "{} embeds a personal path".format(value)))

    # M17-003 / M17-010: GLIBC, GLIBCXX and CXXABI ceilings.
    for token in record["forbidden_version_tokens"]:
        out.append(finding("error", record, "version-token",
                           "requires private/unsupported ABI token {}"
                           .format(token)))
    checks = (
        ("glibc", record["glibc_max"], ceilings["glibc_max"], "GLIBC"),
        ("glibcxx", record["glibcxx_max"], ceilings["glibcxx_max"], "GLIBCXX"),
        ("cxxabi", record["cxxabi_max"], ceilings["cxxabi_max"], "CXXABI"),
    )
    for check, value, ceiling, label in checks:
        if value is None:
            continue
        if version_gt(value, ceiling):
            level = "error" if is_public else "warn"
            out.append(finding(level, record, check + "-ceiling",
                               "requires {}_{} (ceiling {}_{})".format(
                                   label, value, label, ceiling)))
    # M17-002: prefer the lowest viable floor and name what raises it.
    glibc_max = record["glibc_max"]
    preferred = ceilings["glibc_preferred"]
    if glibc_max and version_gt(glibc_max, preferred):
        out.append(finding(
            "warn", record, "glibc-preferred",
            "floor is GLIBC_{} (preferred {}); raised by: {}".format(
                glibc_max, preferred,
                ", ".join(record["glibc_floor_symbols"]) or "unknown"),
        ))

    # M17-015: glibc wrappers that are newer than the preferred floor.
    blacklist = policy["wrapper_blacklist"]
    for symbol in record["undefined_symbols"]:
        since = blacklist.get(symbol)
        if since is None:
            continue
        level = "error" if version_gt(since, preferred) else "warn"
        out.append(finding(
            level, record, "new-libc-api",
            "imports {} (glibc wrapper since {}); use syscall() or a shim"
            .format(symbol, since),
        ))

    # M17-014: SDL API floor.
    sdl_floor = options.get("sdl_floor") or policy.get("sdl", {}).get("floor")
    if sdl_floor and record["undefined_sdl"]:
        missing = []
        assumed = []
        for symbol in record["undefined_sdl"]:
            entry = sdl_table.get(symbol)
            if entry is None:
                missing.append(symbol)
                continue
            since, source = entry
            if version_gt(since, sdl_floor):
                out.append(finding(
                    "error", record, "sdl-floor",
                    "imports {} (SDL {}) above the declared floor SDL {}"
                    .format(symbol, since, sdl_floor),
                ))
            elif source == "assumed-baseline":
                assumed.append(symbol)
        if missing:
            out.append(finding(
                "warn", record, "sdl-unknown",
                "{} SDL symbol(s) absent from the floor table: {}".format(
                    len(missing), ", ".join(missing[:8])),
            ))
        if assumed:
            out.append(finding(
                "info", record, "sdl-assumed",
                "{} SDL symbol(s) have no \\since annotation and are assumed "
                "to be SDL 2.0.0 baseline".format(len(assumed)),
            ))

    # M17-018: provenance must be recoverable from the artifact itself.
    if record["build_id"] is None:
        out.append(finding("warn", record, "build-id",
                           "no GNU build-id note; provenance is not verifiable "
                           "from the artifact"))
    if record["toolchain_note"] is None:
        out.append(finding("warn", record, "toolchain-note",
                           "no .comment or .note.nx.toolchain; the producing "
                           "toolchain cannot be recovered from the artifact"))
    return out


# ------------------------------------------------------------------- commands


def build_inventory(targets, policy_path, follow_symlinks=False):
    files = discover(targets, follow_symlinks=follow_symlinks)
    records = [inspect_elf(item) for item in files]
    return {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "policy": str(policy_path),
        "count": len(records),
        "elves": records,
    }


def command_inventory(arguments):
    inventory = build_inventory(
        arguments.targets, arguments.policy, arguments.follow_symlinks
    )
    emit_json(inventory, arguments.json)
    if not arguments.json:
        return 0
    print("{}: inventory of {} ELF(s) -> {}".format(
        TOOL_NAME, inventory["count"], arguments.json))
    return 0


def command_audit(arguments):
    policy = load_policy(arguments.policy)
    sdl_table = load_sdl_table(policy, arguments.policy)
    inventory = build_inventory(
        arguments.targets, arguments.policy, arguments.follow_symlinks
    )
    options = {"profile": arguments.profile, "sdl_floor": arguments.sdl_floor}
    findings = []
    for record in inventory["elves"]:
        findings.extend(apply_exceptions(
            audit_record(record, policy, sdl_table, options), record, policy))
    if not inventory["elves"]:
        fail("no ELF file found in the given targets")

    errors = [item for item in findings if item["level"] == "error"]
    warnings = [item for item in findings if item["level"] == "warn"]
    report = {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "policy_id": policy["policy_id"],
        "profile": arguments.profile,
        "ceilings": policy["ceilings"],
        "counts": {
            "elves": inventory["count"],
            "errors": len(errors),
            "warnings": len(warnings),
        },
        "findings": findings,
        "inventory": inventory["elves"] if arguments.embed_inventory else [],
        # The catalog rows below are recorded claims; this report is the
        # executed evidence that backs them (M17-016, M17-020).
        "catalog_claims_backed": [
            "ABI-*-015-host-elf-class",
            "ABI-*-016-host-machine",
            "ABI-*-017-host-float-abi",
            "ABI-*-018-host-pt-interp",
            "ABI-*-019-host-glibc-ceiling",
            "ABI-*-020-host-dependencies",
        ],
    }
    emit_json(report, arguments.json)

    if not arguments.quiet:
        for item in findings:
            if item["level"] == "info" and not arguments.verbose:
                continue
            if item["level"] == "warn" and arguments.errors_only:
                continue
            print("{}: {}: {}: {}".format(
                item["level"].upper(), item["path"], item["check"],
                item["message"]))
        print("{}: {} ELF(s), {} error(s), {} warning(s)".format(
            TOOL_NAME, inventory["count"], len(errors), len(warnings)))
    if errors:
        return 1
    if warnings and arguments.strict:
        return 1
    return 0


def command_sdl_table(arguments):
    headers = Path(arguments.headers)
    if not headers.is_dir():
        fail("not a directory: {}".format(headers))
    since_re = re.compile(
        r"\\since This function is available since SDL ([0-9]+\.[0-9]+\.[0-9]+)"
    )
    # SDL declares plenty of functions across several lines, so the scan works
    # on the whole file and pairs each declaration with the nearest preceding
    # "\since" annotation instead of matching line by line.
    decl_re = re.compile(r"SDLCALL\s+(SDL_[A-Za-z0-9_]+)\s*\(", re.MULTILINE)
    table = {}
    for header in sorted(headers.glob("*.h")):
        text = header.read_text(encoding="utf-8", errors="replace")
        annotations = [
            (match.start(), match.group(1)) for match in since_re.finditer(text)
        ]
        previous_end = 0
        for declaration in decl_re.finditer(text):
            name = declaration.group(1)
            offset = declaration.start()
            version = None
            # Only an annotation inside this declaration's own doc block
            # counts; otherwise a documented function would lend its version
            # to the next, undocumented one.
            for position, value in annotations:
                if previous_end <= position < offset:
                    version = value
                elif position >= offset:
                    break
            previous_end = declaration.end()
            source = "sdl2-headers" if version else "assumed-baseline"
            version = version or "2.0.0"
            known = table.get(name)
            if known is None or version_gt(version, known[0]):
                table[name] = (version, source)
    lines = [
        "# SPDX-License-Identifier: GPL-3.0-or-later",
        "# symbol\tsdl_version\tsource",
        "# Generated by nxabi sdl-table from {}".format(headers),
        "# Symbols without a \\since annotation are recorded as 2.0.0 and the",
        "# audit reports them as unknown rather than silently passing.",
    ]
    for name in sorted(table):
        version, source = table[name]
        lines.append("{}\t{}\t{}".format(name, version, source))
    output = "\n".join(lines) + "\n"
    if arguments.out:
        Path(arguments.out).write_text(output, encoding="utf-8")
        print("{}: wrote {} symbols to {}".format(
            TOOL_NAME, len(table), arguments.out))
    else:
        sys.stdout.write(output)
    return 0


def command_provenance(arguments):
    """Emit a C translation unit that stamps the toolchain into the ELF.

    M17-018: a stripped artifact keeps no .comment, so the producing toolchain
    cannot be recovered from the binary.  Compiling this file into the artifact
    puts an auditable .note.nx.toolchain string inside it.
    """
    pin_path = Path(arguments.pin)
    try:
        with open(str(pin_path), "r", encoding="utf-8") as stream:
            pin = json.load(stream)
    except (OSError, ValueError) as error:
        fail("cannot load toolchain pin {}: {}".format(pin_path, error))
    entry = pin.get("toolchains", {}).get(arguments.toolchain)
    if entry is None:
        fail("unknown toolchain {!r} in {}".format(arguments.toolchain, pin_path))

    fields = [
        ("toolchain", arguments.toolchain),
        ("kind", entry.get("kind", "")),
        ("architecture", entry.get("architecture", "")),
        ("compiler", entry.get("version_full") or entry.get("compiler", "")),
        ("image", entry.get("image", "")),
        ("image_id", entry.get("image_id", "")),
        ("sysroot", entry.get("sysroot", "")),
        ("sysroot_glibc", entry.get("sysroot_glibc", "")),
        ("profile", arguments.profile),
        ("pin_id", pin.get("pin_id", "")),
    ]
    if arguments.source_date_epoch:
        fields.append(("source_date_epoch", arguments.source_date_epoch))
    payload = "; ".join(
        "{}={}".format(key, value) for key, value in fields if value
    )
    if '"' in payload or "\\" in payload:
        fail("toolchain pin contains a character that cannot be embedded")
    text = (
        "/* SPDX-License-Identifier: GPL-3.0-or-later\n"
        " * Generated by {} {} -- do not edit.\n"
        " * Compile this file into the artifact so that\n"
        " *   readelf -p .note.nx.toolchain <artifact>\n"
        " * recovers the toolchain that produced it (M17-018).\n"
        " */\n"
        "__attribute__((used, section(\".note.nx.toolchain\")))\n"
        "static const char nx_toolchain_note[] =\n"
        "    \"{}\";\n"
    ).format(TOOL_NAME, TOOL_VERSION, payload)
    if arguments.out:
        Path(arguments.out).write_text(text, encoding="utf-8")
        print("{}: wrote provenance stamp to {}".format(TOOL_NAME, arguments.out))
    else:
        sys.stdout.write(text)
    return 0


def command_toolchain(arguments):
    pin_path = Path(arguments.pin)
    try:
        with open(str(pin_path), "r", encoding="utf-8") as stream:
            pin = json.load(stream)
    except (OSError, ValueError) as error:
        fail("cannot load toolchain pin {}: {}".format(pin_path, error))
    if pin.get("schema_version") != 1:
        fail("unsupported toolchain pin schema")

    results = []
    failures = 0
    for name, entry in sorted(pin.get("toolchains", {}).items()):
        status, detail = verify_toolchain(name, entry)
        results.append({"toolchain": name, "status": status, "detail": detail})
        # A pin is useful only when the exact toolchain is locally available.
        # "absent" remains a diagnostic from verify_toolchain(), but the
        # release-facing command is fail-closed rather than silently partial.
        if status != "ok":
            failures += 1
        if not arguments.quiet:
            print("{}: {:<24} {:<8} {}".format(TOOL_NAME, name, status, detail))
    emit_json({
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "pin": str(pin_path),
        "results": results,
    }, arguments.json)
    return 1 if failures else 0


def verify_toolchain(name, entry):
    kind = entry.get("kind")
    if kind == "native-cross":
        compiler = Path(entry["compiler"])
        if not compiler.exists():
            return "fail", "missing compiler {}".format(compiler)
        environment = dict(os.environ)
        environment["LC_ALL"] = "C"
        process = subprocess.run(
            [str(compiler), "--version"], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, universal_newlines=True, env=environment)
        first = (process.stdout or "").splitlines()
        first = first[0] if first else ""
        expected = entry.get("version_string")
        if expected and expected not in first:
            return "fail", "version drift: {!r} does not contain {!r}".format(
                first, expected)
        sysroot = entry.get("sysroot")
        if sysroot and not Path(sysroot).is_dir():
            return "fail", "missing sysroot {}".format(sysroot)
        for relative, expected_hash in sorted(
                entry.get("sysroot_sha256", {}).items()):
            target = Path(sysroot) / relative
            if not target.exists():
                return "fail", "missing {}".format(target)
            actual = sha256_file(target)
            if actual != expected_hash:
                return "fail", "sha256 drift on {}: {}".format(relative, actual)
        return "ok", first
    if kind == "container":
        docker = entry.get("docker", "docker")
        process = subprocess.run(
            [docker, "image", "inspect", "--format", "{{.Id}}", entry["image"]],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            universal_newlines=True)
        if process.returncode != 0:
            return "absent", "image {} not present locally".format(entry["image"])
        resolved = process.stdout.strip()
        if resolved != entry["image_id"]:
            return "fail", "image drift: {} != {}".format(
                resolved, entry["image_id"])
        return "ok", "{} {}".format(entry["image"], resolved[:19])
    return "fail", "unknown toolchain kind {!r}".format(kind)


def emit_json(payload, destination):
    if not destination:
        return
    text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if destination == "-":
        sys.stdout.write(text)
        return
    Path(destination).write_text(text, encoding="utf-8")


# ---------------------------------------------------------------------- entry


def build_parser():
    parser = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="M17 ABI/toolchain gate (host-side, never executes an ELF)",
    )
    parser.add_argument("--version", action="version",
                        version="{} {}".format(TOOL_NAME, TOOL_VERSION))
    subparsers = parser.add_subparsers(dest="command")

    def add_common(target):
        target.add_argument("targets", nargs="+",
                            help="files or directories to inspect")
        target.add_argument("--policy", default=str(DEFAULT_POLICY),
                            help="policy JSON (default: %(default)s)")
        target.add_argument("--json", default=None,
                            help="write the machine-readable report here "
                                 "('-' for stdout)")
        target.add_argument("--follow-symlinks", action="store_true",
                            help="also inspect symlinked ELFs")

    inventory = subparsers.add_parser(
        "inventory", help="emit the machine-readable ELF inventory")
    add_common(inventory)
    inventory.set_defaults(handler=command_inventory)

    audit = subparsers.add_parser(
        "audit", help="apply the public universal policy")
    add_common(audit)
    audit.add_argument("--profile", default="universal-low-glibc",
                       help="build profile (default: %(default)s)")
    audit.add_argument("--sdl-floor", default=None,
                       help="override the declared SDL floor for this run")
    audit.add_argument("--strict", action="store_true",
                       help="treat warnings as failures")
    audit.add_argument("--errors-only", action="store_true",
                       help="print errors only")
    audit.add_argument("--embed-inventory", action="store_true",
                       help="embed the full inventory in the JSON report")
    audit.add_argument("--verbose", action="store_true",
                       help="also print info findings")
    audit.add_argument("--quiet", action="store_true",
                       help="suppress the human-readable report")
    audit.set_defaults(handler=command_audit)

    sdl_table = subparsers.add_parser(
        "sdl-table", help="regenerate the SDL symbol floor table")
    sdl_table.add_argument("--headers", required=True,
                           help="directory holding the SDL2 headers")
    sdl_table.add_argument("--out", default=None, help="output TSV path")
    sdl_table.set_defaults(handler=command_sdl_table)

    toolchain = subparsers.add_parser(
        "toolchain", help="verify the pinned toolchains")
    toolchain.add_argument("--pin", default=str(DEFAULT_PIN),
                           help="toolchain pin JSON (default: %(default)s)")
    toolchain.add_argument("--json", default=None, help="write a JSON report")
    toolchain.add_argument("--quiet", action="store_true")
    toolchain.set_defaults(handler=command_toolchain)

    provenance = subparsers.add_parser(
        "provenance", help="emit the .note.nx.toolchain stamp for a build")
    provenance.add_argument("--toolchain", required=True,
                            help="toolchain key from the pin file")
    provenance.add_argument("--pin", default=str(DEFAULT_PIN))
    provenance.add_argument("--profile", default="universal-low-glibc")
    provenance.add_argument("--source-date-epoch", default=None)
    provenance.add_argument("--out", default=None)
    provenance.set_defaults(handler=command_provenance)
    return parser


def main(argv=None):
    parser = build_parser()
    arguments = parser.parse_args(argv)
    if not getattr(arguments, "handler", None):
        parser.print_help()
        return 2
    try:
        return arguments.handler(arguments)
    except AbiError as error:
        print("{}: {}".format(TOOL_NAME, error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
