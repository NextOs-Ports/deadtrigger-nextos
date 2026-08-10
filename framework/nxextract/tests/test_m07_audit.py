#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Static completeness gate for the NXExtract 1.2.6 / M07 evidence."""

import hashlib
import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
AUDIT_PATH = REPO_ROOT / "framework/nxextract/m07-audit-v1.json"
NX_ROOT = REPO_ROOT / "suportando_outros_devices/extrator-universal"
PIN_GATE = REPO_ROOT / "suportando_outros_devices/tools/check-nxextract-pin.sh"
EXPECTED_UI_SHA256 = (
    "046afb583f5a211c946495e639409f81d9cfec706788eeccb7924b0e8e5a50b6"
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path):
    def no_duplicates(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, "duplicate JSON key %s in %s" % (key, path))
            value[key] = item
        return value

    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicates)


def main():
    audit = load_json(AUDIT_PATH)
    require(audit.get("schema_version") == 1 and audit.get("milestone") == "M07",
            "M07 audit header changed")
    require(audit.get("scope") ==
            "local-filesystem-runtime-and-historical-byte-identical-ui",
            "M07 audit scope changed")
    require(audit.get("release_device_evidence") is False,
            "M07 audit incorrectly claims a physical 1.2.6 device result")
    require(set(audit) == {"schema_version", "milestone", "scope",
                           "release_device_evidence", "requirements"},
            "M07 audit has an unknown top-level field")

    requirements = audit.get("requirements")
    expected_ids = ["M07-%03d" % number for number in range(1, 21)]
    require(isinstance(requirements, list) and
            [item.get("id") for item in requirements] == expected_ids,
            "M07 audit IDs are incomplete or reordered")
    for item in requirements:
        require(set(item) == {"id", "implementation", "tests"},
                "%s has an unknown field" % item.get("id"))
        for group in ("implementation", "tests"):
            references = item.get(group)
            require(isinstance(references, list) and references,
                    "%s lacks %s evidence" % (item["id"], group))
            for reference in references:
                require(set(reference) == {"path", "token"},
                        "%s has malformed evidence" % item["id"])
                relative = Path(reference.get("path", ""))
                require(not relative.is_absolute() and ".." not in relative.parts,
                        "%s evidence escapes the repository" % item["id"])
                evidence = REPO_ROOT / relative
                require(evidence.is_file() and not evidence.is_symlink(),
                        "%s evidence is missing/linked: %s" %
                        (item["id"], relative))
                token = reference.get("token")
                require(isinstance(token, str) and token and
                        token in evidence.read_text(encoding="utf-8"),
                        "%s token is absent from %s: %r" %
                        (item["id"], relative, token))

    audit_text = AUDIT_PATH.read_text(encoding="utf-8")
    require(re.search(r"(?:^|[^0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?:[^0-9]|$)",
                      audit_text) is None,
            "M07 audit contains a device/test IP")
    require((NX_ROOT / "VERSION").read_text(encoding="utf-8").strip() == "1.2.6",
            "NXExtract VERSION is not exactly 1.2.6")
    engine = (NX_ROOT / "nxextract.py").read_text(encoding="utf-8")
    require('NXEXTRACT_VERSION = "1.2.6"' in engine,
            "NXExtract engine version is not exactly 1.2.6")
    require(PIN_GATE.stat().st_mode & 0o111,
            "whole-bundle pin gate is not executable")

    suite = unittest.defaultTestLoader.discover(
        str(NX_ROOT / "tests"), pattern="test_nxextract.py"
    )
    require(suite.countTestCases() == 56,
            "M07 Python regression count changed without an audit update")
    ui = NX_ROOT / "ui/build/nxextract-ui"
    require(ui.is_file() and not ui.is_symlink(), "canonical UI ELF is missing/linked")
    require(hashlib.sha256(ui.read_bytes()).hexdigest() == EXPECTED_UI_SHA256,
            "canonical UI no longer matches the physically evidenced binary")

    print("M07 audit gate passed: 20 requirements, 56 Python cases, "
          "release_device_evidence=0")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("M07 audit gate failed: %s" % error)
        raise SystemExit(1)
