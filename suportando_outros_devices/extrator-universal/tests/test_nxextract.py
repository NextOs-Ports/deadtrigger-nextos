#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import errno
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
NXEXTRACT = ROOT / "nxextract.py"


def load_module():
    spec = importlib.util.spec_from_file_location("nxextract_under_test", NXEXTRACT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NX = load_module()


class SimulatedPowerLoss(BaseException):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fake_elf(machine=183):
    data = bytearray(96)
    data[:4] = b"\x7fELF"
    data[4] = 1 if machine == 40 else 2
    data[5] = 1
    data[6] = 1
    struct.pack_into("<H", data, 16, 3)
    struct.pack_into("<H", data, 18, machine)
    return bytes(data) + b"NX-TEST-LIBRARY"


def plain_manifest(package, split=""):
    split_attribute = ' split="%s"' % split if split else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<manifest package="%s"%s></manifest>' % (package, split_attribute)
    ).encode("utf-8")


def _utf8_string(value):
    encoded = value.encode("utf-8")
    assert len(value) < 128 and len(encoded) < 128
    return bytes((len(value), len(encoded))) + encoded + b"\0"


def binary_manifest(package, split=""):
    strings = ["manifest", "package", "split", package, split]
    payload = b""
    offsets = []
    for value in strings:
        offsets.append(len(payload))
        payload += _utf8_string(value)
    header_size = 28
    strings_start = header_size + len(strings) * 4
    pool_size = strings_start + len(payload)
    pool = struct.pack(
        "<HHIIIIII",
        0x0001,
        header_size,
        pool_size,
        len(strings),
        0,
        0x100,
        strings_start,
        0,
    )
    pool += struct.pack("<%dI" % len(offsets), *offsets) + payload

    attributes = []
    for name_index, value_index in ((1, 3), (2, 4)):
        attributes.append(
            struct.pack(
                "<IIIHBBI",
                0xFFFFFFFF,
                name_index,
                value_index,
                8,
                0,
                0x03,
                value_index,
            )
        )
    node_size = 16 + 20 + sum(len(value) for value in attributes)
    node = struct.pack("<HHIII", 0x0102, 16, node_size, 1, 0xFFFFFFFF)
    node += struct.pack(
        "<IIHHHHHH", 0xFFFFFFFF, 0, 20, 20, len(attributes), 0, 0, 0
    )
    node += b"".join(attributes)
    total = 8 + len(pool) + len(node)
    return struct.pack("<HHI", 0x0003, 8, total) + pool + node


