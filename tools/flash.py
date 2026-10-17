"""Offline plan by default. Explicit --flash performs backup + compatible app write."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import struct
import sys

FLASH_SIZE = 0x1000000
APP_OFFSET = 0x20000
APP_SIZE = 0x3F0000
OTA_OFFSET = 0xD000
OTA_SIZE = 0x2000


def partitions(data):
    result = []
    for pos in range(0, min(len(data), 0xC00), 32):
        entry = data[pos:pos + 32]
        if len(entry) != 32:
            raise ValueError('Truncated partition table')
        magic = struct.unpack_from('<H', entry)[0]
        if magic == 0xEBEB:
            if entry[16:] != hashlib.md5(data[:pos]).digest():
                raise ValueError('Invalid partition table checksum')
            return result
        if magic != 0x50AA:
            raise ValueError('Missing partition table checksum or invalid entry')
        _, kind, subtype, offset, size, label, flags = struct.unpack('<HBBII16sI', entry)
        result.append((kind, subtype, offset, size, label.rstrip(b'\0'), flags))
    raise ValueError('Unterminated partition table')


def bundle_files(folder):
    manifest = json.loads((folder / 'manifest.json').read_text())
    if manifest.get('target') != 'esp32s3' or manifest.get('flash_size') != FLASH_SIZE:
        raise ValueError('Wrong firmware target or flash size')
    required = {'esp32_dot.bin', 'partition-table.bin'}
    listed = manifest.get('files', {})
    if not required.issubset(listed):
        raise ValueError('Incomplete firmware manifest')
    for name, checksum in listed.items():
        if Path(name).name != name or name in ('.', '..'):
            raise ValueError('Invalid manifest filename')
        if hashlib.sha256((folder / name).read_bytes()).hexdigest() != checksum:
            raise ValueError(f'Firmware checksum mismatch: {name}')
    app = (folder / 'esp32_dot.bin').read_bytes()
    if not app or len(app) > APP_SIZE or app[0] != 0xE9:
        raise ValueError('Invalid or oversized app image')
    table = partitions((folder / 'partition-table.bin').read_bytes())
    if (0, 0x10, APP_OFFSET, APP_SIZE, b'ota_0', 0) not in table:
        raise ValueError('Unexpected app partition')
    if (1, 0, OTA_OFFSET, OTA_SIZE, b'otadata', 0) not in table:
        raise ValueError('Unexpected OTA metadata partition')
    return app, table


def write_app(esp, data):
    padded = data + b'\xff' * (-len(data) % esp.FLASH_WRITE_SIZE)
    blocks = esp.flash_begin(len(padded), APP_OFFSET)
    for seq in range(blocks):
        block = padded[seq * esp.FLASH_WRITE_SIZE:(seq + 1) * esp.FLASH_WRITE_SIZE]
        esp.flash_block(block, seq)
        if seq % 32 == 0:
            print(f'Writing app: {seq * 100 // blocks}%')
    esp.flash_finish(reboot=False)
    if esp.read_flash(APP_OFFSET, len(data)) != data:
        raise RuntimeError('App readback verification failed; keep the backup')


def flash(folder, port, backup_dir, app, expected_table, before='no_reset', connection_baud=115200, existing_backup=None):
    import esptool
    from esptool.targets.esp32s3 import ESP32S3ROM
    from esptool.bin_image import LoadFirmwareImage
    image = LoadFirmwareImage('esp32s3', str(folder / 'esp32_dot.bin'))
    if image.chip_id != ESP32S3ROM.IMAGE_CHIP_ID:
        raise ValueError('App is not an ESP32-S3 image')
    image.verify()
    esp = ESP32S3ROM(port, baud=connection_baud)
    writes_started = False
    success = False
    try:
        esp.connect(mode=before)
        security = esp.get_security_info()
        if (security['parsed_flags']['SECURE_BOOT_EN'] or
                security['parsed_flags']['SECURE_DOWNLOAD_ENABLE'] or
                security['flash_crypt_cnt'].bit_count() % 2):
            raise RuntimeError('Protected device; this unsigned prototype flasher refuses to write')
        rev = esp.get_chip_revision()
        if not image.min_rev_full <= rev <= image.max_rev_full:
            raise RuntimeError('App does not support this chip revision')
        esp = esp.run_stub()
        esp.change_baud(460800)
        if (esp.flash_id() >> 16) & 0xFF != 24:
            raise RuntimeError('Expected exactly 16 MB physical flash')
        if partitions(esp.read_flash(0x8000, 0x1000)) != expected_table:
            raise RuntimeError('Existing partition layout differs. Nothing written')
        print('Verifying the full 16 MB backup before any flash write...')
        original = existing_backup.read_bytes() if existing_backup else esp.read_flash(0, FLASH_SIZE)
        if len(original) != FLASH_SIZE or esp.flash_md5sum(0, FLASH_SIZE).lower() != hashlib.md5(original).hexdigest():
            raise RuntimeError('Full backup read/checksum failed. Nothing written')
        backup = existing_backup
        if backup is None:
            backup_dir.mkdir(parents=True, exist_ok=True)
            stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
            backup = backup_dir / f'lantern-original-{stamp}.bin'
            with backup.open('xb') as stream:
                stream.write(original)
                stream.flush()
                import os
                os.fsync(stream.fileno())
        if hashlib.sha256(backup.read_bytes()).digest() != hashlib.sha256(original).digest():
            raise RuntimeError('Backup disk verification failed. Nothing written')
        if existing_backup is None:
            backup.with_suffix('.sha256').write_text(hashlib.sha256(original).hexdigest() + '  ' + backup.name + '\n')
        print(f'Backup verified: {backup}\nThis private file contains device credentials. Keep it private.')
        writes_started = True
        write_app(esp, app)
        # Erased OTA metadata selects ota_0 on this exact two-slot layout.
        esp.erase_region(OTA_OFFSET, OTA_SIZE)
        if esp.read_flash(OTA_OFFSET, OTA_SIZE) != b'\xff' * OTA_SIZE:
            raise RuntimeError('OTA selection reset verification failed; keep the backup')
        success = True
        print('App and OTA selection verified. Restarting ESP32-dot.')
    finally:
        if success or not writes_started:
            try:
                esp.hard_reset()
            except Exception:
                pass
        else:
            print('Write interrupted. Device left in loader mode. Restore the verified backup before reuse.', file=sys.stderr)
        esp._port.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM6')
    parser.add_argument('--firmware', type=Path, default=Path(__file__).resolve().parent.parent / 'firmware')
    parser.add_argument('--backup-dir', type=Path, required=True)
    parser.add_argument('--flash', action='store_true')
    parser.add_argument('--before', choices=['no_reset', 'default_reset'], default='no_reset')
    parser.add_argument('--connection-baud', type=int, choices=[115200, 460800], default=115200)
    parser.add_argument('--existing-backup', type=Path, help='Reuse only after its full checksum matches current physical flash')
    args = parser.parse_args()
    folder = args.firmware.resolve()
    app, table = bundle_files(folder)
    print(f'Validated {len(app)}-byte ESP32-S3 app; port {args.port}.')
    print('Plan: verify security/layout; back up 16 MB; replace ota_0 app; reset OTA selection.')
    print('Bootloader, partition table, model partition and NVS are preserved.')
    if not args.flash:
        print('Offline plan only. Add -Flash to the PowerShell command to access/write the device.')
        return
    flash(folder, args.port, args.backup_dir.resolve(), app, table,
          args.before, args.connection_baud, args.existing_backup.resolve() if args.existing_backup else None)


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'Flash stopped: {exc}', file=sys.stderr)
        sys.exit(1)
