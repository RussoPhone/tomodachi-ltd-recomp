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


HOOKED_BLOCK = b"""void blk_main_000000000022dce0(GuestContext* c){
    { uint64_t _entry=c->pc-g_module_base; if(_entry!=0x22dce0ULL){ if(_entry<0x22dce0ULL){ recomp_unhandled(c,0U,c->pc); return; } switch((unsigned)((_entry-0x22dce0ULL)>>2)){
      case 1U: goto _recomp_entry_1;
      default: recomp_unhandled(c,0U,c->pc); return;
    } } }
    c->x[8]=1;
_recomp_entry_1: ;
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

    def test_hook_goes_after_the_dispatcher_and_only_once(self):
        once = slim.apply_hooks(HOOKED_BLOCK)
        self.assertEqual(once.count(b'nxrt_hook_pointer(c);'), 1)
        self.assertTrue(once.index(b'    } } }') < once.index(b'nxrt_hook_pointer(c);') < once.index(b'c->x[8]=1;'))
        self.assertTrue(once.startswith(b'__attribute__((weak)) void nxrt_hook_pointer(GuestContext* c){}'))
        old = once.replace(b'__attribute__((weak)) void nxrt_hook_pointer(GuestContext* c){}', b'void nxrt_hook_pointer(GuestContext* c);')
        self.assertEqual(slim.apply_hooks(old), once)
        self.assertEqual(slim.apply_hooks(once), once)
        other = BLOCK
        self.assertEqual(slim.apply_hooks(other), other)

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
