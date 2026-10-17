"""Flasher safety tests with a fake ESP32; never opens a real port."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout

spec = importlib.util.spec_from_file_location('dot_flash', Path(__file__).resolve().parents[1] / 'tools/flash.py')
flasher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flasher)


def table_bytes():
    entries = [struct.pack('<HBBII16sI', 0x50AA, 1, 0, 0xD000, 0x2000, b'otadata', 0),
               struct.pack('<HBBII16sI', 0x50AA, 0, 0x10, 0x20000, 0x3F0000, b'ota_0', 0)]
    data = b''.join(entries)
    return data + b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(data).digest() + b'\xff' * (4096 - len(data) - 32)


class FakeESP:
    IMAGE_CHIP_ID = 9

    def __init__(self):
        self.secure = False
        self.table = table_bytes()
        self.bad_md5 = False
        self.did_reset = False
        self.erased = False
        self.closed = False
        self._port = types.SimpleNamespace(close=lambda: setattr(self, 'closed', True))

    def connect(self, mode='no_reset'): assert mode in ('no_reset', 'default_reset')
    def get_security_info(self):
        return {'parsed_flags': {'SECURE_BOOT_EN': self.secure, 'SECURE_DOWNLOAD_ENABLE': False}, 'flash_crypt_cnt': 0}
    def get_chip_revision(self): return 0
    def run_stub(self): return self
    def change_baud(self, baud): pass
    def flash_id(self): return 24 << 16
    def read_flash(self, offset, length):
        if offset == 0x8000: return self.table
        if offset == 0: return b'\xff' * length
        if offset == 0xD000 and self.erased: return b'\xff' * length
        raise AssertionError('Unexpected read')
    def flash_md5sum(self, offset, length):
        return 'wrong' if self.bad_md5 else hashlib.md5(b'\xff' * length).hexdigest()
    def erase_region(self, offset, length):
        assert (offset, length) == (0xD000, 0x2000)
        self.erased = True
    def hard_reset(self): self.did_reset = True


class SafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.backup = self.folder / 'private'
        self.device = FakeESP()
        self.app = b'\xe9' + b'firmware' * 10
        (self.folder / 'esp32_dot.bin').write_bytes(self.app)
        (self.folder / 'partition-table.bin').write_bytes(table_bytes())
        self.manifest = {'target': 'esp32s3', 'flash_size': 0x1000000, 'files': {
            name: hashlib.sha256((self.folder / name).read_bytes()).hexdigest()
            for name in ['esp32_dot.bin', 'partition-table.bin']}}
        self.save_manifest()
        image = types.SimpleNamespace(chip_id=9, min_rev_full=0, max_rev_full=65535, verify=lambda: None)
        constructor = lambda *a, **kw: self.device
        constructor.IMAGE_CHIP_ID = 9
        self.modules = patch.dict(sys.modules, {
            'esptool': types.SimpleNamespace(ESP32S3ROM=constructor),
            'esptool.targets.esp32s3': types.SimpleNamespace(ESP32S3ROM=constructor),
            'esptool.bin_image': types.SimpleNamespace(LoadFirmwareImage=lambda *a: image)})
        self.modules.start()

    def tearDown(self):
        self.modules.stop()
        self.temp.cleanup()

    def save_manifest(self):
        (self.folder / 'manifest.json').write_text(json.dumps(self.manifest))

    def invoke(self):
        with redirect_stdout(io.StringIO()):
            flasher.flash(self.folder, 'FAKE_PORT', self.backup, self.app, flasher.partitions(table_bytes()))

    def test_partition_checksum(self):
        corrupted = bytearray(table_bytes()); corrupted[5] ^= 1
        with self.assertRaises(ValueError): flasher.partitions(corrupted)

    def test_artifact_hashes_and_filename_bounds(self):
        self.assertEqual(flasher.bundle_files(self.folder)[0], self.app)
        (self.folder / 'esp32_dot.bin').write_bytes(b'\xe9changed')
        with self.assertRaises(ValueError): flasher.bundle_files(self.folder)
        self.manifest['files']['../outside'] = 'x'; self.save_manifest()
        with self.assertRaises(ValueError): flasher.bundle_files(self.folder)

    def test_security_and_layout_fail_before_any_write(self):
        for field, value in [('secure', True), ('table', b'\x00' * 4096), ('bad_md5', True)]:
            with self.subTest(field=field), patch.object(flasher, 'write_app') as writer:
                self.device = FakeESP(); setattr(self.device, field, value)
                with self.assertRaises((RuntimeError, ValueError)): self.invoke()
                writer.assert_not_called()
                self.assertFalse(self.device.erased)
                self.assertTrue(self.device.did_reset)
                self.assertTrue(self.device.closed)
                self.assertFalse(self.backup.exists())

    def test_verified_backup_precedes_write_and_ota_reset(self):
        def writer(device, app):
            copies = list(self.backup.glob('*.bin'))
            self.assertEqual(len(copies), 1)
            self.assertEqual(copies[0].stat().st_size, 0x1000000)
            self.assertFalse(device.erased)
        with patch.object(flasher, 'write_app', side_effect=writer) as mock:
            self.invoke()
            mock.assert_called_once()
        self.assertTrue(self.device.erased)
        self.assertTrue(self.device.did_reset)

    def test_app_failure_does_not_erase_ota_or_reset(self):
        with patch.object(flasher, 'write_app', side_effect=RuntimeError('simulated write failure')):
            with self.assertRaises(RuntimeError): self.invoke()
        self.assertFalse(self.device.erased)
        self.assertFalse(self.device.did_reset)
        self.assertTrue(self.device.closed)

    def test_reused_backup_must_match_current_flash(self):
        backup = self.folder / 'saved.bin'
        backup.write_bytes(b'\xff' * 0x1000000)
        with patch.object(flasher, 'write_app') as writer, redirect_stdout(io.StringIO()):
            flasher.flash(self.folder, 'FAKE_PORT', self.backup, self.app,
                          flasher.partitions(table_bytes()), existing_backup=backup)
            writer.assert_called_once()
        self.assertFalse(self.backup.exists())
        self.device = FakeESP()
        backup.write_bytes(b'\x00' * 0x1000000)
        with patch.object(flasher, 'write_app') as writer, redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                flasher.flash(self.folder, 'FAKE_PORT', self.backup, self.app,
                              flasher.partitions(table_bytes()), existing_backup=backup)
            writer.assert_not_called()


if __name__ == '__main__':
    unittest.main()
