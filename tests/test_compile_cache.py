"""Compile-cache integration tests with a tiny C program, never game data."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


METER = Path(__file__).resolve().parents[1] / 'scripts/compile-meter.py'


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class CompileCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / 'src/main.c'
        self.src.parent.mkdir()
        self.inc = self.root / 'includes/nested/config.h'
        self.inc.parent.mkdir(parents=True)
        self.src.write_text('#include "nested/config.h"\nint sample(void) { return ANSWER; }\n')
        self.inc.write_text('#define ANSWER 10\n')
        self.obj = self.root / 'sample.o'
        self.log = self.root / 'compiles.tsv'
        self.env = dict(os.environ, COMPILE_OBJ_CACHE=str(self.root / 'cache'),
                        COMPILE_METER_LOG=str(self.log))

    def compile(self):
        subprocess.run([sys.executable, str(METER), shutil.which('cc'), '-c', str(self.src),
                        '-I', str(self.inc.parent.parent), '-o', str(self.obj)],
                       cwd=self.root, env=self.env, check=True, capture_output=True, text=True)
        return self.log.read_text().splitlines()[-1].split('\t')[-1], self.obj.read_bytes()

    def test_nested_header_change_misses_cache_and_rebuilds_object(self):
        first, old_obj = self.compile()
        second, cached_obj = self.compile()
        self.assertEqual((first, second), ('miss', 'hit'))
        self.assertEqual(old_obj, cached_obj)
        self.inc.write_text('#define ANSWER 20\n')
        third, new_obj = self.compile()
        self.assertEqual(third, 'miss')
        self.assertNotEqual(old_obj, new_obj)

    def test_header_next_to_source_change_misses_cache(self):
        self.src.write_text('#include "local.h"\nint sample(void) { return ANSWER; }\n')
        header = self.src.parent / 'local.h'
        header.write_text('#define ANSWER 30\n')
        self.assertEqual(self.compile()[0], 'miss')
        self.assertEqual(self.compile()[0], 'hit')
        header.write_text('#define ANSWER 40\n')
        self.assertEqual(self.compile()[0], 'miss')


if __name__ == '__main__':
    unittest.main()
