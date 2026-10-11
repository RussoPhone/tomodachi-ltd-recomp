import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
METER = ROOT / 'scripts/compile-meter.py'


def object_hash(path):
    return hashlib.sha256(path.read_bytes()).digest()


class CompileMeterCacheTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
    def test_source_path_change_invalidates_path_sensitive_object(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first_dir, second_dir = root / 'first', root / 'second'
            first_dir.mkdir()
            second_dir.mkdir()
            source_text = 'const char *source_name(void) { return __FILE__; }\n'
            first_source, second_source = first_dir / 'main.c', second_dir / 'main.c'
            first_source.write_text(source_text)
            second_source.write_text(source_text)

            env = dict(os.environ)
            env['COMPILE_OBJ_CACHE'] = str(root / 'cache')
            env['COMPILE_METER_LOG'] = str(root / 'meter.tsv')
            outputs = [root / 'first.o', root / 'second.o']
            for source, output in zip((first_source, second_source), outputs):
                subprocess.run([sys.executable, str(METER), shutil.which('cc'), '-c', str(source),
                                '-o', str(output)], check=True, env=env)

            self.assertNotEqual(outputs[0].read_bytes(), outputs[1].read_bytes(),
                                'cache reused an object with the wrong __FILE__ expansion')
            self.assertEqual([line.rsplit('\t', 1)[-1] for line in
                              (root / 'meter.tsv').read_text().splitlines()], ['miss', 'miss'])

    @unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
    def test_nested_header_change_invalidates_cached_object(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / 'project'
            nested = project / 'nested'
            nested.mkdir(parents=True)
            source = project / 'main.c'
            header = nested / 'value.h'
            source.write_text('#include "nested/value.h"\nint answer(void) { return VALUE; }\n')
            header.write_text('#define VALUE 11\n')

            env = dict(os.environ)
            env['COMPILE_OBJ_CACHE'] = str(root / 'cache')
            env['COMPILE_METER_LOG'] = str(root / 'meter.tsv')

            def compile_to(output):
                subprocess.run([sys.executable, str(METER), shutil.which('cc'),
                                f'-I{project}', '-c', str(source), '-o', str(output)],
                               check=True, env=env)

            first, second = root / 'first.o', root / 'second.o'
            compile_to(first)
            header.write_text('#define VALUE 22\n')
            compile_to(second)

            self.assertNotEqual(object_hash(first), object_hash(second),
                                'cache returned an object compiled with the old nested header')

    @unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
    def test_separate_include_flag_invalidates_cached_object(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / 'project'
            includes = root / 'includes'
            project.mkdir()
            includes.mkdir()
            source = project / 'main.c'
            header = includes / 'value.h'
            source.write_text('#include "value.h"\nint answer(void) { return VALUE; }\n')
            header.write_text('#define VALUE 11\n')

            env = dict(os.environ)
            env['COMPILE_OBJ_CACHE'] = str(root / 'cache')
            env['COMPILE_METER_LOG'] = str(root / 'meter.tsv')

            def compile_to(output):
                subprocess.run([sys.executable, str(METER), shutil.which('cc'), '-I', str(includes),
                                '-c', str(source), '-o', str(output)], check=True, env=env)

            first, second = root / 'first.o', root / 'second.o'
            compile_to(first)
            header.write_text('#define VALUE 22\n')
            compile_to(second)

            self.assertNotEqual(object_hash(first), object_hash(second),
                                'cache returned an object compiled with the old include header')

    @unittest.skipUnless(shutil.which('cc'), 'a C compiler is required')
    def test_cache_hit_depfile_tracks_nested_headers(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / 'project'
            nested = project / 'nested'
            nested.mkdir(parents=True)
            source = project / 'main.c'
            header = nested / 'value.h'
            depfile = root / 'main.d'
            source.write_text('#include "nested/value.h"\nint answer(void) { return VALUE; }\n')
            header.write_text('#define VALUE 11\n')

            env = dict(os.environ)
            env['COMPILE_OBJ_CACHE'] = str(root / 'cache')
            env['COMPILE_METER_LOG'] = str(root / 'meter.tsv')

            def compile_to(output):
                subprocess.run([sys.executable, str(METER), shutil.which('cc'), f'-I{project}', '-MMD', '-c',
                                str(source), '-o', str(output), '-MF', str(depfile)],
                               check=True, env=env)

            output = root / 'main.o'
            compile_to(output)
            depfile.unlink()
            compile_to(output)

            self.assertIn('nested/value.h', depfile.read_text(),
                          'cache hit omitted a nested header from the generated depfile')


if __name__ == '__main__':
    unittest.main()
