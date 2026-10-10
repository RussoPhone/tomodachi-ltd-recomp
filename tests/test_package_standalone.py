import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from unittest import mock


REPO = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = REPO / "scripts/package-standalone.py"


def load_packager():
    spec = importlib.util.spec_from_file_location("package_standalone", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class StandalonePackageTests(unittest.TestCase):
    def run_fixture(self, *args, build_rc=0, create_executable=False):
        packager = load_packager()
        with tempfile.TemporaryDirectory(prefix="standalone-package-") as tmp:
            root = pathlib.Path(tmp)
            export = root / "local/aot/export-hybrid/aot_cache"
            export.mkdir(parents=True)
            (root / "local/runtime.json").parent.mkdir(parents=True, exist_ok=True)
            (root / "local/runtime.json").write_text(json.dumps({"target": "fixture"}))
            adapter = root / "adapters/fixture/target.json"
            adapter.parent.mkdir(parents=True)
            adapter.write_text(json.dumps({"aot": {"backend": "hybrid"}}))
            (export / "aot_manifest.json").write_text(json.dumps({
                "modules": [{"name": "main"}],
                "image_features": 0,
                "image_abi": 4,
            }))
            (export / "exefs").mkdir()
            (root / "upstream/mk8-recomp/third_party/suyu").mkdir(parents=True)
            fake_cmake = root / "fake-cmake"
            fake_cmake.write_text(
                "#!/usr/bin/env python3\n"
                "import os, pathlib, sys\n"
                "if '--build' in sys.argv and os.environ.get('FIXTURE_CREATE_OUTPUT') == '1':\n"
                "    output = pathlib.Path(sys.argv[2]) / 'bin/suyu-cmd-static'\n"
                "    output.parent.mkdir(parents=True, exist_ok=True)\n"
                "    output.write_text('fixture executable')\n"
                "sys.exit(int(os.environ.get('FIXTURE_BUILD_RC', '0')) if '--build' in sys.argv else 0)\n"
            )
            fake_cmake.chmod(0o755)

            packager.ROOT = root
            packager.lab.ROOT = root
            packager.CMAKE = str(fake_cmake)
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(packager.sys, "argv", [str(SCRIPT), *args]), \
                    mock.patch.object(packager.shutil, "which", return_value=None), \
                    mock.patch.dict(packager.os.environ, {
                        "FIXTURE_BUILD_RC": str(build_rc),
                        "FIXTURE_CREATE_OUTPUT": "1" if create_executable else "0",
                    }), \
                    contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                try:
                    result = packager.main()
                except SystemExit as exc:
                    result = exc.code
        return result, stdout.getvalue(), stderr.getvalue()

    def test_successful_build_without_executable_is_failure(self):
        result, stdout, stderr = self.run_fixture()

        self.assertNotEqual(result, 0, stdout + stderr)
        self.assertIn("missing", stdout)
        self.assertIn("did not produce the executable", stderr)

    def test_build_failure_exit_code_is_preserved(self):
        result, stdout, _ = self.run_fixture(build_rc=7)

        self.assertEqual(result, 7, stdout)

    def test_successful_build_with_executable_succeeds(self):
        result, stdout, stderr = self.run_fixture(create_executable=True)

        self.assertEqual(result, 0, stdout + stderr)
        self.assertIn("suyu-cmd-static", stdout)
        self.assertNotIn("missing", stdout)

    def test_configure_only_does_not_require_executable(self):
        result, stdout, stderr = self.run_fixture("--configure-only")

        self.assertEqual(result, 0, stdout + stderr)
        self.assertIn("configured upstream/mk8-recomp/build/suyu-static", stdout)

    def test_negative_resource_limits_are_rejected(self):
        invalid_options = (
            (("--jobs", "-1"), "--jobs must be at least 1"),
            (("--recomp-jobs", "-1"), "--recomp-jobs must be at least 1"),
            (("--mem-budget-mb", "-1"), "--mem-budget-mb cannot be negative"),
        )
        for option, message in invalid_options:
            with self.subTest(option=option):
                result, _, stderr = self.run_fixture(*option, "--configure-only")
                self.assertEqual(result, 2, stderr)
                self.assertIn(message, stderr)


if __name__ == "__main__":
    unittest.main()
