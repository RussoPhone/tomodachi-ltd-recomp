import importlib.util
import sys
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/slim-export.py'
spec = importlib.util.spec_from_file_location('slim_export', SCRIPT)
slim = importlib.util.module_from_spec(spec)
sys.modules['slim_export'] = slim
spec.loader.exec_module(slim)

BLOCK = b"""void blk_main_0000000001cdfd14(GuestContext* c){
    static const uint32_t _expected[]={3U,0x01cdfd14U,0x00000000U,0xb9400048U,};
    RECOMP_GG_GUARD(_expected,0U);
    c->x[8]=recomp_load32(c,c->x[2] + 0);
    c->pc=c->x[30]; return;
}
"""


class SlimExportTests(unittest.TestCase):
    def test_removes_arrays_and_guards_and_keeps_code(self):
        new, n, g = slim.slim_text(BLOCK * 3)
        self.assertEqual((n, g), (3, 3))
        self.assertNotIn(b'_expected', new)
        self.assertNotIn(b'RECOMP_GG_GUARD', new)
        self.assertEqual(new.count(b'c->x[8]=recomp_load32'), 3)

    def test_refuses_mismatched_counts(self):
        with self.assertRaises(ValueError):
            slim.slim_text(BLOCK + b'    RECOMP_GG_GUARD(_expected,9U);\n')

    def test_folder_is_idempotent(self):
        folder = Path(tempfile.mkdtemp())
        (folder / 'a.c').write_bytes(BLOCK)
        (folder / 'b.c').write_bytes(b'int x;\n')
        first = slim.slim_folder(folder, jobs=1)
        self.assertEqual(first[0], 2)
        self.assertLess(first[2], first[1])
        self.assertIsNone(slim.slim_folder(folder, jobs=1))
        self.assertEqual((folder / 'b.c').read_bytes(), b'int x;\n')


if __name__ == '__main__':
    unittest.main()
