"""Exercise actual offline bundle creation/failure paths without an SDK build."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("dot_build", Path(__file__).resolve().parent.parent / "tools/build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="esp32-dot-build-test-")
        self.root = Path(self.temporary.name)
        self.project = self.root / "project"
        self.project.mkdir()
        (self.project / "CMakeLists.txt").write_text("# fixture project\n")
        (self.project / "sdkconfig.defaults").write_text("# fixture defaults\n")
        (self.project / "sdkconfig").write_text('CONFIG_IDF_TARGET="esp32s3"\n')
        (self.project / "partitions.csv").write_text("# test fixture metadata\n")
        self.work = self.root / "work"
        self.work.mkdir()
        self.firmware = self.project / "firmware"
        self.project_patch = patch.object(build, "PROJECT", self.project)
        self.project_patch.start()
        self.addCleanup(self.project_patch.stop)
        self.addCleanup(self.temporary.cleanup)

    def artifacts(self):
        for folder in ("bootloader", "partition_table"):
            (self.work / folder).mkdir(exist_ok=True)
        for name, content in {"esp32_dot.bin": b"\xe9fixture-app", "esp32_dot.elf": b"ELF fixture",
                "bootloader/bootloader.bin": b"\xe9fixture-bootloader", "partition_table/partition-table.bin": b"fixture-table",
                "ota_data_initial.bin": b"fixture-ota"}.items():
            (self.work / name).write_bytes(content)
        description = {"target": "esp32s3", "project_name": "esp32_dot", "project_version": "test",
            "app_elf": "esp32_dot.elf", "config_file": str(self.project / "sdkconfig")}
        (self.work / "project_description.json").write_text(json.dumps(description))
        flash = {"flash_settings": {"flash_size": "16MB"}, "extra_esptool_args": {"chip": "esp32s3"},
            "flash_files": {"0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                "0xd000": "ota_data_initial.bin", "0x20000": "esp32_dot.bin"}}
        (self.work / "flasher_args.json").write_text(json.dumps(flash))

    def test_complete_flat_bundle_and_verified_replacement(self):
        self.artifacts()
        manifest = build.publish_bundle(self.work, self.firmware, "5.2.3")
        self.assertEqual(manifest["target"], "esp32s3")
        self.assertEqual(manifest["flash_size"], 16777216)
        self.assertIn("sdkconfigcompiled.txt", manifest["files"])
        for name, checksum in manifest["files"].items():
            self.assertEqual(Path(name).name, name)
            self.assertEqual(build.sha256(self.firmware / name), checksum)
        flash = json.loads((self.firmware / "flasher_args.json").read_text())
        self.assertEqual(flash["flash_files"]["0x8000"], "partition-table.bin")
        (self.work / "esp32_dot.bin").write_bytes(b"\xe9new-fixture-app")
        build.publish_bundle(self.work, self.firmware, "5.2.3")
        self.assertEqual((self.firmware / "esp32_dot.bin").read_bytes(), b"\xe9new-fixture-app")
        self.assertFalse(list(self.work.glob("firmware-stage-*")))
        self.assertFalse(list(self.work.glob("firmware-previous-*")))

    def test_modified_output_is_preserved(self):
        self.artifacts()
        build.publish_bundle(self.work, self.firmware, "5.2.3")
        (self.firmware / "esp32_dot.bin").write_bytes(b"user-changed-file")
        with self.assertRaisesRegex(RuntimeError, "modified"):
            build.publish_bundle(self.work, self.firmware, "5.2.3")
        self.assertEqual((self.firmware / "esp32_dot.bin").read_bytes(), b"user-changed-file")

    def test_missing_or_escaping_artifact_publishes_nothing(self):
        self.artifacts()
        (self.work / "esp32_dot.elf").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing"):
            build.publish_bundle(self.work, self.firmware, "5.2.3")
        self.assertFalse(self.firmware.exists())
        self.artifacts()
        flash = json.loads((self.work / "flasher_args.json").read_text())
        flash["flash_files"]["0x0"] = "../outside.bin"
        (self.root / "outside.bin").write_bytes(b"outside")
        (self.work / "flasher_args.json").write_text(json.dumps(flash))
        with self.assertRaisesRegex(RuntimeError, "outside"):
            build.publish_bundle(self.work, self.firmware, "5.2.3")
        self.assertFalse(self.firmware.exists())

    def test_failed_build_does_not_publish_or_replace_bundle(self):
        self.artifacts()
        build.publish_bundle(self.work, self.firmware, "5.2.3")
        original = (self.firmware / "manifest.json").read_bytes()
        config = (self.root, self.root, self.work, self.firmware, "5.2.3", os.environ.copy(),
            [sys.executable, "-c", "raise SystemExit(7)"])
        with patch.object(build, "configuration", return_value=config):
            with self.assertRaisesRegex(RuntimeError, "exit 7"):
                build.main(["--quiet"])
        self.assertEqual((self.firmware / "manifest.json").read_bytes(), original)
        self.assertFalse((self.work / ".esp32-dot-build.lock").exists())

    def test_existing_build_lock_is_not_stolen(self):
        (self.work / ".esp32-dot-build.lock").write_text("123\n")
        with self.assertRaisesRegex(RuntimeError, "Another wrapper build"):
            with build.build_lock(self.work):
                self.fail("must not acquire another build's lock")
        self.assertTrue((self.work / ".esp32-dot-build.lock").exists())


if __name__ == "__main__":
    unittest.main()