def make_zip(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def zip_bytes(entries):
    with tempfile.NamedTemporaryFile(suffix=".zip") as stream:
        make_zip(Path(stream.name), entries)
        stream.seek(0)
        return stream.read()


def base_recipe(lib_data, assets, extra_rules=None, extra_commit=None, hooks=None):
    asset_bytes = sum(len(value) for value in assets.values())
    rules = [
        {
            "id": "native",
            "source": {
                "kind": "entry",
                "patterns": ["lib/{abi}/libgame.so"],
            },
            "destination": "lib/{abi}/libgame.so",
            "validate": {
                "type": "file",
                "size": len(lib_data),
                "sha256": sha256(lib_data),
                "elf_machine": "arm64-v8a",
            },
        },
        {
            "id": "assets",
            "source": {
                "kind": "entries",
                "patterns": ["assets/**"],
                "strip_prefix": "assets/",
            },
            "destination": "assets",
            "validate": {
                "type": "tree",
                "exact_files": len(assets),
                "exact_bytes": asset_bytes,
                "required_paths": sorted(assets),
            },
        },
    ]
    if extra_rules:
        rules.extend(extra_rules)
    commit = ["lib/{abi}/libgame.so", "assets"]
    if extra_commit:
        commit.extend(extra_commit)
    return {
        "schema": 1,
        "id": "synthetic-port",
        "version": "test-1",
        "title": "SYNTHETIC PORT",
        "abi_order": ["arm64-v8a"],
        "input": {
            "search_dirs": ["gamedata", "."],
            "prefer_first_nonempty": True,
            "sniff_all_in_primary": True,
            "max_files": 64,
            "max_bundle_apks": 32,
            "max_member_bytes": 32 * 1024 * 1024,
            "max_bundle_bytes": 64 * 1024 * 1024,
        },
        "extract": rules,
        "validate": [
            {
                "path": "lib/{abi}/libgame.so",
                "type": "file",
                "sha256": sha256(lib_data),
                "elf_machine": "arm64-v8a",
            }
        ],
        "commit": commit,
        "hooks": hooks or [],
        "marker": ".synthetic-data.json",
        "space": {"safety_bytes": 0},
        "log": "test-extract.log",
        "ui_success_seconds": 0,
        "ui_error_seconds": 0,
    }


class NXExtractCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nxextract-test-")
        self.game = Path(self.temporary.name) / "port"
        self.data = self.game / "gamedata"
        self.data.mkdir(parents=True)
        self.recipe_path = self.game / "extractor.json"
        self.lib = fake_elf()
        self.assets = {
            "readme.dat": b"asset-one",
            "levels/level01.bin": b"level-data" * 7,
        }

    def tearDown(self):
        self.temporary.cleanup()

    def write_recipe(self, recipe=None):
        if recipe is None:
            recipe = base_recipe(self.lib, self.assets)
        self.recipe_path.write_text(
            json.dumps(recipe, sort_keys=True, indent=2), encoding="utf-8"
        )
        return recipe

    def run_cli(self, command="install", inputs=None, expect=0, extra=None):
        argv = [
            sys.executable,
            str(NXEXTRACT),
            command,
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
        ]
        if command in ("install", "plan"):
            argv.append("--quiet")
        if command == "install":
            argv += ["--ui", "none"]
        for value in inputs or []:
            argv += ["--input", str(value)]
        argv += extra or []
        result = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
        )
        if result.returncode != expect:
            self.fail(
                "command returned %d, expected %d\nstdout:\n%s\nstderr:\n%s"
                % (result.returncode, expect, result.stdout, result.stderr)
            )
        return result

    def assert_payload(self):
        self.assertEqual(
            (self.game / "lib/arm64-v8a/libgame.so").read_bytes(), self.lib
        )
        for relative, data in self.assets.items():
            self.assertEqual((self.game / "assets" / relative).read_bytes(), data)

    def merged_entries(self, manifest=None):
        entries = {
            "AndroidManifest.xml": manifest or plain_manifest("org.nextos.synthetic"),
            "lib/arm64-v8a/libgame.so": self.lib,
        }
        entries.update({"assets/" + key: value for key, value in self.assets.items()})
        return entries

    def test_manifest_parser_handles_binary_axml(self):
        package, split = NX.parse_android_manifest(
            binary_manifest("org.nextos.binary", "config.arm64_v8a")
        )
        self.assertEqual(package, "org.nextos.binary")
        self.assertEqual(split, "config.arm64_v8a")

    def test_renamed_merged_apk_is_selected_by_content(self):
        self.write_recipe()
        source = self.data / ("renamed-" + uuid.uuid4().hex)
        make_zip(source, self.merged_entries(binary_manifest("org.nextos.synthetic")))
        self.run_cli()
        self.assert_payload()
        self.assertTrue(source.exists(), "the legal source must be preserved")
        self.run_cli(command="verify")

    def test_templated_elf_machine_selects_and_validates_armv7(self):
        self.lib = fake_elf(machine=40)
        recipe = base_recipe(self.lib, self.assets)
        recipe["abi_order"] = ["arm64-v8a", "armeabi-v7a"]
        recipe["extract"][0]["validate"]["elf_machine"] = "{abi}"
        recipe["validate"][0]["elf_machine"] = "{abi}"
        self.write_recipe(recipe)
        entries = {
            "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
            "lib/armeabi-v7a/libgame.so": self.lib,
        }
        entries.update({"assets/" + key: value for key, value in self.assets.items()})
        source = self.data / ("abi-neutral-" + uuid.uuid4().hex + ".apk")
        make_zip(source, entries)

        self.run_cli()
        self.assertEqual(
            (self.game / "lib/armeabi-v7a/libgame.so").read_bytes(), self.lib
        )
        self.run_cli(command="verify")

    def test_loose_splits_are_grouped_by_manifest_package(self):
        self.write_recipe()
        base = self.data / ("one-" + uuid.uuid4().hex + ".apk")
        abi = self.data / ("two-" + uuid.uuid4().hex + ".apk")
        asset_entries = {
            "AndroidManifest.xml": binary_manifest("org.nextos.synthetic"),
        }
        asset_entries.update(
            {"assets/" + key: value for key, value in self.assets.items()}
        )
        make_zip(base, asset_entries)
        make_zip(
            abi,
            {
                "AndroidManifest.xml": binary_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertTrue(base.exists())
        self.assertTrue(abi.exists())

    def _bundle_case(self, extension):
        self.write_recipe()
        base_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("completely-random-" + uuid.uuid4().hex + extension)
        make_zip(
            bundle,
            {
                "unknown/base-random.apk": base_bytes,
                "splits/no-fixed-name.apk": split_bytes,
                "metadata/info.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertTrue(bundle.exists())

    def test_apkm_bundle(self):
        self._bundle_case(".apkm")

    def test_apks_bundle(self):
        self._bundle_case(".apks")

    def test_bundle_space_preflight_counts_only_valid_cached_apks(self):
        self.write_recipe()
        base_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / "cache-space.apkm"
        names = ["base.apk", "split.apk"]
        make_zip(bundle, {names[0]: base_bytes, names[1]: split_bytes})

        recipe = NX.Recipe(str(self.recipe_path))
        workspace = Path(NX.prepare_workspace(str(self.game), recipe.identifier))
        cache = (
            workspace
            / "source-cache"
            / ("bundle-" + NX._bundle_cache_token(str(bundle)))
        )
        cache.mkdir(parents=True)
        with zipfile.ZipFile(bundle, "r") as archive:
            members = [archive.getinfo(name) for name in names]
        destinations = []
        for index, info in enumerate(members):
            token = NX.sha256_bytes(info.filename.encode("utf-8"))[:12]
            destinations.append(cache / ("%03d-%s.apk" % (index, token)))
        destinations[0].write_bytes(base_bytes)
        destinations[1].write_bytes(b"X" * len(split_bytes))
        (cache / "stale-extra.apk").write_bytes(b"S" * (1024 * 1024))

        discovery = NX.discover_inputs(
            recipe,
            str(self.game),
            [str(bundle)],
            NX.Logger(None, verbose=False),
        )
        checks = []

        def capture_space(_path, required, label):
            checks.append((required, label))

        archives = []
        with mock.patch.object(NX, "_check_free_space", side_effect=capture_space):
            groups, archives = NX.build_candidate_groups(
                recipe,
                discovery,
                str(workspace),
                NX.Logger(None, verbose=False),
                NX.Progress(None),
            )
        try:
            self.assertTrue(groups)
            self.assertEqual(checks, [(len(split_bytes), "APK bundle expansion")])
        finally:
            for archive in archives:
                archive.close()

    def test_xapk_bundle_with_direct_obb(self):
        obb = b"LPK\0" + b"xapk-obb-payload" * 11
        obb_rule = {
            "id": "obb",
            "source": {
                "kind": "entry",
                "patterns": ["*.obb", "**/*.obb"],
                "scopes": ["bundle"],
            },
            "destination": "data/game.obb",
            "validate": {
                "type": "file",
                "size": len(obb),
                "sha256": sha256(obb),
                "magic_ascii": "LPK\u0000",
            },
        }
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[obb_rule],
            extra_commit=["data/game.obb"],
        )
        self.write_recipe(recipe)
        merged = zip_bytes(self.merged_entries())
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "install/" + uuid.uuid4().hex + ".apk": merged,
                "Android/obb/org.nextos.synthetic/main.payload.obb": obb,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertEqual((self.game / "data/game.obb").read_bytes(), obb)

    def container_rule(self, destination="data/game.apk", **source):
        rule = {
            "id": "container",
            "source": dict({"kind": "container"}, **source),
            "destination": destination,
            "validate": {"type": "file", "min_size": 64, "magic_ascii": "PK\u0003\u0004"},
        }
        return rule

    def test_container_copies_the_loose_apk_itself(self):
        """O payload de alguns jogos e' o APK inteiro, nao um subconjunto dele."""
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule()],
            extra_commit=["data/game.apk"],
        )
        self.write_recipe(recipe)
        source = self.data / ("app-" + uuid.uuid4().hex + ".apk")
        make_zip(source, self.merged_entries())
        self.run_cli()
        self.assert_payload()
        self.assertEqual(
            (self.game / "data/game.apk").read_bytes(), source.read_bytes()
        )

    def test_container_picks_the_base_apk_inside_a_bundle(self):
        """Num XAPK, o APK com os assets e' o BASE; o split nao entra por engano."""
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule()],
            extra_commit=["data/game.apk"],
        )
        self.write_recipe(recipe)
        base = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "org.nextos.synthetic.apk": base,
                "config.arm64_v8a.apk": split,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertEqual((self.game / "data/game.apk").read_bytes(), base)

    def test_container_can_ask_for_a_named_split(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule(split="config.arm64_v8a")],
            extra_commit=["data/game.apk"],
        )
        self.write_recipe(recipe)
        base = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "org.nextos.synthetic.apk": base,
                "config.arm64_v8a.apk": split,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assertEqual((self.game / "data/game.apk").read_bytes(), split)

    def test_recipe_refuses_another_application(self):
        """Dois jogos do mesmo estudio tem os mesmos nomes de asset.

        Sem fixar o pacote, a receita de um aceita o APK do outro e instala o
        jogo errado sem uma linha de reclamacao.
        """
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"] = dict(recipe.get("input", {}),
                               packages=["org.nextos.expected"])
        self.write_recipe(recipe)
        source = self.data / ("other-" + uuid.uuid4().hex + ".apk")
        make_zip(source, self.merged_entries())
        self.run_cli(expect=1)
        report = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("org.nextos.synthetic", report)
        self.assertIn("org.nextos.expected", report)
        self.assertFalse((self.game / "assets").exists())

    def test_recipe_accepts_the_declared_application(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"] = dict(recipe.get("input", {}),
                               packages=["org.nextos.synthetic"])
        self.write_recipe(recipe)
        source = self.data / ("app-" + uuid.uuid4().hex + ".apk")
        make_zip(source, self.merged_entries())
        self.run_cli()
        self.assert_payload()

    def test_invalid_input_contract_fails_before_log_or_workspace(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"]["max_files"] = True
        self.write_recipe(recipe)

        self.run_cli(expect=1)

        self.assertFalse((self.game / "test-extract.log").exists())
        self.assertFalse((self.game / ".nxextract").exists())

    def test_invalid_recipe_templates_and_reserved_paths_fail_before_effects(self):
        cases = []

        unknown_template = base_recipe(self.lib, self.assets)
        unknown_template["hooks"] = [
            {"id": "bad-template", "argv": ["{unknown_field}"]}
        ]
        cases.append(("unknown-template", unknown_template))

        workspace_commit = base_recipe(self.lib, self.assets)
        workspace_commit["commit"] = [".nxextract", "assets"]
        cases.append(("workspace-commit", workspace_commit))

        marker_log_collision = base_recipe(self.lib, self.assets)
        marker_log_collision["marker"] = marker_log_collision["log"]
        cases.append(("marker-log-collision", marker_log_collision))

        invalid_fingerprint = base_recipe(self.lib, self.assets)
        invalid_fingerprint["extract"][1]["validate"]["tree_fingerprint"] = "bad"
        cases.append(("invalid-fingerprint", invalid_fingerprint))

        for label, recipe in cases:
            with self.subTest(label=label):
                self.write_recipe(recipe)
                self.run_cli(expect=1)
                self.assertFalse((self.game / "test-extract.log").exists())
                self.assertFalse((self.game / ".nxextract").exists())

    def test_loose_obb_is_chosen_by_hash_not_filename(self):
        obb = b"LPK\0" + os.urandom(128)
        obb_rule = {
            "id": "obb",
            "source": {
                "kind": "entry_or_file",
                "patterns": ["*"],
                "file_extensions": [".obb"],
            },
            "destination": "data/game.obb",
            "validate": {
                "size": len(obb),
                "sha256": sha256(obb),
                "magic_ascii": "LPK\u0000",
            },
        }
        self.write_recipe(
            base_recipe(
                self.lib,
                self.assets,
                extra_rules=[obb_rule],
                extra_commit=["data/game.obb"],
            )
        )
        make_zip(self.data / "base.apk", self.merged_entries())
        loose = self.data / (uuid.uuid4().hex + ".obb")
        loose.write_bytes(obb)
        self.run_cli()
        self.assertEqual((self.game / "data/game.obb").read_bytes(), obb)
        self.assertTrue(loose.exists())

    def test_second_run_uses_marker_without_source(self):
        self.write_recipe()
        source = self.data / "first.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        parked = Path(self.temporary.name) / "parked-source"
        source.rename(parked)
        self.run_cli()
        self.assert_payload()

    def test_fast_marker_requires_schema_and_payload_metadata_seal(self):
        self.write_recipe()
        source = self.data / "sealed.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        recipe = NX.Recipe(str(self.recipe_path))
        marker_path = self.game / ".synthetic-data.json"
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
        logger = NX.Logger(None, verbose=False)

        self.assertTrue(NX.marker_matches_recipe(marker, recipe))
        self.assertIsNotNone(
            NX.marker_fast_valid(
                str(marker_path), recipe, str(self.game), logger
            )
        )

        payload = self.game / "assets/readme.dat"
        original = payload.read_bytes()
        payload.write_bytes(b"X" * len(original))
        self.assertIsNone(
            NX.marker_fast_valid(
                str(marker_path), recipe, str(self.game), logger
            )
        )

        payload.write_bytes(original)
        marker["commit"] = marker["commit"] + ["unexpected"]
        NX.atomic_write_json(marker_path, marker)
        self.assertFalse(NX.marker_matches_recipe(marker, recipe))
        self.assertIsNone(
            NX.marker_fast_valid(
                str(marker_path), recipe, str(self.game), logger
            )
        )

    def test_force_source_reinstalls_valid_payload_transactionally(self):
        self.write_recipe()
        source = self.data / "first.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        marker_path = self.game / ".synthetic-data.json"
        first_marker = json.loads(marker_path.read_text(encoding="utf-8"))

        self.run_cli(extra=["--force-source"])

        second_marker = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertNotEqual(
            first_marker["transaction_id"],
            second_marker["transaction_id"],
        )
        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "force-source requested; bypassing the installed marker",
            log,
        )

    @unittest.skipIf(os.geteuid() == 0, "root ignores the read-only directory")
    def test_undeletable_source_cache_does_not_fail_a_committed_install(self):
        # A FUSE-backed share (exFAT on Knulli, NFS, SMB) can refuse to drop the
        # scratch cache. The payload is already committed at that point, so the
        # run must still succeed. A read-only directory reproduces the refusal.
        self.write_recipe()
        make_zip(self.data / "merged.apk", self.merged_entries())
        cache = self.game / ".nxextract/synthetic-port/source-cache"
        trap = cache / "undeletable"
        trap.mkdir(parents=True)
        (trap / "pinned").write_bytes(b"pinned")
        trap.chmod(0o500)
        try:
            self.run_cli()
            self.assert_payload()
            log = (self.game / "test-extract.log").read_text(encoding="utf-8")
            self.assertIn("warning: kept source cache for the next run", log)
        finally:
            trap.chmod(0o700)

    def _assert_published_cleanup_failure_is_nonfatal(self, cleanup_name):
        self.write_recipe()
        make_zip(self.data / "cleanup.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        marker = self.game / ".synthetic-data.json"
        target = workspace / cleanup_name
        real_remove_path = NX.remove_path
        real_rmtree = NX.shutil.rmtree
        real_unlink = NX.os.unlink
        target_path = os.path.abspath(str(target))

        def is_published_target(path):
            return marker.exists() and os.path.abspath(os.fspath(path)) == target_path

        def refuse_remove(path):
            if is_published_target(path):
                raise OSError(errno.ENOTEMPTY, "synthetic cleanup refusal", str(path))
            return real_remove_path(path)

        def refuse_rmtree(path, *args, **kwargs):
            if is_published_target(path):
                # Simulate a FUSE/SMB path that remains present after the
                # best-effort retry. The retained journal must trigger a later
                # recovery attempt.
                return None
            return real_rmtree(path, *args, **kwargs)

        def refuse_unlink(path, *args, **kwargs):
            if is_published_target(path):
                raise OSError(errno.EBUSY, "synthetic cleanup refusal", str(path))
            return real_unlink(path, *args, **kwargs)

        argv = [
            "install",
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
            "--quiet",
            "--ui",
            "none",
        ]
        with mock.patch.object(NX, "remove_path", side_effect=refuse_remove), \
                mock.patch.object(NX.shutil, "rmtree", side_effect=refuse_rmtree), \
                mock.patch.object(NX.os, "unlink", side_effect=refuse_unlink):
            status = NX.main(argv)

        self.assertEqual(status, 0)
        self.assert_payload()
        self.assertTrue(marker.exists())
        self.assertTrue(target.exists())
        self.assertTrue((workspace / "transaction.json").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("published payload remains valid", log)

        # Once the filesystem accepts removal again, normal recovery consumes
        # the retained journal before accepting the fast marker.
        self.run_cli()
        self.assertFalse(target.exists())
        self.assertFalse((workspace / "transaction.json").exists())

    def test_backup_cleanup_failure_after_marker_is_nonfatal(self):
        self._assert_published_cleanup_failure_is_nonfatal("backup")

    def test_stage_cleanup_failure_after_marker_is_nonfatal(self):
        self._assert_published_cleanup_failure_is_nonfatal("stage")

    def test_journal_cleanup_failure_after_marker_is_nonfatal(self):
        self._assert_published_cleanup_failure_is_nonfatal("transaction.json")

    def test_existing_data_rejection_logs_validation_reason(self):
        self.write_recipe()
        installed = self.game / "lib/arm64-v8a/libgame.so"
        installed.parent.mkdir(parents=True)
        installed.write_bytes(self.lib + b"-changed")
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "existing data not adoptable for ABI arm64-v8a:",
            log,
        )
        self.assertIn(
            "native (lib/arm64-v8a/libgame.so) has unexpected size",
            log,
        )

    def test_rejected_candidates_are_reported_as_different_build(self):
        self.write_recipe()
        entries = self.merged_entries()
        entries["lib/arm64-v8a/libgame.so"] = self.lib + b"-other-build"
        make_zip(self.data / "other-build.apk", entries)
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "required payload native was not found: 1 candidate(s) matched "
            "the source pattern but failed validation",
            log,
        )
        self.assertIn("probably a different build of the game", log)
        self.assertIn("lib/arm64-v8a/libgame.so", log)

    def test_valid_staged_file_is_resumed_without_rewrite(self):
        self.write_recipe()
        source = self.data / "resume.apk"
        make_zip(source, self.merged_entries())
        staged = (
            self.game
            / ".nxextract/synthetic-port/stage/lib/arm64-v8a/libgame.so"
        )
        staged.parent.mkdir(parents=True)
        staged.write_bytes(self.lib)
        old_time = time.time() - 3600
        os.utime(staged, (old_time, old_time))
        self.run_cli()
        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("resuming", log)

    def test_stage_failing_whole_set_validation_is_discarded(self):
        # Cada item extraído passa sozinho; o CONJUNTO reprova porque a receita
        # exige um payload companheiro que nenhuma regra produz. Antes da 1.2.2
        # o stage ruim ficava no disco e o resume o revalidava e reprovava para
        # sempre. Agora ele é descartado e a execução seguinte extrai do zero.
        recipe = base_recipe(self.lib, self.assets)
        recipe["validate"].append(
            {"path": "assets/companion.dat", "type": "file", "min_size": 1}
        )
        self.write_recipe(recipe)
        source = self.data / "whole-set.apk"
        make_zip(source, self.merged_entries())
        stage = self.game / ".nxextract/synthetic-port/stage"

        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("discarding staged data that failed validation", log)
        self.assertFalse(stage.exists())
        self.assertFalse(
            (self.game / ".nxextract/synthetic-port/state.json").exists()
        )
        # A fonte legal do usuário nunca é apagada, e nada foi publicado.
        self.assertTrue(source.exists())
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())

        # Segunda tentativa: reprova de novo (a receita continua exigindo o
        # companheiro), mas EXTRAINDO do zero — sem retomar o stage reprovado.
        (self.game / "test-extract.log").unlink()
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertNotIn("resuming", log)

    def test_failed_hook_preserves_previous_live_payload_and_source(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[{"id": "intentional-failure", "argv": ["/bin/false"]}],
        )
        self.write_recipe(recipe)
        old = self.game / "assets/readme.dat"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"old-live-data")
        source = self.data / "hook.apk"
        make_zip(source, self.merged_entries())
        self.run_cli(expect=1)
        self.assertEqual(old.read_bytes(), b"old-live-data")
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())
        self.assertTrue(source.exists())
        self.assertTrue(
            (
                self.game
                / ".nxextract/synthetic-port/stage/lib/arm64-v8a/libgame.so"
            ).exists()
        )

    def test_hook_checkpoint_expands_selected_abi(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[
                {
                    "id": "abi-checkpoint",
                    "argv": ["/bin/true"],
                    "checkpoint": [
                        {
                            "path": "lib/{abi}/libgame.so",
                            "type": "file",
                            "sha256": sha256(self.lib),
                            "elf_machine": "arm64-v8a",
                        }
                    ],
                }
            ],
        )
        self.write_recipe(recipe)
        make_zip(self.data / "checkpoint.apk", self.merged_entries())
        self.run_cli()
        self.assert_payload()

    def test_recovery_rolls_back_an_interrupted_publish(self):
        self.write_recipe()
        recipe = NX.Recipe(str(self.recipe_path))
        workspace = self.game / ".nxextract/synthetic-port"
        live = self.game / "assets/readme.dat"
        backup = workspace / "backup/assets/readme.dat"
        cache = workspace / "source-cache/bundle/base.apk"
        live.parent.mkdir(parents=True)
        backup.parent.mkdir(parents=True)
        cache.parent.mkdir(parents=True)
        live.write_bytes(b"new-unpublished-data")
        backup.write_bytes(b"old-live-data")
        cache.write_bytes(b"cached-source")
        NX.atomic_write_json(
            workspace / "transaction.json",
            {
                "format": NX.FORMAT_VERSION,
                "transaction_format": NX.TRANSACTION_FORMAT_VERSION,
                "transaction_id": "1" * 32,
                "recipe_id": recipe.identifier,
                "recipe_digest": recipe.digest,
                "abi": "arm64-v8a",
                "published": False,
                "paths": [
                    {
                        "path": "lib/arm64-v8a/libgame.so",
                        "had_live": False,
                        "phase": "pending",
                    },
                    {
                        "path": "assets",
                        "had_live": True,
                        "phase": "installed",
                    }
                ],
            },
        )

        logger = NX.Logger(None, verbose=False)
        NX.recover_transaction(
            recipe,
            str(self.game),
            str(workspace),
            str(self.game / ".synthetic-data.json"),
            logger,
        )

        self.assertEqual(live.read_bytes(), b"old-live-data")
        self.assertEqual(
            (workspace / "stage/assets/readme.dat").read_bytes(),
            b"new-unpublished-data",
        )
        self.assertEqual(cache.read_bytes(), b"cached-source")
        self.assertFalse((workspace / "backup").exists())
        self.assertFalse((workspace / "transaction.json").exists())

    def test_recovery_finishes_a_marker_published_transaction(self):
        self.write_recipe()
        make_zip(self.data / "published.apk", self.merged_entries())
        self.run_cli()
        recipe = NX.Recipe(str(self.recipe_path))
        workspace = self.game / ".nxextract/synthetic-port"
        marker = self.game / ".synthetic-data.json"
        marker_data = json.loads(marker.read_text(encoding="utf-8"))
        for relative in ("stage/payload", "backup/payload", "source-cache/base.apk"):
            path = workspace / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"temporary")
        transaction_id = marker_data["transaction_id"]
        NX.atomic_write_json(
            workspace / "transaction.json",
            {
                "format": NX.FORMAT_VERSION,
                "transaction_format": NX.TRANSACTION_FORMAT_VERSION,
                "transaction_id": transaction_id,
                "recipe_id": recipe.identifier,
                "recipe_digest": recipe.digest,
                "abi": "arm64-v8a",
                "published": False,
                "paths": [
                    {
                        "path": "lib/arm64-v8a/libgame.so",
                        "had_live": False,
                        "phase": "installed",
                    },
                    {
                        "path": "assets",
                        "had_live": False,
                        "phase": "installed",
                    },
                ],
            },
        )

        logger = NX.Logger(None, verbose=False)
        NX.recover_transaction(
            recipe, str(self.game), str(workspace), str(marker), logger
        )

        self.assertFalse((workspace / "stage").exists())
        self.assertFalse((workspace / "backup").exists())
        self.assertFalse((workspace / "source-cache").exists())
        self.assertFalse((workspace / "transaction.json").exists())
        self.assertTrue(marker.exists())

    def test_every_transaction_boundary_recovers_after_power_loss(self):
        transitions = [
            "journal-created",
            "backup-skipped-0",
            "install-intent-0",
            "install-renamed-0",
            "install-recorded-0",
            "backup-intent-1",
            "backup-renamed-1",
            "backup-recorded-1",
            "install-intent-1",
            "install-renamed-1",
            "install-recorded-1",
            "payload-validated",
            "marker-published",
            "journal-published",
        ]
        published = {"marker-published", "journal-published"}
        for number, transition in enumerate(transitions):
            with self.subTest(transition=transition):
                game = Path(self.temporary.name) / ("power-loss-%02d" % number)
                data = game / "gamedata"
                data.mkdir(parents=True)
                recipe_path = game / "extractor.json"
                recipe_path.write_text(
                    json.dumps(
                        base_recipe(self.lib, self.assets),
                        sort_keys=True,
                        indent=2,
                    ),
                    encoding="utf-8",
                )
                source = data / "payload.apk"
                make_zip(source, self.merged_entries())

                # Path 0 (lib) has no previous live object; path 1 (assets)
                # does. Together they exercise backup-skipped and backed-up.
                old_assets = game / "assets"
                old_assets.mkdir()
                (old_assets / "legacy.dat").write_bytes(b"old-live-payload")

                observed = []

                def interrupt(name, _journal):
                    observed.append(name)
                    if name == transition:
                        raise SimulatedPowerLoss(transition)

                argv = [
                    "install",
                    "--recipe",
                    str(recipe_path),
                    "--game-dir",
                    str(game),
                    "--quiet",
                    "--ui",
                    "none",
                    "--force-source",
                ]
                with mock.patch.object(
                    NX, "_transaction_transition", side_effect=interrupt
                ):
                    with self.assertRaises(SimulatedPowerLoss):
                        NX.main(argv)
                self.assertIn(transition, observed)

                recipe = NX.Recipe(str(recipe_path))
                workspace = game / ".nxextract/synthetic-port"
                logger = NX.Logger(None, verbose=False)
                NX.recover_transaction(
                    recipe,
                    str(game),
                    str(workspace),
                    str(game / ".synthetic-data.json"),
                    logger,
                )

                self.assertFalse((workspace / "transaction.json").exists())
                self.assertFalse((workspace / "backup").exists())
                if transition in published:
                    self.assertEqual(
                        (game / "lib/arm64-v8a/libgame.so").read_bytes(),
                        self.lib,
                    )
                    for relative, payload in self.assets.items():
                        self.assertEqual(
                            (game / "assets" / relative).read_bytes(),
                            payload,
                        )
                    self.assertFalse((game / "assets/legacy.dat").exists())
                    marker = json.loads(
                        (game / ".synthetic-data.json").read_text(encoding="utf-8")
                    )
                    self.assertTrue(NX.marker_matches_recipe(marker, recipe))
                else:
                    self.assertFalse(
                        (game / "lib/arm64-v8a/libgame.so").exists()
                    )
                    self.assertEqual(
                        (game / "assets/legacy.dat").read_bytes(),
                        b"old-live-payload",
                    )
                    self.assertFalse((game / "assets/readme.dat").exists())
                    self.assertFalse((game / ".synthetic-data.json").exists())

    def test_recovery_rolls_back_when_matching_marker_payload_is_corrupt(self):
        self.write_recipe()
        make_zip(self.data / "corrupt-after-marker.apk", self.merged_entries())
        old_assets = self.game / "assets"
        old_assets.mkdir()
        (old_assets / "legacy.dat").write_bytes(b"old-live-payload")

        def interrupt(name, _journal):
            if name == "marker-published":
                raise SimulatedPowerLoss(name)

        argv = [
            "install",
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
            "--quiet",
            "--ui",
            "none",
            "--force-source",
        ]
        with mock.patch.object(
            NX, "_transaction_transition", side_effect=interrupt
        ):
            with self.assertRaises(SimulatedPowerLoss):
                NX.main(argv)

        live_library = self.game / "lib/arm64-v8a/libgame.so"
        damaged = bytearray(live_library.read_bytes())
        damaged[-1] ^= 0xFF
        live_library.write_bytes(damaged)

        recipe = NX.Recipe(str(self.recipe_path))
        workspace = self.game / ".nxextract/synthetic-port"
        NX.recover_transaction(
            recipe,
            str(self.game),
            str(workspace),
            str(self.game / ".synthetic-data.json"),
            NX.Logger(None, verbose=False),
        )

        self.assertFalse(live_library.exists())
        self.assertEqual(
            (self.game / "assets/legacy.dat").read_bytes(),
            b"old-live-payload",
        )
        self.assertFalse((workspace / "transaction.json").exists())

    def test_zip_slip_is_rejected_before_writing_payload(self):
        self.write_recipe()
        entries = self.merged_entries()
        entries["assets/../../escaped"] = b"bad"
        make_zip(self.data / "unsafe.apk", entries)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "escaped").exists())
        self.assertFalse((Path(self.temporary.name) / "escaped").exists())

    def test_casefold_destination_collision_is_rejected(self):
        collision_assets = {
            "Foo.bin": b"first",
            "foo.bin": b"second",
        }
        self.assets = collision_assets
        self.write_recipe(base_recipe(self.lib, collision_assets))
        make_zip(self.data / "collision.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

    def test_workspace_symlink_is_rejected(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-workspace"
        outside.mkdir()
        (self.game / ".nxextract").symlink_to(outside, target_is_directory=True)
        make_zip(self.data / "payload.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_log_symlink_and_hardlink_never_touch_their_target(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-log"
        outside.write_bytes(b"sentinel-log")
        log = self.game / "test-extract.log"

        log.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-log")
        log.unlink()

        os.link(outside, log)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-log")

    def test_lock_symlink_and_hardlink_never_touch_their_target(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        lock = workspace / "install.lock"
        outside = Path(self.temporary.name) / "outside-lock"
        outside.write_bytes(b"sentinel-lock")

        lock.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-lock")
        lock.unlink()

        os.link(outside, lock)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-lock")

    def test_internal_source_cache_symlink_is_rejected(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-cache"
        outside.mkdir()
        (workspace / "source-cache").symlink_to(
            outside, target_is_directory=True
        )

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_internal_stage_symlink_is_rejected(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-stage"
        outside.mkdir()
        (workspace / "stage").symlink_to(outside, target_is_directory=True)

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_nested_stage_symlink_is_rejected_before_preflight_or_copy(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        stage = workspace / "stage"
        stage.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-nested-stage"
        outside.mkdir()
        (stage / "lib").symlink_to(outside, target_is_directory=True)

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_hardlinked_existing_payload_is_not_adopted(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-hardlink-payload"
        outside.write_bytes(self.lib)
        destination = self.game / "lib/arm64-v8a/libgame.so"
        destination.parent.mkdir(parents=True)
        os.link(outside, destination)

        self.run_cli(expect=1)

        self.assertEqual(outside.read_bytes(), self.lib)
        self.assertFalse((self.game / ".synthetic-data.json").exists())

    def test_internal_hook_checkpoint_symlink_is_rejected(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[{"id": "safe-noop", "argv": ["/bin/true"]}],
        )
        self.write_recipe(recipe)
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-hooks"
        outside.mkdir()
        (workspace / "hooks").symlink_to(outside, target_is_directory=True)

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_progress_file_must_remain_inside_private_workspace(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-progress"
        outside.write_bytes(b"sentinel-progress")

        self.run_cli(
            expect=1,
            extra=["--progress-file", str(outside)],
        )

        self.assertEqual(outside.read_bytes(), b"sentinel-progress")

    def test_recipe_and_explicit_input_symlinks_are_rejected(self):
        outside_recipe = Path(self.temporary.name) / "outside-recipe.json"
        outside_recipe.write_text(
            json.dumps(base_recipe(self.lib, self.assets)),
            encoding="utf-8",
        )
        self.recipe_path.symlink_to(outside_recipe)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

        self.recipe_path.unlink()
        os.link(outside_recipe, self.recipe_path)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

        self.recipe_path.unlink()
        self.write_recipe()
        outside_apk = Path(self.temporary.name) / "outside.apk"
        make_zip(outside_apk, self.merged_entries())
        linked_apk = self.data / "linked.apk"
        linked_apk.symlink_to(outside_apk)
        self.run_cli(inputs=[linked_apk], expect=1)
        self.assertTrue(outside_apk.exists())
        self.assertFalse((self.game / "assets").exists())

    def test_recipe_inside_symlinked_parent_is_rejected_before_reading_it(self):
        outside = Path(self.temporary.name) / "outside-recipe-parent"
        outside.mkdir()
        (outside / "extractor.json").write_text("not-json\n", encoding="utf-8")
        (self.game / "linked-config").symlink_to(
            outside, target_is_directory=True
        )
        self.recipe_path = self.game / "linked-config/extractor.json"

        result = self.run_cli(expect=1)

        self.assertIn("unsafe non-directory parent", result.stderr)
        self.assertFalse((self.game / "test-extract.log").exists())
        self.assertFalse((self.game / ".nxextract").exists())

    def test_linked_transaction_journal_is_rejected_without_reading_target(self):
        self.write_recipe()
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        journal = workspace / "transaction.json"
        outside = Path(self.temporary.name) / "outside-journal"
        outside.write_text('{"hostile":true}\n', encoding="utf-8")

        journal.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(
            outside.read_text(encoding="utf-8"),
            '{"hostile":true}\n',
        )
        journal.unlink()

        os.link(outside, journal)
        self.run_cli(expect=1)
        self.assertEqual(
            outside.read_text(encoding="utf-8"),
            '{"hostile":true}\n',
        )

    def test_zip_symbolic_link_member_is_rejected(self):
        assets = dict(self.assets, link=b"outside")
        self.write_recipe(base_recipe(self.lib, assets))
        source = self.data / "symlink-member.apk"
        source.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(
            source, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for name, payload in self.merged_entries().items():
                archive.writestr(name, payload)
            link = zipfile.ZipInfo("assets/link")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(link, b"outside")

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_unselected_special_zip_member_is_rejected(self):
        self.write_recipe()
        source = self.data / "unselected-special.apk"
        with zipfile.ZipFile(
            source, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for name, payload in self.merged_entries().items():
                archive.writestr(name, payload)
            link = zipfile.ZipInfo("not-selected-by-recipe")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(link, b"outside")

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_unselected_zip_case_collision_is_accepted(self):
        """A collision outside the selected members must not reject the APK.

        Real APKs carry obfuscated resources differing only in case; refusing
        the archive rejected legitimate builds whose colliding members are
        never written to the card.
        """
        self.write_recipe()
        entries = self.merged_entries()
        entries["Assets/unselected.bin"] = b"collision"
        make_zip(self.data / "directory-collision.apk", entries)

        self.run_cli()

        self.assertTrue((self.game / "assets").is_dir())

    def test_selected_zip_case_collision_is_rejected(self):
        """Two SELECTED members collapsing to one portable path still fail."""
        self.write_recipe()
        entries = self.merged_entries()
        entries["assets/Collide.bin"] = b"one"
        entries["assets/collide.bin"] = b"two"
        make_zip(self.data / "selected-collision.apk", entries)

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_unicode_normalization_destination_collision_is_rejected(self):
        collision_assets = {
            "\u00e9.bin": b"first",
            "e\u0301.bin": b"second",
        }
        self.assets = collision_assets
        self.write_recipe(base_recipe(self.lib, collision_assets))
        make_zip(self.data / "unicode-collision.apk", self.merged_entries())

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_recipe_rejects_log_path_escape(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["log"] = "../escaped.log"
        self.write_recipe(recipe)
        make_zip(self.data / "payload.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertFalse((Path(self.temporary.name) / "escaped.log").exists())

    def test_different_packages_are_never_merged(self):
        self.write_recipe()
        base_entries = {
            "AndroidManifest.xml": plain_manifest("org.nextos.one"),
            **{"assets/" + key: value for key, value in self.assets.items()},
        }
        make_zip(self.data / "one.apk", base_entries)
        make_zip(
            self.data / "two.apk",
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.two", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            },
        )
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

    def test_two_different_matching_bundles_are_ambiguous(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["extract"][0]["validate"].pop("sha256")
        recipe["extract"][0]["validate"].pop("size")
        recipe["validate"] = []
        self.write_recipe(recipe)
        bundles = []
        for number in (1, 2):
            changed_lib = self.lib + bytes((number,))
            inner = zip_bytes(
                {
                    "AndroidManifest.xml": plain_manifest(
                        "org.nextos.synthetic%d" % number
                    ),
                    "lib/arm64-v8a/libgame.so": changed_lib,
                    **{
                        "assets/" + key: value
                        for key, value in self.assets.items()
                    },
                }
            )
            bundle = self.data / ("bundle%d.apkm" % number)
            bundles.append(bundle)
            make_zip(
                bundle,
                {"payload%d.apk" % number: inner},
            )
        self.run_cli(expect=1)

        # An explicit legal input resolves the ambiguity without relying on
        # external filenames or deleting either user package.
        self.run_cli(inputs=[bundles[0]])
        self.assertEqual(
            (self.game / "lib/arm64-v8a/libgame.so").read_bytes(),
            self.lib + b"\x01",
        )
        self.assertTrue(all(path.exists() for path in bundles))

    def test_progress_protocol_is_atomic_and_parseable(self):
        target = Path(self.temporary.name) / "progress.txt"
        result = subprocess.run(
            [
                sys.executable,
                str(NXEXTRACT),
                "progress",
                "--file",
                str(target),
                "--phase",
                "5",
                "--overall",
                "700",
                "--phase-progress",
                "321",
                "--done-bytes",
                "123",
                "--total-bytes",
                "456",
                "--message",
                "BAKING TEXTURES",
                "--detail",
                "texture 7 / 20",
            ],
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        lines = target.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "1 700 1000")
        self.assertEqual(lines[1], "BAKING TEXTURES")
        self.assertEqual(lines[2], "NXEXTRACT_V1 5 700 321 123 456")
        self.assertEqual(lines[3], "texture 7 / 20")


if __name__ == "__main__":
    unittest.main(verbosity=2)
