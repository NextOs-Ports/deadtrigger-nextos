#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Process-free closure, evidence and documentation gate for M19."""

import ast
import json
from pathlib import Path, PurePosixPath
import re
import sys


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
CLOSURE = ROOT / "m19-closure-v1.json"
TOOL = ROOT / "nxgenerator.py"
SCHEMA = ROOT / "schema" / "nxproject-v1.schema.json"
TEMPLATE = ROOT / "templates" / "README.md.in"
README = ROOT / "README.md"
GENERATOR_TEST = ROOT / "tests" / "test_nxgenerator.py"
ITEM_IDS = tuple("M19-%03d" % index for index in range(1, 21))
REFERENCE_ROOTS = (
    "framework/",
    "suportando_outros_devices/",
    "publicando_ports/",
)
PRESENTATION_REFERENCE = "ports/gtasa/README.md"
EVIDENCE_RE = re.compile(r"^([^:]+):([1-9][0-9]*)$")
LINK_RE = re.compile(r"\[[^]]+\]\(([^)]+)\)")
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])"
)


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def read(path):
    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe file: %s" % path.relative_to(REPOSITORY))
    return path.read_text(encoding="utf-8")


def validate_evidence(reference):
    require(isinstance(reference, str), "evidence reference is not a string")
    matched = EVIDENCE_RE.fullmatch(reference)
    require(matched is not None, "evidence lacks an exact line: %s" % reference)
    relative, raw_line = matched.groups()
    require(relative == PRESENTATION_REFERENCE or
            relative.startswith(REFERENCE_ROOTS),
            "evidence escaped the approved M19 roots: %s" % relative)
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe evidence path: %s" % relative)
    path = REPOSITORY.joinpath(*logical.parts)
    text = read(path)
    line = int(raw_line)
    require(line <= len(text.splitlines()),
            "evidence line is outside the file: %s" % reference)


def validate_links(path):
    source = read(path)
    for target in LINK_RE.findall(source):
        if target.startswith(("#", "http://", "https://")):
            continue
        target = target.split("#", 1)[0]
        logical = PurePosixPath(target)
        require(not logical.is_absolute(),
                "unsafe documentation link in %s: %s" % (path.name, target))
        candidate = (path.parent / target).resolve()
        try:
            candidate.relative_to(REPOSITORY)
        except ValueError as error:
            raise GateError(
                "documentation link escaped the repository in %s: %s" %
                (path.name, target)
            ) from error
        require(candidate.exists(),
                "broken documentation link in %s: %s" % (path.name, target))


def main():
    document = json.loads(read(CLOSURE))
    require(set(document) == {
        "schema", "schema_version", "milestone", "status", "scope",
        "method", "safety", "items",
    }, "M19 closure schema changed")
    require(document["schema"] == "nxgenerator-m19-closure-v1" and
            document["schema_version"] == 1 and
            document["milestone"] == "M19" and
            document["status"] == "closed_for_generator_and_documentation",
            "M19 closure identity changed")
    require(document["safety"] == {
        "game_execution": False,
        "guest_code_execution": False,
        "device_access": False,
        "network_access": False,
        "physical_support_claimed": False,
        "approved_port_modified": False,
        "master_plan_modified": False,
    }, "M19 safety claims changed")

    items = document["items"]
    require(isinstance(items, list) and len(items) == 20,
            "M19 must account for exactly twenty items")
    require(tuple(item.get("id") for item in items) == ITEM_IDS,
            "M19 IDs or ordering changed")
    for item in items:
        require(set(item) == {
            "id", "status", "evidence_refs", "guarantees",
            "limitations", "tests",
        }, "%s has malformed fields" % item.get("id"))
        require(item["status"] == "closed",
                "%s is not closed" % item["id"])
        for field in ("evidence_refs", "guarantees", "limitations", "tests"):
            require(isinstance(item[field], list) and item[field],
                    "%s lacks %s" % (item["id"], field))
        for reference in item["evidence_refs"]:
            validate_evidence(reference)

    tool_source = read(TOOL)
    ast.parse(tool_source, filename=str(TOOL))
    json.loads(read(SCHEMA))
    template = read(TEMPLATE)
    test_source = read(GENERATOR_TEST)
    for token in (
        "BOOTSTRAP_GENERATOR.generate", "NXEXTRACT_FILES",
        "unimplemented_nonrelease", "rename_noreplace",
        "artifact_inventory", '"physical_support_proven": False',
    ):
        require(token in tool_source, "M19 generator lacks: %s" % token)
    for token in (
        "## English", "## Português", "### Architecture",
        "### Arquitetura", "### Controls", "### Controles",
        "### Game data", "### Dados do jogo", "### Licenses",
        "### Licenças", "same ZIP and SHA-256",
        "mesmo ZIP público exato", "development-only",
        "somente ao desenvolvimento",
    ):
        require(token in template, "M19 README template lacks: %s" % token)
    for token in (
        "two clean generations differ", "retired artifact",
        "golden-port guarantee", "does not record its generator version",
        "vendored NXExtract file differs", "adapter skeleton invented lifecycle",
        "generated PortMaster metadata changed", "validate_links(readme)",
    ):
        require(token in test_source, "M19 regression gate lacks: %s" % token)

    for path in (README, REPOSITORY / "framework" / "README.md",
                 REPOSITORY / "suportando_outros_devices" /
                 "padrao-universal.md",
                 REPOSITORY / "publicando_ports" / "README.md"):
        validate_links(path)
    for path in (README, TEMPLATE):
        text = read(path)
        require(not IPV4_RE.search(text) and "/home/" not in text and
                "/Users/" not in text,
                "M19 public documentation contains a private literal")

    print("M19 closure gate passed: items=20 status=closed "
          "abis=2 physical_support_claimed=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError, SyntaxError) as error:
        print("M19 closure gate failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
