"""Check the upstream revision validator against a disposable local git repo."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/verify-upstream.py'
spec = importlib.util.spec_from_file_location('tested_upstream', SCRIPT)
upstream = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upstream)


@unittest.skipUnless(shutil.which('git'), 'git required')
class UpstreamLockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / 'upstream/mk8-recomp'
        self.repo.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)
        subprocess.run(['git', '-C', str(self.repo), '-c', 'user.name=Test',
                        '-c', 'user.email=test@example.invalid', 'commit', '--allow-empty',
                        '-q', '-m', 'empty'], check=True)
        self.commit = subprocess.check_output(['git', '-C', str(self.repo), 'rev-parse', 'HEAD'], text=True).strip()
        self.lock = self.root / 'upstream.lock.json'
        patcher = mock.patch.object(upstream, 'ROOT', self.root)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_matching_lock_without_submodules_is_accepted(self):
        self.lock.write_text(json.dumps({'commit': self.commit, 'submodules': []}))
        with contextlib.redirect_stdout(io.StringIO()):
            upstream.verify(self.lock)

    def test_commit_drift_is_rejected(self):
        self.lock.write_text(json.dumps({'commit': '0' * 40, 'submodules': []}))
        with self.assertRaisesRegex(ValueError, 'revision drift'):
            upstream.verify(self.lock)


if __name__ == '__main__':
    unittest.main()
