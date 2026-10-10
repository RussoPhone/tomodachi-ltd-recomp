import importlib.util
from io import BytesIO
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'inventory_romfs_fixture', ROOT / 'scripts/inventory-romfs.py'
)
inventory = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inventory)


def pfs0_fixture(*, data_offset=0, data_size=1, name_offset=0, strtab=b'x\0', payload=b'x'):
    header = struct.pack('<4sIII', b'PFS0', 1, len(strtab), 0)
    entry = struct.pack('<QQI4x', data_offset, data_size, name_offset)
    return BytesIO(header + entry + strtab + payload)


class IdentityCipher:
    class _Decryptor:
        def update(self, data):
            return data

        def finalize(self):
            return b''

    def __init__(self, *args, **kwargs):
        pass

    def decryptor(self):
        return self._Decryptor()


class RomfsReader:
    def __init__(self, data):
        self.data = data
        self.length = len(data)

    def read(self, offset, size):
        return self.data[offset:offset + size]


def romfs_fixture(*, file_meta=None, dir_meta=None, file_meta_off=0x70,
                  file_meta_size=None, file_parent=0, data_off_override=None):
    if dir_meta is None:
        dir_meta = bytes(24)  # root directory metadata
    if file_meta is None:
        file_meta = struct.pack('<IIQQII', file_parent, 0, 0, 4, 0, 1) + b'a' + b'\0\0\0'
    if file_meta_size is None:
        file_meta_size = len(file_meta)
    dir_meta_off = 0x58
    data_off = file_meta_off + file_meta_size if data_off_override is None else data_off_override
    header = struct.pack('<11Q', 0x50, 0, 0, dir_meta_off, len(dir_meta),
                         0, 0, file_meta_off, file_meta_size, data_off, 4)
    image = bytearray(data_off + 4)
    image[:len(header)] = header
    image[dir_meta_off:dir_meta_off + len(dir_meta)] = dir_meta
    image[file_meta_off:file_meta_off + len(file_meta)] = file_meta
    image[data_off:data_off + 4] = b'DATA'
    return RomfsReader(image)


class Pfs0BoundsTests(unittest.TestCase):
    def test_rejects_data_extent_beyond_container(self):
        with self.assertRaises((ValueError, EOFError)):
            inventory.pfs0(pfs0_fixture(data_offset=100, data_size=20))

    def test_rejects_name_offset_outside_string_table(self):
        with self.assertRaises((ValueError, EOFError)):
            inventory.pfs0(pfs0_fixture(name_offset=2))

    def test_rejects_unterminated_name(self):
        with self.assertRaises((ValueError, EOFError)):
            inventory.pfs0(pfs0_fixture(strtab=b'xy'))

    def test_rejects_entry_table_that_exceeds_container(self):
        with self.assertRaises(ValueError):
            inventory.pfs0(BytesIO(struct.pack('<4sIII', b'PFS0', 0xFFFFFFFF, 0, 0)))


class CtrReaderBoundsTests(unittest.TestCase):
    def test_rejects_short_ciphertext_read(self):
        reader = inventory.CtrReader(BytesIO(b'1234567'), 0, bytes(16), bytes(8))
        with mock.patch.object(inventory, 'Cipher', IdentityCipher):
            with self.assertRaises(EOFError):
                reader.read(0, 8)

    def test_rejects_section_that_exceeds_nca(self):
        with self.assertRaises(ValueError):
            inventory.CtrReader(BytesIO(bytes(16)), 0, bytes(16), bytes(8),
                                nca_size=16, section_offset=0, section_size=32)

    def test_rejects_truncated_nca_header_before_decryption(self):
        with self.assertRaises(ValueError):
            inventory.read_nca_header(BytesIO(bytes(0xBFF)), 0, bytes(32), 'fixture.nca')


class RomfsMetadataBoundsTests(unittest.TestCase):
    def test_walks_a_minimal_valid_file(self):
        files = inventory.walk_romfs(romfs_fixture(), 0)
        self.assertEqual(files, [('/a', 0x94, 4)])

    def test_rejects_truncated_file_metadata_record(self):
        with self.assertRaises((ValueError, EOFError)):
            inventory.walk_romfs(romfs_fixture(file_meta=b'\0' * 31), 0)

    def test_rejects_parent_directory_outside_directory_table(self):
        with self.assertRaises((ValueError, EOFError)):
            inventory.walk_romfs(romfs_fixture(file_parent=0x1000), 0)

    def test_rejects_cyclic_directory_parent_chain(self):
        # A non-root directory at offset 24 that names itself as its parent.
        dir_meta = bytes(24) + struct.pack('<6I', 24, 0, 0, 0, 0, 1) + b'd' + b'\0\0\0'
        with self.assertRaises((ValueError, RecursionError)):
            inventory.walk_romfs(romfs_fixture(dir_meta=dir_meta, file_parent=24), 0)

    def test_rejects_romfs_metadata_table_outside_reader_extent(self):
        reader = romfs_fixture()
        header = list(struct.unpack('<11Q', reader.data[:0x58]))
        header[3] = reader.length + 1
        reader.data[:0x58] = struct.pack('<11Q', *header)
        with self.assertRaises(ValueError):
            inventory.walk_romfs(reader, 0)

    def test_rejects_file_data_outside_reader_extent(self):
        reader = romfs_fixture()
        reader.length -= 1
        with self.assertRaises(ValueError):
            inventory.walk_romfs(reader, 0)

    def test_rejects_misaligned_directory_parent_offset(self):
        with self.assertRaises(ValueError):
            inventory.walk_romfs(romfs_fixture(file_parent=1), 0)


class SafeOutputTests(unittest.TestCase):
    def test_target_traversal_is_rejected_before_key_lookup(self):
        with mock.patch.object(inventory.lab, 'target_name', return_value='../outside'), \
                mock.patch.object(inventory, 'keys', side_effect=AssertionError('keys should not be read')):
            with self.assertRaises(ValueError):
                inventory.main()

    def test_rejects_output_symlink_escaping_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'root'
            outside = Path(temp) / 'outside'
            root.mkdir()
            outside.mkdir()
            link = root / 'link.tsv'
            link.symlink_to(outside / 'target.tsv')
            with mock.patch.object(inventory, 'ROOT', root):
                with self.assertRaises(ValueError):
                    inventory.safe_output_path(link)

    def test_tsv_escaping_keeps_untrusted_names_on_one_row(self):
        self.assertEqual(inventory.tsv_escape('one\trow\nnext'), r'one\trow\nnext')


if __name__ == '__main__':
    unittest.main()
