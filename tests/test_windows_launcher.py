import tempfile
import unittest
from pathlib import Path

import importlib.util


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('assemble_package', ROOT / 'scripts/assemble-package.py')
assemble = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assemble)


class WindowsLauncherTests(unittest.TestCase):
    def write_launcher(self, package_dir, name):
        package_dir.mkdir(parents=True)
        assemble.write_windows_launcher(package_dir, name)
        return package_dir / 'run.ps1', package_dir / 'run.bat'

    def test_executable_name_is_literal_and_handles_spaces_and_apostrophes(self):
        name = "native $(throw 'owned')'s build"
        with tempfile.TemporaryDirectory() as temp:
            ps1, _ = self.write_launcher(Path(temp) / 'package with spaces', name)
            script = ps1.read_text()

        self.assertIn("$exe = Join-Path $PSScriptRoot 'native $(throw ''owned'')''s build.exe'", script)
        self.assertIn('& $exe @extra -g', script)
        self.assertNotIn(name, script)

    def test_moved_package_and_optional_arguments_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            ps1, bat = self.write_launcher(Path(temp) / 'package with spaces', 'native build')
            powershell = ps1.read_text()
            batch = bat.read_text()

        self.assertIn('Set-Location -LiteralPath $PSScriptRoot', powershell)
        self.assertIn("$ini = 'user\\config\\sdl2-config.ini'", powershell)
        self.assertIn("'--fullscreen' { $extra += '-f' }", powershell)
        self.assertIn("'--scale' {", powershell)
        for scale in assemble.SCALES:
            self.assertIn(f"'{scale}' = {assemble.SCALES[scale]}", powershell)
        self.assertIn("default { $extra += $argv[$i] }", powershell)
        self.assertIn('exit $LASTEXITCODE', powershell)
        self.assertIn('"%~dp0run.ps1" %*', batch)


if __name__ == '__main__':
    unittest.main()
