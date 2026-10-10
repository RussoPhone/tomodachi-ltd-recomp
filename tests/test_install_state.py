import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('install_state_under_test', ROOT / 'scripts/install.py')
install = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(install)


class InstallStateTests(unittest.TestCase):
    def run_with_fixture(self, root, step):
        with mock.patch.object(install, 'ROOT', root), \
                mock.patch.object(install, 'STATE', root / 'local/state'), \
                mock.patch.object(install, 'STEPS', [('fake', 'Fake step', step)]), \
                mock.patch.object(install, 'CI', False), \
                mock.patch.object(install, 'refresh_after_update', return_value=(root / 'state.sha256', 'fingerprint')), \
                mock.patch.object(install, 'say'):
            install.main()

    def test_failed_marker_commit_does_not_make_an_incomplete_step_look_done(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            marker = root / 'local/state/01-fake.ok'
            step = mock.Mock()

            with mock.patch.object(install.os, 'replace', side_effect=OSError('simulated interrupted marker commit')):
                with self.assertRaisesRegex(OSError, 'simulated interrupted marker commit'):
                    self.run_with_fixture(root, step)

            self.assertFalse(marker.exists(), 'an uncommitted marker must not survive as completed state')

            self.run_with_fixture(root, step)
            self.assertEqual(step.call_count, 2, 'a restart must retry the step whose marker was not committed')

    def test_restart_skips_committed_steps_and_runs_only_the_next_step(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first, second = mock.Mock(), mock.Mock()
            steps = [('first', 'First step', first), ('second', 'Second step', second)]

            for _ in range(2):
                with mock.patch.object(install, 'ROOT', root), \
                        mock.patch.object(install, 'STATE', root / 'local/state'), \
                        mock.patch.object(install, 'STEPS', steps), \
                        mock.patch.object(install, 'CI', False), \
                        mock.patch.object(install, 'refresh_after_update', return_value=(root / 'state.sha256', 'fingerprint')), \
                        mock.patch.object(install, 'say'):
                    install.main()

            first.assert_called_once_with()
            second.assert_called_once_with()
            self.assertTrue((root / 'local/state/01-first.ok').is_file())
            self.assertTrue((root / 'local/state/02-second.ok').is_file())

    def test_changed_patches_invalidate_only_their_downstream_markers(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state, patches = root / 'local/state', root / 'patches'
            state.mkdir(parents=True)
            patches.mkdir()
            (patches / 'fix.patch').write_text('new patch contents')
            keys = ('check', 'choose', 'fetch', 'patches', 'backend', 'dump', 'export', 'package', 'install', 'bundle')
            with mock.patch.object(install, 'ROOT', root), mock.patch.object(install, 'STATE', state), \
                    mock.patch.object(install, 'STEPS', [(key, key, None) for key in keys]):
                for index, key in enumerate(keys, 1):
                    (state / f'{index:02d}-{key}.ok').touch()
                (state / 'patches.sha256').write_text('old fingerprint')

                install.refresh_after_update()

            removed = {'patches', 'backend', 'package', 'bundle'}
            for index, key in enumerate(keys, 1):
                self.assertEqual((state / f'{index:02d}-{key}.ok').exists(), key not in removed, key)

    def test_run_failure_exits_one_and_keeps_step_unmarked(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with mock.patch.object(install, 'ROOT', root), mock.patch.object(install, 'say'):
                with self.assertRaises(SystemExit) as caught:
                    install.run([sys.executable, '-c', 'print("fixture failure"); raise SystemExit(17)'], 'step.log')

            self.assertEqual(caught.exception.code, 1)
            self.assertIn('fixture failure', (root / 'local/logs/step.log').read_text())


if __name__ == '__main__':
    unittest.main()
